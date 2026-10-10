/*************************************************************************************************
*
* File: MIDI.cpp
*
* Description: Implements the MIDI objects
*
* Copyright (c) 2010 Brian Pantano. All rights reserved.
*
*************************************************************************************************/
#include "MIDI.h"
#include "ParallelMIDIPos.h"
#include "ParallelPostProcess.h"
#include "PackedMIDI.h"
#include <fstream>
#include <stack>
#include <array>
#include <exception>
#include <ppl.h>
#include <thread>
#include <memory>
#include <algorithm>
#include <intrin.h>
#include <smmintrin.h>
#include "lzma.h"
#include <psapi.h>
#pragma comment(lib, "Psapi.lib")

// std::map<int, std::pair<std::vector<MIDIEvent*>::iterator, std::vector<MIDIEvent*>>> midi_map;
MIDILoadingProgress g_LoadingProgress;

extern void PRE_DbgLog(const char* format, ...);
static ULONGLONG g_llLoadStageLast = 0;
static void LogLoadStage(const char* stage, const MIDI* pMidi)
{
    ULONGLONG llNow = GetTickCount64();
    if (g_llLoadStageLast == 0) g_llLoadStageLast = llNow;
    PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    size_t iPoolBytes = 0, iPoolCount = 0;
    if (pMidi)
    {
        iPoolBytes = pMidi->GetEventPoolBytes();
        iPoolCount = pMidi->GetEventPoolCount();
    }
    PRE_DbgLog("LOAD [%s] +%llums ws=%.1fMB peakWS=%.1fMB pool=%u/%zuMB",
        stage, llNow - g_llLoadStageLast,
        pmc.WorkingSetSize / 1048576.0, pmc.PeakWorkingSetSize / 1048576.0,
        (unsigned)iPoolCount, iPoolBytes / 1048576);
    g_llLoadStageLast = llNow;
}


MIDIPos::MIDIPos( MIDI &midi ) : m_MIDI( midi )
{
    m_iCurrTick = m_iCurrMicroSec = 0;

    size_t iTracks = m_MIDI.m_vTracks.size();
    size_t iTracksRounded = (iTracks + 8) & ~7; // Need to round up to 32 bytes, each int is 4 bytes
    m_pTrackTime = (int*)_aligned_malloc(iTracksRounded * sizeof(int), 32);
    for (size_t i = 0; i < iTracks; i++)
    {
        m_vRowPos.push_back(0);
        m_vThinPos.push_back(0);
        m_vMetaPos.push_back(0);
        PrimeTime(i);
    }
    for (size_t i = iTracks; i < iTracksRounded; i++)
        m_pTrackTime[i] = INT_MAX;

    if ( m_MIDI.m_Info.iDivision & 0x8000 )
    {
        int iFramesPerSec = -( ( m_MIDI.m_Info.iDivision | static_cast< int >( 0xFFFF0000 ) ) >> 8 ) * 100;
        if ( iFramesPerSec == 2900 ) iFramesPerSec = 2997;
        int iTicksPerFrame = m_MIDI.m_Info.iDivision & 0xFF;

        m_bIsStandard = false;
        m_iTicksPerBeat = m_iMicroSecsPerBeat = 0;
        m_iTicksPerSecond = iTicksPerFrame * iFramesPerSec;
    }
    else
    {
        m_bIsStandard = true;
        m_iTicksPerSecond = 0;
        m_iTicksPerBeat = m_MIDI.m_Info.iDivision;
        m_iMicroSecsPerBeat = 500000;
    }
}

void MIDIPos::PrimeTime( size_t iTrack )
{
    MIDITrack* pTrack = m_MIDI.m_vTracks[iTrack];
    int iRowT = INT_MAX, iThinT = INT_MAX;
    if ( m_vRowPos[iTrack] < pTrack->GetRowCount() )
        iRowT = (int)m_MIDI.GetEventTicks((MIDIChannelEvent)(pTrack->GetRowStart() + m_vRowPos[iTrack]));
    if ( m_vThinPos[iTrack] < pTrack->GetThinCount() )
        iThinT = (int)m_MIDI.GetEventTicks((MIDIChannelEvent)(m_MIDI.m_iFullRows + pTrack->GetThinStart() + m_vThinPos[iTrack]));
    int iMetaT = INT_MAX;
    if ( m_vMetaPos[iTrack] < pTrack->GetMetaCount() )
        iMetaT = pTrack->m_vMetas[m_vMetaPos[iTrack]]->GetAbsT();
    m_pTrackTime[iTrack] = min( min( iRowT, iThinT ), iMetaT );
}

MIDIPos::~MIDIPos() {
    if (m_pTrackTime)
        _aligned_free(m_pTrackTime);
}

#ifdef __AVX2__
size_t min_index_avx2(int32_t* array, size_t size) {
    const __m256i increment = _mm256_set1_epi32(8);
    __m256i indices = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    __m256i minindices = indices;
    __m256i minvalues = _mm256_loadu_si256((__m256i*)array);

    for (size_t i = 8; i < size; i += 8) {

        indices = _mm256_add_epi32(indices, increment);

        const __m256i values = _mm256_loadu_si256((__m256i*)(array + i));
        const __m256i lt = _mm256_cmpgt_epi32(minvalues, values);
        minindices = _mm256_blendv_epi8(minindices, indices, lt);
        minvalues = _mm256_min_epi32(values, minvalues);
    }

    int32_t values_array[8];
    uint32_t indices_array[8];

    _mm256_storeu_si256((__m256i*)values_array, minvalues);
    _mm256_storeu_si256((__m256i*)indices_array, minindices);

    size_t  minindex = indices_array[0];
    int32_t minvalue = values_array[0];
    for (int i = 1; i < 8; i++) {
        if (values_array[i] < minvalue) {
            minvalue = values_array[i];
            minindex = indices_array[i];
        }
        else if (values_array[i] == minvalue) {
            minindex = min(minindex, size_t(indices_array[i]));
        }
    }

    return minindex;
}
#else
size_t min_index_sse(int32_t* array, size_t size) {
    const __m128i increment = _mm_set1_epi32(4);
    __m128i indices = _mm_setr_epi32(0, 1, 2, 3);
    __m128i minindices = indices;
    __m128i minvalues = _mm_loadu_si128((__m128i*)array);

    for (size_t i = 4; i < size; i += 4) {

        indices = _mm_add_epi32(indices, increment);

        const __m128i values = _mm_loadu_si128((__m128i*)(array + i));
        const __m128i lt = _mm_cmplt_epi32(values, minvalues);
        minindices = _mm_blendv_epi8(minindices, indices, lt);
        minvalues = _mm_min_epi32(values, minvalues);
    }

    int32_t values_array[4];
    uint32_t indices_array[4];

    _mm_storeu_si128((__m128i*)values_array, minvalues);
    _mm_storeu_si128((__m128i*)indices_array, minindices);

    size_t  minindex = indices_array[0];
    int32_t minvalue = values_array[0];
    for (int i = 1; i < 4; i++) {
        if (values_array[i] < minvalue) {
            minvalue = values_array[i];
            minindex = indices_array[i];
        }
        else if (values_array[i] == minvalue) {
            minindex = min(minindex, size_t(indices_array[i]));
        }
    }

    return minindex;
}
#endif

int MIDIPos::GetNextEvent( int iMicroSecs, MIDIEvent **pOutMeta, MIDIChannelEvent *pOutRow )
{
    if ( !pOutMeta || !pOutRow ) return 0;
    *pOutMeta = NULL;
    *pOutRow = UINT32_MAX;

    size_t iTracks = m_vRowPos.size();
#ifdef __AVX2__
    int iMinPos = (int)min_index_avx2(m_pTrackTime, (iTracks + 8) & ~7);
#else
    int iMinPos = (int)min_index_sse(m_pTrackTime, (iTracks + 8) & ~7);
#endif
    if (m_pTrackTime[iMinPos] == INT_MAX)
        return 0;

    MIDITrack* pMinTrack = m_MIDI.m_vTracks[iMinPos];

    MIDIChannelEvent iRow = UINT32_MAX;
    if ( m_vRowPos[iMinPos] < pMinTrack->GetRowCount() )
        iRow = (MIDIChannelEvent)( pMinTrack->GetRowStart() + m_vRowPos[iMinPos] );
    MIDIChannelEvent iThin = UINT32_MAX;
    if ( m_vThinPos[iMinPos] < pMinTrack->GetThinCount() )
        iThin = (MIDIChannelEvent)( m_MIDI.m_iFullRows + pMinTrack->GetThinStart() + m_vThinPos[iMinPos] );
    MIDIEvent* pMetaEvent = NULL;
    if ( m_vMetaPos[iMinPos] < pMinTrack->GetMetaCount() )
        pMetaEvent = pMinTrack->m_vMetas[m_vMetaPos[iMinPos]];
    int iRowT = iRow != UINT32_MAX ? (int)m_MIDI.GetEventTicks( iRow ) : INT_MAX;
    int iThinT = iThin != UINT32_MAX ? (int)m_MIDI.GetEventTicks( iThin ) : INT_MAX;
    int iMetaT = pMetaEvent ? pMetaEvent->GetAbsT() : INT_MAX;
    bool bThinNext = iThinT < iRowT; // note-ons process before same-tick thin note-offs so note-on time is initialized first
    bool bMetaNext = iMetaT <= (bThinNext ? iThinT : iRowT);
    int iNextT = bMetaNext ? iMetaT : ( bThinNext ? iThinT : iRowT );

    int iMaxTickAllowed = m_iCurrTick;
    if (m_bIsStandard) {
        if (m_iMicroSecsPerBeat != 0)
            iMaxTickAllowed += (static_cast<long long>(m_iTicksPerBeat) * (m_iCurrMicroSec + iMicroSecs)) / m_iMicroSecsPerBeat;
    } else {
        iMaxTickAllowed += (static_cast<long long>(m_iTicksPerSecond) * (m_iCurrMicroSec + iMicroSecs)) / 1000000;
    }

    if ( iMicroSecs < 0 || iNextT <= iMaxTickAllowed )
    {
        int iSpan = iNextT - m_iCurrTick;
        if ( m_bIsStandard )
            iSpan = ( static_cast< long long >( m_iMicroSecsPerBeat ) * iSpan ) / m_iTicksPerBeat - m_iCurrMicroSec;
        else
            iSpan = ( 1000000LL * iSpan ) / m_iTicksPerSecond - m_iCurrMicroSec;
        m_iCurrTick = iNextT;
        m_iCurrMicroSec = 0;

        if ( bMetaNext )
        {
            m_vMetaPos[iMinPos]++;
            *pOutMeta = pMetaEvent;
            if ( pMetaEvent->GetEventType() == MIDIEvent::MetaEvent )
            {
                MIDIMetaEvent *pMeta = reinterpret_cast< MIDIMetaEvent* >( pMetaEvent );
                if ( pMeta->GetMetaEventType() == MIDIMetaEvent::SetTempo && pMeta->GetDataLen() == 3 )
                    MIDI::Parse24Bit ( pMeta->GetData(), 3, &m_iMicroSecsPerBeat );
            }
        }
        else if ( bThinNext )
        {
            m_vThinPos[iMinPos]++;
            *pOutRow = iThin;
        }
        else
        {
            m_vRowPos[iMinPos]++;
            *pOutRow = iRow;
        }

        PrimeTime( iMinPos );

        return iSpan;
    }
    else
    {
        if ( m_bIsStandard )
            m_iCurrMicroSec = iMicroSecs + m_iCurrMicroSec -
                              ( static_cast< long long >( m_iMicroSecsPerBeat ) * ( iMaxTickAllowed - m_iCurrTick ) ) / m_iTicksPerBeat;
        else
            m_iCurrMicroSec = iMicroSecs + m_iCurrMicroSec -
                              ( 1000000LL * ( iMaxTickAllowed - m_iCurrTick ) ) / m_iTicksPerSecond;
        m_iCurrTick = iMaxTickAllowed;
        return iMicroSecs;
    }
}

// PGFA_PACKED=1 forces packed mode, =0 disables it. Without it, builds made with PGFA_PACKED_DEFAULT pack
// every file of at least PGFA_PACKED_MIN_MB (default 100): smaller ones gain nothing and would lose their
// markers, lyrics and track names for no reason.
static bool PackedModeWanted( const wstring &sFile )
{
    char aEnv[16] = {};
    if ( GetEnvironmentVariableA( "PGFA_PACKED", aEnv, sizeof( aEnv ) ) > 0 )
        return aEnv[0] != '0';
#ifdef PGFA_PACKED_DEFAULT
    unsigned long long iMinMB = 100;
    if ( GetEnvironmentVariableA( "PGFA_PACKED_MIN_MB", aEnv, sizeof( aEnv ) ) > 0 )
        iMinMB = _strtoui64( aEnv, nullptr, 10 );
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if ( GetFileAttributesExW( sFile.c_str(), GetFileExInfoStandard, &fad ) )
    {
        const unsigned long long iBytes = ( (unsigned long long)fad.nFileSizeHigh << 32 ) | fad.nFileSizeLow;
        return iBytes >= iMinMB * 1048576ull;
    }
#endif
    return false;
}

bool MIDI::LoadPackedWindow( long long llStartUs, uint64_t iMaxNotes, long long llMinSpanUs )
{
    try
    {
        if ( !m_pPacked )
        {
            auto ps = std::make_shared< PGFAPacked::PackedSong >();
            g_LoadingProgress.stage = MIDILoadingProgress::Stage::ParseTracks;
            g_LoadingProgress.progress = 0;
            g_LoadingProgress.max = 1;
            if ( !PGFAPacked::PackFile( m_sPackedPath, *ps ) ) return false;
            m_pPacked = ps;
        }
        const PGFAPacked::PackedSong &ps = *m_pPacked;
        const uint32_t t0 = PGFAPacked::TickAtUs( ps, llStartUs );
        const uint32_t minTicks = PGFAPacked::TickAtUs( ps, llStartUs + llMinSpanUs ) - t0;
        const uint32_t t1 = PGFAPacked::WindowEnd( ps, t0, iMaxNotes, minTicks );
        PGFAPacked::SynthStats ss;
        std::vector< uint8_t > smf = PGFAPacked::SynthWindow( ps, t0, t1, &ss );
        PRE_DbgLog("PACKED window: ticks [%u,%u) = %.1fs..%.1fs, %llu notes, SMF %.1f MB, synth %.0f ms",
            t0, t1, PGFAPacked::UsAtTick( ps, t0 ) / 1e6, PGFAPacked::UsAtTick( ps, t1 ) / 1e6,
            (unsigned long long)ss.notes, smf.size() / 1048576.0, ss.ms );
        m_llPackedStartUs = PGFAPacked::UsAtTick( ps, t0 );
        m_llPackedEndUs = PGFAPacked::UsAtTick( ps, t1 );
        ParseMIDI( smf.data(), smf.size() );
        m_Info.sFilename = m_sPackedPath;
        // the slider, the time readout and the end-of-song check use the whole song, not the window
        m_llPackedSongEndUs = PGFAPacked::UsAtTick( ps, ps.endTick );
        if ( ps.firstNoteTick != 0xFFFFFFFFu ) m_Info.llFirstNote = PGFAPacked::UsAtTick( ps, ps.firstNoteTick );
        m_Info.llTotalMicroSecs = m_llPackedSongEndUs;
        m_bPackedPending = false;
        return IsValid();
    }
    catch ( const std::exception &e )
    {
        PRE_DbgLog("PACKED window failed: %s", e.what());
        clear();
        return false;
    }
}

void MIDI::ApplyPackedSongInfo()
{
    if ( !m_pPacked ) return;
    const PGFAPacked::PackedSong &ps = *m_pPacked;
    m_llPackedSongEndUs = PGFAPacked::UsAtTick( ps, ps.endTick );
    if ( ps.firstNoteTick != 0xFFFFFFFFu ) m_Info.llFirstNote = PGFAPacked::UsAtTick( ps, ps.firstNoteTick );
    m_Info.llTotalMicroSecs = m_llPackedSongEndUs;
}

void MIDI::ReleasePacked()
{
    m_pPacked.reset();
}


MIDI::MIDI ( const wstring &sFilename )
{
    if ( PackedModeWanted( sFilename ) || PGFAPacked::g_Cont.active )
    {
        // the song is packed and a window of it expanded by MainScreen (LoadPackedWindow)
        m_sPackedPath = sFilename;
        m_bPackedPending = true;
        return;
    }
    try
    {
        InitFromFile( sFilename );
    }
    catch ( const std::exception &e )
    {
        clear();
        PRE_DbgLog("MIDI file load failed: %s", e.what());
    }
}

void MIDI::InitFromFile( const wstring &sFilename )
{
    InitFromFileCore(sFilename);
    if (!IsValid())
    {
        ReleaseOwnedData();
        clear();
        Sleep(300);
        InitFromFileCore(sFilename);
    }
}

// Reads the whole file with permissive sharing (AV scanners / other apps often hold
// transient locks), retrying briefly on lock errors and tolerating short reads.
static unsigned char* ReadWholeFile( const wstring &sFilename, size_t &iSizeOut, bool &bMappedOut )
{
    iSizeOut = 0;
    bMappedOut = false;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    for ( int iAttempt = 0; iAttempt < 10; iAttempt++ )
    {
        hFile = CreateFileW( sFilename.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL );
        if ( hFile != INVALID_HANDLE_VALUE ) break;
        DWORD dwErr = GetLastError();
        PRE_DbgLog("ReadWholeFile: open failed err=%lu (attempt %d)", dwErr, iAttempt);
        if ( dwErr != ERROR_SHARING_VIOLATION && dwErr != ERROR_LOCK_VIOLATION && dwErr != ERROR_ACCESS_DENIED )
            break;
        Sleep( 100 );
    }
    if ( hFile == INVALID_HANDLE_VALUE ) return nullptr;

    LARGE_INTEGER liSize = {};
    if ( !GetFileSizeEx( hFile, &liSize ) || liSize.QuadPart <= 0 )
    {
        CloseHandle( hFile );
        return nullptr;
    }
    const size_t iSize = static_cast< size_t >( liSize.QuadPart );

    // Huge plain .mid files are mapped instead of copied: the pages are file-backed, so
    // they cost no private memory, can be trimmed from the working set while parsing, and
    // the OS can drop them under pressure. (Copy-on-write, so a stray write can't hit the
    // file.) .xz files are decompressed into a heap block as before, so they take the read path.
    constexpr size_t kMapMin = (size_t)256 << 20;
    if ( iSize >= kMapMin )
    {
        HANDLE hMap = CreateFileMappingW( hFile, NULL, PAGE_WRITECOPY, 0, 0, NULL );
        if ( hMap )
        {
            void *pView = MapViewOfFile( hMap, FILE_MAP_COPY, 0, 0, 0 );
            CloseHandle( hMap ); // the view keeps the mapping alive
            if ( pView )
            {
                constexpr uint8_t kXzMagic[] = { 0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00 };
                if ( memcmp( pView, kXzMagic, sizeof( kXzMagic ) ) != 0 )
                {
                    CloseHandle( hFile );
                    g_LoadingProgress.stage = MIDILoadingProgress::Stage::CopyToMem;
                    g_LoadingProgress.max = iSize;
                    // Page the file in sequentially, one window ahead of a touch pass. Left to the parser, the
                    // worker threads would fault pages in at random offsets, which is crippling on a spinning
                    // disk or network share (a cold 300 MB file went from 2 s to 17 s).
                    {
                        typedef BOOL ( WINAPI *PrefetchFn )( HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG );
                        PrefetchFn pPrefetch = (PrefetchFn)GetProcAddress( GetModuleHandleW( L"kernel32.dll" ), "PrefetchVirtualMemory" );
                        const size_t kWin = (size_t)64 << 20;
                        auto prefetch = [&]( size_t off ) {
                            if ( !pPrefetch || off >= iSize ) return;
                            WIN32_MEMORY_RANGE_ENTRY r = { (unsigned char*)pView + off, (std::min)( kWin, iSize - off ) };
                            pPrefetch( GetCurrentProcess(), 1, &r, 0 );
                        };
                        prefetch( 0 );
                        volatile unsigned char sink = 0;
                        g_LoadingProgress.progress = 0;
                        for ( size_t off = 0; off < iSize; off += kWin )
                        {
                            prefetch( off + kWin );
                            const size_t end = (std::min)( off + kWin, iSize );
                            for ( size_t p = off; p < end; p += 4096 )
                                sink = sink + ( (volatile unsigned char*)pView )[p];
                            g_LoadingProgress.progress = end;
                        }
                    }
                    iSizeOut = iSize;
                    bMappedOut = true;
                    PRE_DbgLog("LOAD [ReadWholeFile mapped %zu bytes]", iSize);
                    return static_cast< unsigned char* >( pView );
                }
                UnmapViewOfFile( pView );
            }
        }
        PRE_DbgLog("ReadWholeFile: mapping unavailable/xz, reading %zu bytes", iSize);
    }

    unsigned char *pcBuf = nullptr;
    try { pcBuf = new unsigned char[iSize]; }
    catch ( const std::bad_alloc & )
    {
        PRE_DbgLog("ReadWholeFile: out of memory for %zu bytes", iSize);
        CloseHandle( hFile );
        return nullptr;
    }

    g_LoadingProgress.stage = MIDILoadingProgress::Stage::CopyToMem;
    g_LoadingProgress.progress = 0;
    g_LoadingProgress.max = iSize;

    size_t iRead = 0;
    while ( iRead < iSize )
    {
        DWORD dwWant = (DWORD)( std::min )( iSize - iRead, (size_t)( 16u << 20 ) ), dwGot = 0;
        if ( !ReadFile( hFile, pcBuf + iRead, dwWant, &dwGot, NULL ) || dwGot == 0 )
            break;
        iRead += dwGot;
        g_LoadingProgress.progress = iRead;
    }
    CloseHandle( hFile );

    if ( iRead != iSize )
        PRE_DbgLog("ReadWholeFile: short read %zu/%zu", iRead, iSize);
    if ( iRead == 0 )
    {
        delete[] pcBuf;
        return nullptr;
    }
    iSizeOut = iRead;
    return pcBuf;
}

void MIDI::InitFromFileCore( const wstring &sFilename )
{
    size_t iSize = 0;
    bool bMapped = false;
    unsigned char* pcMemBlock = ReadWholeFile(sFilename, iSize, bMapped);
    PRE_DbgLog("InitFromFileCore: read '%ls' -> %p (%zu bytes)", sFilename.c_str(), (void*)pcMemBlock, iSize);
    if (pcMemBlock)
    {
        constexpr uint8_t lzma_magic[] = {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00};
        bool bIsLzma = iSize >= sizeof(lzma_magic) && !memcmp(pcMemBlock, lzma_magic, sizeof(lzma_magic));

        if (bIsLzma)
        {
        while (iSize >= LZMA_STREAM_HEADER_SIZE * 2 && !memcmp(pcMemBlock, lzma_magic, sizeof(lzma_magic))) {
            auto compressed = pcMemBlock;

            // Get the decompressed size This is a real pain in the ass for concatenated .xz files, lots of sanity checking is skipped here See https://github.com/kobolabs/liblzma/blob/master/src/xz/list.c
            char err[1024] = {};
            uint64_t decompressed_size = 0;
            lzma_stream strm = LZMA_STREAM_INIT;
            lzma_stream_flags stream_flags;
            lzma_index* index = nullptr;
            auto pos = (int64_t)iSize;
            lzma_ret ret;
            do {
                // Position sanity check
                if (pos < LZMA_STREAM_HEADER_SIZE * 2) {
                    MessageBoxA(NULL, "Position sanity check failed. Corrupted file?", "Piano From Above", MB_OK | MB_ICONERROR);
                    lzma_index_end(index, NULL);
                    delete[] pcMemBlock;
                    return;
                }
                pos -= LZMA_STREAM_HEADER_SIZE;

                // Locate and decode stream footer
                uint64_t footer_pos;
                while (true) {
                    if (pos < LZMA_STREAM_HEADER_SIZE) {
                        MessageBoxA(NULL, "Locating stream footer failed. Corrupted file?", "Piano From Above", MB_OK | MB_ICONERROR);
                        lzma_index_end(index, NULL);
                        delete[] pcMemBlock;
                        return;
                    }
                    footer_pos = pos;

                    int i = 2;
                    if (*(uint32_t*)&compressed[footer_pos + 8] != 0)
                        break;

                    do {
                        pos -= 4;
                        --i;
                    } while (i >= 0 && *(uint32_t*)&compressed[footer_pos + i * 4] == 0);
                }
                ret = lzma_stream_footer_decode(&stream_flags, &compressed[footer_pos]);
                if (ret != LZMA_OK) {
                    snprintf(err, sizeof(err) - 1, "Decoding stream footer failed: %d\nCorrupt file?", ret);
                    MessageBoxA(NULL, err, "Piano From Above", MB_OK | MB_ICONERROR);
                    lzma_index_end(index, NULL);
                    delete[] pcMemBlock;
                    return;
                }
                if ((lzma_vli)pos < stream_flags.backward_size + LZMA_STREAM_HEADER_SIZE) {
                    MessageBoxA(NULL, "Stream footer position sanity check failed. Corrupted file?", "Piano From Above", MB_OK | MB_ICONERROR);
                    lzma_index_end(index, NULL);
                    delete[] pcMemBlock;
                    return;
                }

                // Decode index
                pos -= stream_flags.backward_size;
                lzma_index_decoder(&strm, &index, UINT64_MAX);
                strm.avail_in = stream_flags.backward_size;
                strm.next_in = &compressed[pos];
                pos += stream_flags.backward_size;
                ret = lzma_code(&strm, LZMA_RUN);
                if (ret != LZMA_STREAM_END) {
                    snprintf(err, sizeof(err) - 1, "Index decode failed: %d\nCorrupt file?", ret);
                    MessageBoxA(NULL, err, "Piano From Above", MB_OK | MB_ICONERROR);
                    lzma_index_end(index, NULL);
                    delete[] pcMemBlock;
                    return;
                }
                pos -= stream_flags.backward_size + LZMA_STREAM_HEADER_SIZE;
                if ((lzma_vli)pos < lzma_index_total_size(index)) {
                    MessageBoxA(NULL, "Index position sanity check failed. Corrupted file?", "Piano From Above", MB_OK | MB_ICONERROR);
                    lzma_index_end(index, NULL);
                    delete[] pcMemBlock;
                    return;
                }
                pos -= lzma_index_total_size(index);
                decompressed_size += lzma_index_uncompressed_size(index);
            } while (pos > 0);

            // Initialize progress
            g_LoadingProgress.stage = MIDILoadingProgress::Stage::Decompress;
            g_LoadingProgress.progress = 0;
            g_LoadingProgress.max = decompressed_size;
            
            // Decompress it
            pcMemBlock = new unsigned char[decompressed_size];
            uint8_t* write_ptr = pcMemBlock;
            lzma_end(&strm);
            strm = LZMA_STREAM_INIT;
            // Multi-block .xz files (made with xz -T / 7-Zip multithreading) decode on several cores;
            // single-block files simply run on one. Fall back to the plain decoder if the library
            // refuses the threaded one.
            {
                lzma_mt mt = {};
                mt.flags = LZMA_CONCATENATED;
                mt.threads = (std::max)(1u, (std::min)(std::thread::hardware_concurrency(), 16u));
                mt.memlimit_threading = (uint64_t)2 << 30; // soft: the decoder uses fewer threads rather than more memory
                mt.memlimit_stop = UINT64_MAX;
                if (lzma_stream_decoder_mt(&strm, &mt) != LZMA_OK) {
                    lzma_end(&strm);
                    strm = LZMA_STREAM_INIT;
                    lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED);
                }
            }
            strm.next_in = compressed;
            strm.avail_in = iSize;
            bool done = false;
            lzma_action action = LZMA_RUN;
            // Hand the decoder output space before the first call, and top it up in big pieces.
            const size_t kOutChunk = (size_t)8 << 20;
            size_t written = 0;
            auto refill = [&]() {
                size_t remaining = (std::min)((size_t)decompressed_size - written, kOutChunk);
                strm.next_out = pcMemBlock + written;
                strm.avail_out = remaining;
                written += remaining;
            };
            refill();
            while (!done) {
                if (strm.avail_in == 0)
                    action = LZMA_FINISH;
                lzma_ret ret = lzma_code(&strm, action);
                g_LoadingProgress.progress = written - strm.avail_out;
                if (strm.avail_out == 0 && written < decompressed_size)
                    refill();
                switch (ret) {
                case LZMA_STREAM_END:
                    done = true;
                    break;
                case LZMA_OK:
                    break;
                case LZMA_MEM_ERROR:
                    MessageBoxA(NULL, "Ran out of memory while decompressing.", "Piano From Above", MB_OK | MB_ICONERROR);
                    delete[] compressed;
                    delete[] pcMemBlock;
                    return;
                default:
                    snprintf(err, sizeof(err) - 1, "An error occurred while decompressing: %d\nCorrupt file?", ret);
                    MessageBoxA(NULL, err, "Piano From Above", MB_OK | MB_ICONERROR);
                    delete[] compressed;
                    delete[] pcMemBlock;
                    return;
                }
            }
            iSize = decompressed_size;
            delete[] compressed;
        }
        }

        m_pcOwnedData = pcMemBlock;
        m_bOwnedMapped = bMapped;
        ParseMIDI(pcMemBlock, iSize);
        m_Info.sFilename = sFilename;

        // heap blocks; if the parse didn't already take ownership and free it
        ReleaseOwnedData();
    }
}

MIDI::~MIDI( void )
{
    ReleaseOwnedData();
    clear();
}

MIDIChannelEvent MIDI::AppendChannelEvent( int iTrack, uint32_t iAbsTicks )
{
    m_vTimes.push_back( 0 );
    m_vTicks.push_back( iAbsTicks );
    m_vLengths.push_back( 0 );
    m_vSisters.push_back( UINT32_MAX );
    m_vSimult.push_back( 0 );
    m_vEventTrack.push_back( static_cast< uint16_t >( iTrack ) );
    m_vPack.push_back( 0 );
    return static_cast< MIDIChannelEvent >( m_vTicks.size() - 1 + m_vThinOwners.size() );
}

size_t MIDI::GetEventPoolBytes() const
{
    return m_vTimes.size() * sizeof( int64_t ) +
           m_vTicks.size() * sizeof( uint32_t ) +
           m_vLengths.size() * sizeof( uint32_t ) +
           m_vSisters.size() * sizeof( uint32_t ) +
           m_vSimult.size() * sizeof( uint16_t ) +
           m_vEventTrack.size() * sizeof( uint16_t ) +
           m_vPack.size() * sizeof( uint32_t ) +
           m_vThinOwners.size() * ( 2 * sizeof( uint32_t ) + ( m_bThinWide ? sizeof( uint32_t ) : sizeof( uint16_t ) ) ) + m_vThinEsc.size() * sizeof( ThinEsc );
}

size_t MIDI::GetEventPoolCount() const
{
    return m_vTicks.size() + m_vThinOwners.size();
}


const wstring MIDI::Instruments[129] =
{
    L"Acoustic Grand Piano", L"Bright Acoustic Piano", L"Electric Grand Piano", L"Honky-tonk Piano", L"Electric Piano 1", 
    L"Electric Piano 2", L"Harpsichord", L"Clavi", L"Celesta", L"Glockenspiel", 
    L"Music Box", L"Vibraphone", L"Marimba", L"Xylophone", L"Tubular Bells", 
    L"Dulcimer", L"Drawbar Organ", L"Percussive Organ", L"Rock Organ", L"Church Organ", 
    L"Reed Organ", L"Accordion", L"Harmonica", L"Tango Accordion", L"Acoustic Guitar (nylon)", 
    L"Acoustic Guitar (steel)", L"Electric Guitar (jazz)", L"Electric Guitar (clean)", L"Electric Guitar (muted)", L"Overdriven Guitar", 
    L"Distortion Guitar", L"Guitar harmonics", L"Acoustic Bass", L"Electric Bass (finger)", L"Electric Bass (pick)", 
    L"Fretless Bass", L"Slap Bass 1", L"Slap Bass 2", L"Synth Bass 1", L"Synth Bass 2", 
    L"Violin", L"Viola", L"Cello", L"Contrabass", L"Tremolo Strings", 
    L"Pizzicato Strings", L"Orchestral Harp", L"Timpani", L"String Ensemble 1", L"String Ensemble 2", 
    L"SynthStrings 1", L"SynthStrings 2", L"Choir Aahs", L"Voice Oohs", L"Synth Voice", 
    L"Orchestra Hit", L"Trumpet", L"Trombone", L"Tuba", L"Muted Trumpet", 
    L"French Horn", L"Brass Section", L"SynthBrass 1", L"SynthBrass 2", L"Soprano Sax", 
    L"Alto Sax", L"Tenor Sax", L"Baritone Sax", L"Oboe", L"English Horn", 
    L"Bassoon", L"Clarinet", L"Piccolo", L"Flute", L"Recorder", 
    L"Pan Flute", L"Blown Bottle", L"Shakuhachi", L"Whistle", L"Ocarina", 
    L"Lead 1 (square)", L"Lead 2 (sawtooth)", L"Lead 3 (calliope)", L"Lead 4 (chiff)", L"Lead 5 (charang)", 
    L"Lead 6 (voice)", L"Lead 7 (fifths)", L"Lead 8 (bass + lead)", L"Pad 1 (new age)", L"Pad 2 (warm)", 
    L"Pad 3 (polysynth)", L"Pad 4 (choir)", L"Pad 5 (bowed)", L"Pad 6 (metallic)", L"Pad 7 (halo)", 
    L"Pad 8 (sweep)", L"FX 1 (rain)", L"FX 2 (soundtrack)", L"FX 3 (crystal)", L"FX 4 (atmosphere)", 
    L"FX 5 (brightness)", L"FX 6 (goblins)", L"FX 7 (echoes)", L"FX 8 (sci-fi)", L"Sitar", 
    L"Banjo", L"Shamisen", L"Koto", L"Kalimba", L"Bag pipe", 
    L"Fiddle", L"Shanai", L"Tinkle Bell", L"Agogo", L"Steel Drums", 
    L"Woodblock", L"Taiko Drum", L"Melodic Tom", L"Synth Drum", L"Reverse Cymbal", 
    L"Guitar Fret Noise", L"Breath Noise", L"Seashore", L"Bird Tweet", L"Telephone Ring",
    L"Helicopter", L"Applause", L"Gunshot", L"Various"
};

const wstring &MIDI::NoteName( int iNote )
{
    InitArrays();
    if ( iNote < 0 || iNote >= MIDI::KEYS ) return aNoteNames[MIDI::KEYS];
    return aNoteNames[iNote];
}

MIDI::Note MIDI::NoteVal( int iNote )
{
    InitArrays();
    if ( iNote < 0 || iNote >= MIDI::KEYS ) return C;
    return aNoteVal[iNote];
}

bool MIDI::IsSharp( int iNote )
{
    /*
    InitArrays();
    if ( iNote < 0 || iNote >= MIDI::KEYS ) return false;
    return aIsSharp[iNote];
    */
    return (1 << (iNote % 12)) & 0b010101001010;
}

// Number of white keys in [iMinNote, iMaxNote)
int MIDI::WhiteCount( int iMinNote, int iMaxNote )
{
    InitArrays();
    if ( iMinNote < 0 || iMinNote >= MIDI::KEYS || iMaxNote < 0 || iMaxNote >= MIDI::KEYS ) return false;
    return aWhiteCount[iMaxNote] - aWhiteCount[iMinNote];
}

wstring MIDI::aNoteNames[MIDI::KEYS + 1];
MIDI::Note MIDI::aNoteVal[MIDI::KEYS];
bool MIDI::aIsSharp[MIDI::KEYS];
int MIDI::aWhiteCount[MIDI::KEYS + 1];

void MIDI::InitArrays()
{
    static bool bValid = false;

    // Build the array of note names upon first call
    if ( !bValid )
    {
        wchar_t buf[10];
        wchar_t cNote = L'C';
        int iOctave = -1;
        bool bIsSharp = false;
        MIDI::Note eNote = MIDI::C;
        for ( int i = 0; i < MIDI::KEYS; i++ )
        {
            // Don't want sprintf because we're in c++ and string building is too slow. Manual construction!
            int iPos = 0;
            buf[iPos++] = cNote;
            if ( bIsSharp ) buf[iPos++] = L'#';
            if ( iOctave < 0 ) buf[iPos++] = L'-';
            buf[iPos++] = L'0' + abs( iOctave );
            buf[iPos++] = L'\0';

            aNoteNames[i] = buf;
            aNoteVal[i] = eNote;
            aIsSharp[i] = bIsSharp;

            // Advance counters
            if ( eNote == MIDI::B || eNote == MIDI::E || bIsSharp )
                cNote++;
            if ( eNote != MIDI::B && eNote != MIDI::E )
                bIsSharp = !bIsSharp;
            if ( eNote == MIDI::B )
                iOctave++;
            if ( eNote == MIDI::GS )
            {
                cNote = 'A';
                eNote = MIDI::A;
            }
            else
                eNote = static_cast< MIDI::Note >( eNote + 1 );
        }
        aWhiteCount[0] = 0;
        for ( int i = 1; i < MIDI::KEYS + 1; i++ )
            aWhiteCount[i] = aWhiteCount[i - 1] + !aIsSharp[i - 1];
        aNoteNames[MIDI::KEYS] = L"Invalid";
        bValid = true;
    }
}

void MIDI::clear( void )
{
    for ( vector< MIDITrack* >::iterator it = m_vTracks.begin(); it != m_vTracks.end(); ++it )
        delete *it;
    m_vTracks.clear();
    m_Info.clear();
    PoolVec<int64_t>().swap( m_vTimes );
    PoolVec<uint32_t>().swap( m_vTicks );
    PoolVec<uint32_t>().swap( m_vLengths );
    PoolVec<uint32_t>().swap( m_vSisters );
    PoolVec<uint16_t>().swap( m_vSimult );
    PoolVec<uint16_t>().swap( m_vEventTrack );
    PoolVec<uint32_t>().swap( m_vPack );
    PoolVec<uint32_t>().swap( m_vThinTicks );
    PoolVec<uint16_t>().swap( m_vThinDelta );
    PoolVec<ThinEsc>().swap( m_vThinEsc );
    m_bThinWide = false;
    PoolVec<uint32_t>().swap( m_vThinOwners );
    PoolVec<uint32_t>().swap( m_vThinSisters );
    m_iFullRows = 0;
    m_bComputedTimes = false;
    m_bTimeStandard = true;
    m_iTimeTicksPerBeat = m_iTimeTicksPerSecond = 0;
    SetTimeDivisor( 0 );
    SetTimeAnchors( 0, nullptr, nullptr, nullptr );
}

void MIDI::ReleaseOwnedData( void )
{
    if ( m_pcOwnedData )
    {
        if ( m_bOwnedMapped )
            UnmapViewOfFile( m_pcOwnedData );
        else
            delete[] m_pcOwnedData;
        m_pcOwnedData = nullptr;
    }
    m_bOwnedMapped = false;
}

size_t MIDI::ParseMIDI( const unsigned char *pcData, size_t iMaxSize )
{
    try
    {
        return ParseMIDICore( pcData, iMaxSize );
    }
    catch ( const std::exception &e )
    {
        clear();
        PRE_DbgLog("ParseMIDI failed: %s", e.what());
        return 0;
    }
}

size_t MIDI::ParseMIDICore( const unsigned char *pcData, size_t iMaxSize )
{
    g_llLoadStageLast = 0;
    LogLoadStage("ParseMIDI begin", this);
    char pcBuf[4];    size_t iTotal;
    uint32_t iHdrSize;

    clear();

    if ( ParseNChars( pcData, 4, iMaxSize, pcBuf ) != 4 ) return 0;
    if ( Parse32Bit( pcData + 4, iMaxSize - 4, &iHdrSize) != 4 ) return 0;
    iTotal = 8;

    if ( strncmp( pcBuf, "MThd", 4 ) != 0 ) return 0;
    iHdrSize = max( iHdrSize, 6 ); // Allowing a bad header size. Some people ignore and hard code 6.
    
    iTotal += Parse16Bit( pcData + iTotal, iMaxSize - iTotal, &m_Info.iFormatType );
    iTotal += Parse16Bit( pcData + iTotal, iMaxSize - iTotal, &m_Info.iNumTracks );
    iTotal += Parse16Bit( pcData + iTotal, iMaxSize - iTotal, &m_Info.iDivision );

    if ( iTotal != 14 || m_Info.iFormatType < 0 || m_Info.iFormatType > 2 || m_Info.iDivision == 0 ) return 0;

    iTotal += iHdrSize - 6;
    if ( iTotal > iMaxSize ) return 0; // bogus header size; also keeps (iMaxSize - iTotal) from wrapping
    size_t iRet = iTotal + ParseTracks( pcData + iTotal, iMaxSize - iTotal );
    // bytes; free the owned copy now so the merged list doesn't share the load
    ReleaseOwnedData();
    LogLoadStage("ParseTracks end", this);
    return iRet;
}

namespace
{
    // Pending note-on stacks for one (channel, key) pair each. The previous
    // array<array<std::stack<..>>> default-constructed 2048 deques (one heap block each)
    // per track per phase, which dominated load time on files with many tracks. These
    // vectors allocate lazily and are reused across tracks by a worker thread.
    // PGFA_NO_DEDUPE=1 disables duplicate folding (for testing).
    bool g_bNoDedupe = GetEnvironmentVariableA( "PGFA_NO_DEDUPE", nullptr, 0 ) > 0;

    // One note-on waiting for its note-off.
    //
    // Duplicate folding: identical notes (same track, channel, key, velocity, start tick and end tick)
    // are stored as one row that carries a repeat count, and the player re-sends the note that many
    // times. A note F that was pushed directly on top of an identical-start note E is folded into E
    // when both end on the same tick. The counting pass decides this (it sees the note-offs) and
    // records it as a bit per note; the write pass replays the same stacks and obeys the bits.
    struct Pending
    {
        uint32_t tick;          // start tick
        uint32_t seq;           // index among this track's note-ons
        uint32_t rowId;         // write pass: pool row this note is stored in (its folded-into row if dup)
        uint32_t childSeq;      // counting pass: the last same-start note popped off directly above this one
        uint32_t childOffTick;
        uint8_t vel;
        uint8_t key;            // full data byte (stacks are indexed by key & 0x7F, so malformed >=128 keys can share one)
        uint8_t run;            // notes represented by this entry (itself + folded ones above it)
        uint8_t childRun;
        bool sameAsPred;        // pushed on an entry with the same start tick and velocity
        bool childValid;
        bool dup;               // write pass: folded into an earlier row, owns no row of its own
    };

    struct NoteStacks
    {
        vector< Pending > v[ 16 * 128 ];
        uint16_t touched[ 16 * 128 ];
        bool isTouched[ 16 * 128 ] = {};
        size_t nTouched = 0;

        vector< Pending >& At( size_t idx )
        {
            if ( !isTouched[idx] )
            {
                isTouched[idx] = true;
                touched[nTouched++] = (uint16_t)idx;
            }
            return v[idx];
        }
        void Reset()
        {
            for ( size_t i = 0; i < nTouched; i++ )
            {
                v[ touched[i] ].clear();
                isTouched[ touched[i] ] = false;
            }
            nTouched = 0;
        }
    };

    // nothing row-sized; phase 2 (pMIDI != nullptr) runs the identical walk and
    size_t ParseTrackChunk( const unsigned char *pcData, size_t iMaxSize, size_t iTrack, size_t iRowBase,
                            MIDI *pMIDI, MIDITrack::MIDITrackInfo &ti, vector< MIDIEvent* > &vMetas,
                            size_t iThinBase, size_t &iThinOut, NoteStacks &stacks, MetaArena *pArena,
                            vector< uint64_t > &vDupBits, size_t iPoolLimit, size_t iThinLimit,
                            size_t iEscBase = 0, size_t iEscLimit = 0 )
{
        // Returns the number of pool rows this track keeps (after duplicate folding); iThinOut gets
        // the number of folded note-off rows. In the write pass iPoolLimit/iThinLimit are the counts
        // the counting pass promised: writing past them would corrupt the heap, so exceeding them
        // throws and the load fails cleanly instead.
        ti.iSequenceNumber = (uint32_t)iTrack;
        int iPrevEventCode = 0;
        uint32_t iAbsTicks = 0;
        size_t iTotal = 0, nPool = 0, nThin = 0;
        size_t nPoolAll = 0, nThinAll = 0, nDup = 0; // counting pass bookkeeping
        size_t iEscNext = iEscBase, iEscEnd = iEscBase + iEscLimit;
        uint32_t iNoteSeq = 0;
        auto fnWritePool = [&]( uint32_t uPack ) -> uint32_t {
            if ( nPool >= iPoolLimit ) throw std::runtime_error( "MIDI parse: pool row overflow" );
            const uint32_t iRow = (uint32_t)( iRowBase + nPool++ );
            pMIDI->SetPoolRow( iRow, iAbsTicks, 0, UINT32_MAX, 0, (uint16_t)iTrack, uPack );
            return iRow;
        };
        int iCount = 0;
        bool bEndOfTrack = false;
        stacks.Reset();
        // row is written; phase 2 writes each kept row at its compacted id and
        do
        {
            iCount = 0;
            bEndOfTrack = false;
            uint32_t iDT;
            uint32_t iVarlen = MIDI::ParseVarNum( pcData + iTotal, iMaxSize - iTotal, &iDT );
            if ( iVarlen == 0 || iMaxSize - iTotal - iVarlen < 1 ) break;
            int iEventCode = pcData[iTotal + iVarlen];
            MIDIEvent::EventType eEventType = MIDIEvent::DecodeEventType( iEventCode );
            size_t iDTCode = iVarlen + 1;
            if ( eEventType == MIDIEvent::RunningStatus && iPrevEventCode != 0 )
            {
                iEventCode = iPrevEventCode;
                eEventType = MIDIEvent::DecodeEventType( iEventCode );
                iDTCode = iVarlen;
            }
            if ( eEventType == MIDIEvent::RunningStatus ) break;
            iAbsTicks += iDT;

            switch ( eEventType )
            {
                case MIDIEvent::ChannelEvent:
                {
                    int iNumParams = ( iEventCode & 0xF0 ) == 0xC0 || ( iEventCode & 0xF0 ) == 0xD0 ? 1 : 2;
                    uint32_t uPack = (uint32_t)( (unsigned char)iEventCode );
                    bool bParams = ( iMaxSize - iTotal - iDTCode >= (size_t)iNumParams );
                    if ( bParams )
                    {
                        uPack |= (uint32_t)pcData[ iTotal + iDTCode ] << 8;
                        if ( iNumParams > 1 )
                            uPack |= (uint32_t)pcData[ iTotal + iDTCode + 1 ] << 16;
                    }
                    MIDI::ChannelEventType eType = (MIDI::ChannelEventType)( ( uPack >> 4 ) & 0xF );
                    int iChannel = (int)( uPack & 0xF );
                    int iParam1 = (int)( ( uPack >> 8 ) & 0xFF );
                    int iParam2 = (int)( ( uPack >> 16 ) & 0xFF );
                    // data bytes >= 0x80 only occur in malformed files; mask so the index stays in range
                    const size_t iStackIdx = ( (size_t)iChannel << 7 ) | ( (size_t)iParam1 & 0x7F );
                    const bool bIsOn = eType == MIDI::NoteOn && iParam2 > 0;
                    const bool bIsOff = !bIsOn && ( eType == MIDI::NoteOff || eType == MIDI::NoteOn );
                    if ( !pMIDI )
                    {
                        // Counting pass.
                        if ( bIsOn )
                        {
                            auto &sStack = stacks.At( iStackIdx );
                            Pending p = {};
                            p.tick = iAbsTicks;
                            p.vel = (uint8_t)iParam2;
                            p.key = (uint8_t)iParam1;
                            p.run = 1;
                            p.seq = iNoteSeq++;
                            p.sameAsPred = !sStack.empty() && sStack.back().tick == iAbsTicks && sStack.back().vel == p.vel &&
                                           sStack.back().key == p.key;
                            sStack.push_back( p );
                            nPoolAll++;
                        }
                        else if ( bIsOff )
                        {
                            auto &sStack = stacks.At( iStackIdx );
                            if ( !sStack.empty() )
                            {
                                Pending e = sStack.back();
                                sStack.pop_back();
                                if ( iAbsTicks - e.tick >= MIDI::THIN_DELTA_ESC ) ti.iEscCount++;
                                nThinAll++;
                                // The last identical-start note popped off directly above this one ends on the
                                // same tick: fold it into this note.
                                if ( e.childValid && e.childOffTick == iAbsTicks && (unsigned)e.run + e.childRun <= 128 && !g_bNoDedupe )
                                {
                                    const size_t iWord = e.childSeq >> 6;
                                    if ( iWord >= vDupBits.size() )
                                        vDupBits.resize( (std::max)( iWord + 1, vDupBits.size() + vDupBits.size() / 2 + 16 ), 0 );
                                    vDupBits[iWord] |= 1ull << ( e.childSeq & 63 );
                                    e.run = (uint8_t)( e.run + e.childRun );
                                    nDup++;
                                }
                                if ( e.sameAsPred && !sStack.empty() )
                                {
                                    Pending &pr = sStack.back();
                                    pr.childValid = true;
                                    pr.childSeq = e.seq;
                                    pr.childOffTick = iAbsTicks;
                                    pr.childRun = e.run;
                                }
                            }
                            else
                                nPoolAll++;
                        }
                        else
                            nPoolAll++;
                    }
                    else
                    {
                        // Write pass: same walk, obeying the counting pass's duplicate bits.
                        if ( bIsOn )
                        {
                            auto &sStack = stacks.At( iStackIdx );
                            const uint32_t iSeq = iNoteSeq++;
                            const size_t iWord = iSeq >> 6;
                            const bool bDup = iWord < vDupBits.size() && ( ( vDupBits[iWord] >> ( iSeq & 63 ) ) & 1 ) != 0;
                            Pending p = {};
                            p.tick = iAbsTicks;
                            p.vel = (uint8_t)iParam2;
                            p.seq = iSeq;
                            if ( bDup )
                            {
                                if ( sStack.empty() ) throw std::runtime_error( "MIDI parse: duplicate note without a base note" );
                                p.dup = true;
                                p.rowId = sStack.back().rowId;
                                pMIDI->AddRowRepeat( p.rowId );
                            }
                            else
                                p.rowId = fnWritePool( uPack );
                            sStack.push_back( p );
                        }
                        else if ( bIsOff )
                        {
                            auto &sStack = stacks.At( iStackIdx );
                            if ( !sStack.empty() )
                            {
                                const Pending e = sStack.back();
                                sStack.pop_back();
                                if ( !e.dup )
                                {
                                    if ( nThin >= iThinLimit ) throw std::runtime_error( "MIDI parse: thin row overflow" );
                                    pMIDI->SetThinRow( iThinBase + nThin, iAbsTicks,
                                        e.rowId | ( ( eType == MIDI::NoteOn ) ? MIDI::THIN_OWNER_NOTEON_FLAG : 0 ),
                                        UINT32_MAX, iEscNext, iEscEnd );
                                    nThin++;
                                }
                            }
                            else
                                fnWritePool( uPack );
                        }
                        else
                            fnWritePool( uPack );
                    }

                    if ( !bParams ) break;
                    iTotal += iDTCode + iNumParams;
                    iCount = iNumParams;

                    if ( pMIDI ) break; // track info comes from the counting pass; the write pass needs none of it
                    ti.iEventCount++;
                    ti.iTotalTicks = max( ti.iTotalTicks, (int)iAbsTicks );
                    switch ( eType )
                    {
                        case MIDI::NoteOn:
                            if ( iParam2 > 0 )
                            {
                                if ( !ti.iNoteCount )
                                {
                                    ti.iMinNote = ti.iMaxNote = iParam1;
                                    ti.iMaxVolume = iParam2;
                                }
                                else
                                {
                                    ti.iMinNote = min( iParam1, ti.iMinNote );
                                    ti.iMaxNote = max( iParam1, ti.iMaxNote );
                                    ti.iMaxVolume = max( iParam2, ti.iMaxVolume );
                                }
                                ti.iNoteCount++;
                                ti.iVolumeSum += iParam2;
                                if ( !ti.aNoteCount[ iChannel ] )
                                    ti.iNumChannels++;
                                ti.aNoteCount[ iChannel ]++;
                            }
                            break;
                        case MIDI::ProgramChange:
                            if ( ti.aProgram[ iChannel ] != iParam1 )
                            {
                                if ( ti.aNoteCount[ iChannel ] > 0 )
                                    ti.aProgram[ iChannel ] = 128; // Various
                                else
                                    ti.aProgram[ iChannel ] = iParam1;
                            }
                            break;
                        default:
                            break;
                    }
                    break;
                }
                case MIDIEvent::MetaEvent:
                case MIDIEvent::SysExEvent:
                {
                    // Phase 1 only needs the event for track info, so parse into stack
                    // objects instead of a new/delete per meta event. Phase 2 keeps the
                    // heap events, which the tracks own afterwards.
                    MIDIMetaEvent localMeta;
                    MIDISysExEvent localSysEx;
                    MIDIEvent *pEvent;
                    const size_t iEventBytes = ( eEventType == MIDIEvent::MetaEvent ) ? sizeof( MIDIMetaEvent ) : sizeof( MIDISysExEvent );
                    if ( pMIDI )
                    {
                        void *pMem = pArena->Alloc( iEventBytes );
                        pEvent = ( eEventType == MIDIEvent::MetaEvent ) ? (MIDIEvent*)new ( pMem ) MIDIMetaEvent() : (MIDIEvent*)new ( pMem ) MIDISysExEvent();
                    }
                    else
                        pEvent = ( eEventType == MIDIEvent::MetaEvent ) ? (MIDIEvent*)&localMeta : (MIDIEvent*)&localSysEx;
                    pEvent->m_eEventType = (char)eEventType;
                    pEvent->m_iEventCode = (unsigned char)iEventCode;
                    pEvent->m_iTrack = (unsigned short)iTrack;
                    pEvent->m_iAbsT = (int)iAbsTicks;
                    iCount = ( eEventType == MIDIEvent::MetaEvent )
                                 ? ( (MIDIMetaEvent*)pEvent )->ParseEvent( pcData + iTotal + iDTCode, iMaxSize - iTotal - iDTCode )
                                 : ( (MIDISysExEvent*)pEvent )->ParseEvent( pcData + iTotal + iDTCode, iMaxSize - iTotal - iDTCode );
                    if ( iCount > 0 )
                    {
                        iTotal += iDTCode + iCount;
                        if ( !pMIDI ) ti.AddEventInfo( *pEvent );
                        // Decide here: the old loop condition dereferenced pEvent after it
                        // had been deleted (use-after-free; result depended on the heap).
                        bEndOfTrack = eEventType == MIDIEvent::MetaEvent &&
                            ( (MIDIMetaEvent*)pEvent )->GetMetaEventType() == MIDIMetaEvent::EndOfTrack;
                        if ( pMIDI )
                            vMetas.push_back( pEvent );
                    }
                    else if ( pMIDI )
                    {
                        MIDIEvent::DestroyInPlace( pEvent );
                        pArena->Unalloc( pEvent, iEventBytes );
                    }
                    break;
                }
                default:
                    break;
            }
            if ( eEventType == MIDIEvent::ChannelEvent )
                iPrevEventCode = iEventCode;
        }
        while ( iMaxSize - iTotal > 0 && iCount > 0 && !bEndOfTrack );
        if ( !pMIDI )
        {
            // each folded note drops one pool row and one note-off row
            nPool = nPoolAll - nDup;
            nThin = nThinAll - nDup;
        }
        iThinOut = nThin;
        return nPool;
    }
}

// Writes one channel-event row into the pool columns (parallel phase-2 walk;
void MIDI::SetPoolRow( size_t iRow, uint32_t iTicks, uint32_t iLengths, uint32_t iSisters,
                       uint32_t iSimult, uint16_t iEventTrack, uint32_t iPack )
{
    if ( !m_bComputedTimes ) m_vTimes[iRow] = 0;
    m_vTicks[iRow] = iTicks;
    m_vLengths[iRow] = iLengths;
    m_vSisters[iRow] = iSisters;
    m_vSimult[iRow] = iSimult;
    m_vEventTrack[iRow] = iEventTrack;
    m_vPack[iRow] = iPack;
}

void MIDI::SetThinRow( size_t iThin, uint32_t iTicks, uint32_t iOwners, uint32_t iSisters, size_t &iEscNext, size_t iEscEnd )
{
    if ( m_bThinWide )
        m_vThinTicks[iThin] = iTicks;
    else
    {
        const uint32_t d = iTicks - m_vTicks[ iOwners & ~THIN_OWNER_NOTEON_FLAG ];
        if ( d < THIN_DELTA_ESC )
            m_vThinDelta[iThin] = (uint16_t)d;
        else
        {
            if ( iEscNext >= iEscEnd ) throw std::runtime_error( "MIDI parse: escape table overflow" );
            m_vThinEsc[iEscNext++] = ThinEsc{ (uint32_t)iThin, iTicks };
            m_vThinDelta[iThin] = THIN_DELTA_ESC;
        }
    }
    m_vThinOwners[iThin] = iOwners;
    m_vThinSisters[iThin] = iSisters;
}

uint32_t MIDI::ThinEscTick( size_t thin ) const
{
    size_t lo = 0, hi = m_vThinEsc.size();
    while ( lo < hi )
    {
        const size_t mid = lo + ( hi - lo ) / 2;
        if ( m_vThinEsc[mid].thin < thin ) lo = mid + 1; else hi = mid;
    }
    return ( lo < m_vThinEsc.size() && m_vThinEsc[lo].thin == thin ) ? m_vThinEsc[lo].tick : 0;
}

// Runs fn on up to nThreads threads, the calling thread being one of them. If the OS refuses
// to create a thread (low memory / handle exhaustion) the remaining work still gets done by
// the threads that did start; an uncaught std::system_error here used to terminate the app.
template< typename Fn >
static void RunPool( unsigned nThreads, Fn &&fn )
{
    vector< thread > vThreads;
    try
    {
        vThreads.reserve( nThreads );
        for ( unsigned t = 1; t < nThreads; t++ )
            vThreads.emplace_back( fn );
    }
    catch ( ... ) {}
    fn();
    for ( auto &th : vThreads )
        th.join();
}

size_t MIDI::ParseTracks( const unsigned char *pcData, size_t iMaxSize )
{
    size_t iTotal = 0, iCount = 0;
    g_LoadingProgress.stage = MIDILoadingProgress::Stage::ParseTracks;
    g_LoadingProgress.progress = 0;
    g_LoadingProgress.max = m_Info.iNumTracks; // not actually guaranteed to hit this

    vector< size_t > vOffsets, vSizes;
    do
    {
        char pcBuf[4];
        uint32_t iTrkSize;
        if ( MIDI::ParseNChars( pcData + iTotal, 4, iMaxSize - iTotal, pcBuf ) != 4 ) { iCount = 0; break; }
        if ( MIDI::Parse32Bit( pcData + iTotal + 4, iMaxSize - iTotal - 4, &iTrkSize ) != 4 ) { iCount = 0; break; }
        if ( strncmp( pcBuf, "MTrk", 4 ) != 0 ) { iCount = 0; break; }
        vOffsets.push_back( iTotal );
        vSizes.push_back( iTrkSize );
        iTotal += 8 + iTrkSize;
        iCount = (int)( 8 + iTrkSize );
    }
    // iTotal can overshoot iMaxSize on a truncated file; "iMaxSize - iTotal > 0" then wrapped
    // and the next header read ran off the end of the buffer.
    while ( iTotal < iMaxSize && iCount > 0 && m_Info.iFormatType != 2 );

    const size_t nChunks = vOffsets.size();
    g_LoadingProgress.max = nChunks * 2; // two passes over every chunk
    vector< size_t > vOrder( nChunks );
    for ( size_t c = 0; c < nChunks; c++ ) vOrder[c] = c;
    std::stable_sort( vOrder.begin(), vOrder.end(), [&]( size_t a, size_t b ) { return vSizes[a] > vSizes[b]; } );
    vector< MIDITrack::MIDITrackInfo > vTrackInfo( nChunks );
    vector< size_t > vRows( nChunks ); // pool rows each track keeps (after duplicate folding)
    vector< size_t > vThin( nChunks );
    vector< vector< uint64_t > > vDupBits( nChunks ); // per track: bit per note-on, set if folded into an earlier note
    vector< exception_ptr > vErrors( nChunks );
    // A mapped file view: once a track is parsed, drop its pages from the working set (they stay in the
    // OS cache) so the file doesn't count against the process while the pool grows.
    const bool bTrim = m_bOwnedMapped;
    auto trimChunk = [&]( size_t c ) {
        if ( bTrim )
            VirtualUnlock( const_cast< unsigned char* >( pcData ) + vOffsets[c], min( (size_t)8 + vSizes[c], iMaxSize - vOffsets[c] ) );
    };
    PRE_DbgLog("LOAD [ParseTracks chunks=%zu]", nChunks);
    unsigned int nThreads = std::thread::hardware_concurrency();
    if ( nThreads == 0 ) nThreads = 4;
    nThreads = min( nThreads, (unsigned int)min< size_t >( nChunks, 16 ) );
    if ( nThreads == 0 ) nThreads = 1;
    {
        atomic< size_t > iNext( 0 );
        atomic< size_t > iDone( 0 );
        RunPool( nThreads, [&]() {
                size_t i, k;
                unique_ptr< NoteStacks > pStacks;
                // biggest tracks first, so no worker starts a huge one last and leaves the rest idle
                while ( ( i = ( ( k = iNext.fetch_add( 1, memory_order_relaxed ) ) < nChunks ) ? vOrder[k] : nChunks ) < nChunks )
                {
                    vector< MIDIEvent* > scratch;
                    try
                    {
                        if ( !pStacks ) pStacks.reset( new NoteStacks() );
                        vRows[i] = ParseTrackChunk( pcData + vOffsets[i] + 8, iMaxSize - vOffsets[i] - 8,
                                                    i, 0, nullptr, vTrackInfo[i], scratch, 0, vThin[i], *pStacks, nullptr,
                                                    vDupBits[i], 0, 0 );
                        trimChunk( i );
                    }
                    catch ( ... )
                    {
                        vErrors[i] = current_exception();
                    }
                    g_LoadingProgress.progress = min( (uint64_t)iDone.fetch_add( 1, memory_order_relaxed ) + 1, g_LoadingProgress.max );
                }
            } );
        for ( size_t c = 0; c < nChunks; c++ )
            if ( vErrors[c] )
                rethrow_exception( vErrors[c] );
    }
    PRE_DbgLog("LOAD [ParseTracks parsed chunks]");

    // pending note-on live only in the 16-byte thin arrays; the pool only ever
    size_t iTotalRows = 0, iThinTotal = 0;
    vector< size_t > vFullBases( nChunks ), vThinBases( nChunks );
    for ( size_t c = 0; c < nChunks; c++ )
    {
        vFullBases[c] = iTotalRows;
        iTotalRows += vRows[c];
        vThinBases[c] = iThinTotal;
        iThinTotal += vThin[c];
    }
    m_iFullRows = (uint32_t)iTotalRows;
    PRE_DbgLog("LOAD [ParseTracks rows=%zu thin=%zu]", iTotalRows, iThinTotal);

    // Note-off ticks are stored as 16-bit offsets from their note-on; notes longer than that go to a small
    // sorted escape table. A file full of such notes would make the table cost more than the offsets save,
    // so then the column stays a plain 32-bit tick (PGFA_THIN_WIDE=1 forces that, for testing).
    vector< size_t > vEscBases( nChunks );
    size_t iEscTotal = 0;
    for ( size_t c = 0; c < nChunks; c++ )
    {
        vEscBases[c] = iEscTotal;
        iEscTotal += vTrackInfo[c].iEscCount;
    }
    m_bThinWide = iEscTotal * 4 > iThinTotal;
    {
        char aEnv[8] = {};
        if ( GetEnvironmentVariableA( "PGFA_THIN_WIDE", aEnv, sizeof( aEnv ) ) > 0 )
            m_bThinWide = aEnv[0] != '0';
    }
    PRE_DbgLog("LOAD [ParseTracks thin ticks %s, escapes<=%zu]", m_bThinWide ? "wide" : "delta16", iEscTotal);

    // With only a handful of tempo events the 8-byte time column is redundant: a row's time is a
    // pure function of its tick, so skip storing it (see MIDI::TimeFromTick).
    size_t iTempoTotal = 0;
    for ( size_t c = 0; c < nChunks; c++ )
        iTempoTotal += vTrackInfo[c].iTempoCount;
    // Computing a time costs a few ns more than loading it, so only files big enough for the saved
    // 8 bytes/row to matter use it (PGFA_COMPUTED_TIMES_MIN overrides the row threshold, for testing).
    size_t iComputedMinRows = (size_t)16 << 20;
    {
        char aEnv[32] = {};
        if ( GetEnvironmentVariableA( "PGFA_COMPUTED_TIMES_MIN", aEnv, sizeof( aEnv ) ) > 0 )
            iComputedMinRows = (size_t)_strtoui64( aEnv, nullptr, 10 );
    }
    m_bComputedTimes = iTempoTotal + 1 <= MaxComputedAnchors && iTotalRows + iThinTotal >= iComputedMinRows;
    if ( m_bComputedTimes )
    {
        // Provisional map (default tempo); PostProcess installs the real one.
        m_bTimeStandard = !( m_Info.iDivision & 0x8000 );
        m_iTimeTicksPerBeat = m_bTimeStandard ? (int)m_Info.iDivision : 0;
        m_iTimeTicksPerSecond = 0;
        SetTimeDivisor( m_iTimeTicksPerBeat );
        const int iTick0 = 0;
        const int64_t iTime0 = 0;
        const uint32_t iMpb0 = 500000;
        SetTimeAnchors( 1, &iTick0, &iTime0, &iMpb0 );
    }

    try
    {
        if ( !m_bComputedTimes ) m_vTimes.reserve( iTotalRows );
        m_vTicks.reserve( iTotalRows );
        m_vLengths.reserve( iTotalRows );
        m_vSisters.reserve( iTotalRows );
        m_vSimult.reserve( iTotalRows );
        m_vEventTrack.reserve( iTotalRows );
        m_vPack.reserve( iTotalRows );
        if ( m_bThinWide ) m_vThinTicks.reserve( iThinTotal );
        else { m_vThinDelta.reserve( iThinTotal ); m_vThinEsc.reserve( iEscTotal ); }
        m_vThinOwners.reserve( iThinTotal );
        m_vThinSisters.reserve( iThinTotal );
        PRE_DbgLog("LOAD [ParseTracks reserve done]");
    }
    catch ( const std::exception &e )
    {
        PRE_DbgLog("LOAD [ParseTracks reserve FAILED: %s]", e.what());
        throw;
    }

    if ( !m_bComputedTimes ) m_vTimes.resize( iTotalRows );
    m_vTicks.resize( iTotalRows );
    m_vLengths.resize( iTotalRows );
    m_vSisters.resize( iTotalRows );
    m_vSimult.resize( iTotalRows );
    m_vEventTrack.resize( iTotalRows );
    m_vPack.resize( iTotalRows );
    if ( m_bThinWide ) m_vThinTicks.resize( iThinTotal );
    else
    {
        m_vThinDelta.resize( iThinTotal );
        m_vThinEsc.resize( iEscTotal );
        for ( size_t e = 0; e < iEscTotal; e++ ) m_vThinEsc[e] = ThinEsc{ UINT32_MAX, 0 };
    }
    m_vThinOwners.resize( iThinTotal );
    m_vThinSisters.resize( iThinTotal );
    PRE_DbgLog("LOAD [ParseTracks resized]");

    // worker owns a disjoint full and thin range, so no locks are needed; the
    vector< vector< MIDIEvent* > > vMetasAll( nChunks );
    vector< MetaArena > vArenas( nChunks );
    {
        atomic< size_t > iNext( 0 );
        atomic< size_t > iDone2( 0 );
        RunPool( nThreads, [&]() {
                size_t i, k;
                unique_ptr< NoteStacks > pStacks;
                // biggest tracks first, so no worker starts a huge one last and leaves the rest idle
                while ( ( i = ( ( k = iNext.fetch_add( 1, memory_order_relaxed ) ) < nChunks ) ? vOrder[k] : nChunks ) < nChunks )
                {
                    vector< MIDIEvent* > metas;
                    MIDITrack::MIDITrackInfo tiThrowaway;
                    size_t iThinDummy = 0;
                    try
                    {
                        if ( !pStacks ) pStacks.reset( new NoteStacks() );
                        size_t n = ParseTrackChunk( pcData + vOffsets[i] + 8, iMaxSize - vOffsets[i] - 8,
                                                    i, vFullBases[i], this, tiThrowaway, metas,
                                                    vThinBases[i], iThinDummy, *pStacks, &vArenas[i],
                                                    vDupBits[i], vRows[i], vThin[i], vEscBases[i], vTrackInfo[i].iEscCount );
                        // The rows were sized from the counting pass; the write pass must land exactly on them.
                        if ( n != vRows[i] || iThinDummy != vThin[i] )
                        {
                            PRE_DbgLog("LOAD [ParseTracks count mismatch track=%zu: rows %zu vs %zu, thin %zu vs %zu]",
                                i, n, vRows[i], iThinDummy, vThin[i]);
                            throw std::runtime_error( "MIDI parse: counting and write passes disagree" );
                        }
                        vector< uint64_t >().swap( vDupBits[i] );
                        trimChunk( i );
                        vMetasAll[i].swap( metas );
                    }
                    catch ( ... )
                    {
                        vErrors[i] = current_exception();
                    }
                    g_LoadingProgress.progress = min( nChunks + (uint64_t)iDone2.fetch_add( 1, memory_order_relaxed ) + 1, g_LoadingProgress.max );
                }
            } );
        for ( size_t c = 0; c < nChunks; c++ )
            if ( vErrors[c] )
                rethrow_exception( vErrors[c] );
    }

    PRE_DbgLog("LOAD [ParseTracks phase 2 done]");
    if ( !m_bThinWide )
    {
        // drop the unused escape slots (the counting pass over-counts folded notes); order stays sorted
        // because tracks and their thin ranges are both laid out in track order
        size_t n = 0;
        for ( size_t e = 0; e < m_vThinEsc.size(); e++ )
            if ( m_vThinEsc[e].thin != UINT32_MAX )
                m_vThinEsc[n++] = m_vThinEsc[e];
        m_vThinEsc.resize( n );
    }
    for ( size_t c = 0; c < nChunks; c++ )
    {
        MIDITrack *track = new MIDITrack( *this );
        track->m_iRowStart = vFullBases[c];
        track->m_iRowEnd = vFullBases[c] + vRows[c];
        track->m_iThinStart = vThinBases[c];
        track->m_iThinEnd = vThinBases[c] + vThin[c];
        track->m_TrackInfo = std::move( vTrackInfo[c] );
        track->m_vMetas = std::move( vMetasAll[c] );
        track->m_Arena = std::move( vArenas[c] );
        track->m_bArenaMetas = true;
        m_vTracks.push_back( track );
        m_Info.AddTrackInfo( *track );
    }
    PRE_DbgLog("LOAD [ParseTracks merged rows=%zu]", iTotalRows);

    m_Info.iNumTracks = (int)m_vTracks.size();

    return iTotal;
}

size_t MIDI::ParseEvents( const unsigned char *pcData, size_t iMaxSize )
{
    MIDITrack *track = new MIDITrack(*this);
    size_t iCount = track->ParseEvents( pcData, iMaxSize, m_vTracks.size());

    if ( iCount > 0 ) {
        m_vTracks.push_back( track );
        m_Info.AddTrackInfo( *track );
    }
    else
        delete track;

    return iCount;
}

void MIDI::MIDIInfo::AddTrackInfo( const MIDITrack &mTrack )
{
    const MIDITrack::MIDITrackInfo &mti = mTrack.GetInfo();
    this->iTotalTicks = max( this->iTotalTicks, mti.iTotalTicks );
    this->iEventCount += mti.iEventCount;
    this->iNumChannels += mti.iNumChannels;
    this->iVolumeSum += mti.iVolumeSum;
    if ( mti.iNoteCount )
    {
        if ( !this->iNoteCount )
        {
            this->iMinNote = mti.iMinNote;
            this->iMaxNote = mti.iMaxNote;
            this->iMaxVolume = mti.iMaxVolume;
        }
        else
        {
            this->iMinNote = min( mti.iMinNote, this->iMinNote );
            this->iMaxNote = max( mti.iMaxNote, this->iMaxNote );
            this->iMaxVolume = max( mti.iMaxVolume, this->iMaxVolume );
        }
    }
    this->iNoteCount += mti.iNoteCount;
    if ( !( this->iDivision & 0x8000 ) && this->iDivision > 0 )
        this->iTotalBeats = this->iTotalTicks / this->iDivision;
}

void MIDI::PostProcess(vector<MIDIChannelEvent>& vChannelEvents, eventvec_t* vProgramChanges, vector<MIDIMetaEvent*>* vMetaEvents, eventvec_t* vTempo, eventvec_t* vSignature, eventvec_t* vMarkers)
{
    LogLoadStage("PostProcess parallel start", this);
    PostProcessParallel(vChannelEvents, vProgramChanges, vMetaEvents, vTempo, vSignature, vMarkers);
    LogLoadStage("PostProcess parallel end", this);
    return;

    // Legacy single-consumer implementation retained below as a correctness
    // reference/fallback while the active path above performs every heavy pass
    // across the physical-core worker pool.
    ParallelMIDIPos midiPos( *this );
    bool bIsStandard = midiPos.IsStandard();
    int iTicksPerBeat = midiPos.GetTicksPerBeat();
    int iTicksPerSecond = midiPos.GetTicksPerSecond();
    int iMicroSecsPerBeat = midiPos.GetMicroSecsPerBeat();
    int iLastTempoTick = 0;
    long long llLastTempoTime = 0;
    int iSimultaneous = 0;

    size_t event_count = 0;
    for (size_t i = 0; i < m_vTracks.size(); i++)
        event_count += m_vTracks[i]->GetRowCount() + m_vTracks[i]->GetThinCount() + m_vTracks[i]->GetMetaCount();
    if (event_count > vChannelEvents.capacity())
        vChannelEvents.reserve(event_count);
    LogLoadStage("PostProcess start", this);

    g_LoadingProgress.stage = MIDILoadingProgress::Stage::SortEvents;
    g_LoadingProgress.progress = 0;
    g_LoadingProgress.max = event_count;

    MIDIEvent *pEvent = NULL;
    MIDIChannelEvent iRow = UINT32_MAX;
    long long llFirstNote = -1;
    long long llTime = 0;
    int iSpan;
    while ( ( iSpan = midiPos.GetNextEvent( -1, &pEvent, &iRow ) ) != 0 || pEvent || iRow != UINT32_MAX )
    {
        int iTick = pEvent ? pEvent->GetAbsT() : (int)GetEventTicks(iRow);
        if ( bIsStandard )
            llTime = llLastTempoTime + ( static_cast< long long >( iMicroSecsPerBeat ) * ( iTick - iLastTempoTick ) ) / iTicksPerBeat;
        else
            llTime = llLastTempoTime + ( 1000000LL * ( iTick - iLastTempoTick ) ) / iTicksPerSecond;

        if ( pEvent )
        {
            pEvent->SetAbsMicroSec( llTime );

            if ( pEvent->GetEventType() == MIDIEvent::MetaEvent )
            {
                MIDIMetaEvent *pMetaEvent = reinterpret_cast< MIDIMetaEvent* >( pEvent );
                if ( pMetaEvent->GetMetaEventType() == MIDIMetaEvent::SetTempo )
                {
                    iTicksPerBeat = midiPos.GetTicksPerBeat();
                    iTicksPerSecond = midiPos.GetTicksPerSecond();
                    iMicroSecsPerBeat = midiPos.GetMicroSecsPerBeat();
                    iLastTempoTick = iTick;
                    llLastTempoTime = llTime;
                }

                if (vMetaEvents) {
                    MIDIMetaEvent::MetaEventType eEventType = pMetaEvent->GetMetaEventType();
                    vMetaEvents->push_back(pMetaEvent);
                    switch (eEventType) {
                    case MIDIMetaEvent::SetTempo:
                        if (vTempo)
                            vTempo->push_back(pair< long long, int >(llTime, (int)vMetaEvents->size() - 1));
                        break;
                    case MIDIMetaEvent::TimeSignature:
                        if (vSignature)
                            vSignature->push_back(pair< long long, int >(llTime, (int)vMetaEvents->size() - 1));
                        break;
                    case MIDIMetaEvent::Marker:
                        if (vMarkers)
                            vMarkers->push_back(pair< long long, int >(llTime, (int)vMetaEvents->size() - 1));
                        break;
                    default:
                        break;
                    }
                }
            }
        }
        else
        {
            SetEventTime(iRow, llTime);
            // Folded note-offs derive their time from the note-on owner; stash
            if ( IsThinRow(iRow) )
            {
                MIDIChannelEvent iOwner = GetThinOwner(iRow);
                if ( GetEventTicks(iRow) == GetEventTicks(iOwner) )
                    SetEventLength(iRow, 0);
                else
                    SetEventLength(iRow, (uint32_t)max(0LL, llTime - GetEventTime(iOwner)));
            }

            SetEventSimult(iRow, iSimultaneous);
            if ( EventHasSister(iRow) )
            {
                if ( GetEventChannelEventType(iRow) == NoteOn &&
                     GetEventParam2(iRow) > 0 )
                {
                    if ( llFirstNote < 0  )
                        llFirstNote = llTime;
                    iSimultaneous++;
                }
                else
                    iSimultaneous--;
                if ( !GetEventPassDone(iRow) )
                {
                    MIDIChannelEvent iSister = GetEventSisterIdx(iRow);
                    SetEventSisterIdx(iSister, (unsigned)vChannelEvents.size());
                    SetEventPassDone(iSister, true);
                    if (GetEventChannelEventType(iRow) != NoteOn ||
                        GetEventParam2(iRow) == 0) {
                        SetEventLength(iSister, (uint32_t)(llTime - GetEventTime(iSister)));
                    }
                }
                else
                {
                    MIDIChannelEvent iSister = vChannelEvents[GetEventSisterIdx(iRow)];
                    SetEventSisterIdx(iSister, (unsigned)vChannelEvents.size());
                    SetEventPassDone(iRow, true);
                    SetEventLength(iSister, (uint32_t)(llTime - GetEventTime(iSister)));
                }
            }
            vChannelEvents.push_back(iRow);

            ChannelEventType eEventType = GetEventChannelEventType(iRow);
            if (vProgramChanges && (eEventType == ProgramChange || eEventType == Controller || eEventType == PitchBend))
                vProgramChanges->push_back(pair< long long, int >(llTime, (int)vChannelEvents.size() - 1));
        }

        g_LoadingProgress.progress++;
    }

    // Repeated notes were getting cut off: the packer stores note-offs as thin
    // rows and the merge emits pool rows (note-ons) before thin rows on tick
    // ties, flipping the file's [off, on] order. BASS then kills the just-started
    // note instantly. Restore file-order semantics within each equal-time group:
    // thin rows first, pool rows after (relative order preserved), and keep the
    // sister web pointing at the rows' new positions.
    {
        // pool row id -> current list position (partition moves pools too)
        vector<uint32_t> vRowPos(m_iFullRows);
        for (size_t p = 0; p < vChannelEvents.size(); p++)
            if (!IsThinRow(vChannelEvents[p]))
                vRowPos[vChannelEvents[p]] = (uint32_t)p;

        size_t iGroupStart = 0;
        const size_t iCount = vChannelEvents.size();
        while (iGroupStart < iCount)
        {
            long long llGroupTime = GetEventTime(vChannelEvents[iGroupStart]);
            size_t iGroupEnd = iGroupStart + 1;
            while (iGroupEnd < iCount && GetEventTime(vChannelEvents[iGroupEnd]) == llGroupTime)
                iGroupEnd++;
            if (iGroupEnd - iGroupStart > 1)
            {
                vector<MIDIChannelEvent> vThins, vPools;
                vThins.reserve(iGroupEnd - iGroupStart);
                vPools.reserve(iGroupEnd - iGroupStart);
                for (size_t p = iGroupStart; p < iGroupEnd; p++)
                {
                    if (IsThinRow(vChannelEvents[p]))
                        vThins.push_back(vChannelEvents[p]);
                    else
                        vPools.push_back(vChannelEvents[p]);
                }
                if (!vThins.empty() && !vPools.empty())
                {
                    std::copy(vThins.begin(), vThins.end(), vChannelEvents.begin() + iGroupStart);
                    std::copy(vPools.begin(), vPools.end(), vChannelEvents.begin() + iGroupStart + vThins.size());
                    const size_t iPoolBase = iGroupStart + vThins.size();
                    for (size_t j = 0; j < vPools.size(); j++)
                        vRowPos[vPools[j]] = (uint32_t)(iPoolBase + j);
                }
            }
            iGroupStart = iGroupEnd;
        }

        // The merge points each thin row at its owner pool's position at the
        // moment the thin was emitted; that position is stale whenever the
        // pool's tie group was partitioned afterwards. Rewrite the whole
        // sister web from the final positions.
        for (size_t p = 0; p < iCount; p++)
        {
            MIDIChannelEvent iRow = vChannelEvents[p];
            if (IsThinRow(iRow))
            {
                MIDIChannelEvent iOwner = GetThinOwner(iRow);
                SetEventSisterIdx(iRow, vRowPos[iOwner]);
                SetEventSisterIdx(iOwner, (unsigned)p);
            }
        }
    }

    // We don't need the per-track row/meta cursors anymore; the merged list is

    m_Info.llTotalMicroSecs = llTime;
    m_Info.llFirstNote = max( 0LL, llFirstNote );
    LogLoadStage("PostProcess end", this);
}

void MIDI::ConnectNotes()
{
    LogLoadStage("ConnectNotes start", this);

    g_LoadingProgress.stage = MIDILoadingProgress::Stage::ConnectNotes;
    g_LoadingProgress.progress = 0;
    // Connect just links both directions on the row ids; PostProcess later.
    // Split evenly over the thin rows rather than per track (tracks are wildly uneven) and
    // use plain threads instead of PPL: the ConcRT scheduler is a known source of stalls.
    // Every thin row has a unique owner, so all writes are disjoint.
    const size_t nThin = m_vThinOwners.size();
    unsigned nWorkers = PGFAParallelPostProcess::PhysicalCoreCount();
    nWorkers = (std::min)( nWorkers, (unsigned)(std::min<size_t>)( nThin / 65536 + 1, 64 ) );
    nWorkers = (std::max)( nWorkers, 1u );
    g_LoadingProgress.max = nWorkers;
    atomic< unsigned > iNextWorker( 0 );
    RunPool( nWorkers, [&]() {
        unsigned w;
        while ( ( w = iNextWorker.fetch_add( 1, memory_order_relaxed ) ) < nWorkers )
        {
            const size_t begin = nThin * w / nWorkers, end = nThin * ( w + 1 ) / nWorkers;
            for (size_t t = begin; t < end; t++) {
                MIDIChannelEvent iThin = (MIDIChannelEvent)(m_iFullRows + t);
                MIDIChannelEvent iOwner = GetThinOwner(iThin);
                SetEventSisterIdx(iThin, iOwner);
                SetEventSisterIdx(iOwner, iThin);
            }
            g_LoadingProgress.progress++;
        }
    });
    LogLoadStage("ConnectNotes end", this);
}



MIDITrack::MIDITrack(MIDI& midi) : m_MIDI(midi) {};

MIDITrack::~MIDITrack( void )
{
    clear();
}

void MIDITrack::clear( void )
{
    for (auto it = m_vMetas.begin(); it != m_vMetas.end(); ++it)
    {
        if ( m_bArenaMetas )
            MIDIEvent::DestroyInPlace( *it );
        else
            MIDIEvent::DestroyHeap( *it );
    }
    m_vMetas.clear();
    m_Arena.Release();
    m_bArenaMetas = false;
    m_iRowStart = m_iRowEnd = 0;
    m_TrackInfo.clear();
}

size_t MIDITrack::ParseTrack( const unsigned char *pcData, size_t iMaxSize, size_t iTrack )
{
    char pcBuf[4];
    size_t iTotal;
    uint32_t iTrkSize;

    clear();

    if ( MIDI::ParseNChars( pcData, 4, iMaxSize, pcBuf ) != 4 )
        return 0;
    if ( MIDI::Parse32Bit( pcData + 4, iMaxSize - 4, &iTrkSize) != 4 )
        return 0;
    iTotal = 8;

    if ( strncmp( pcBuf, "MTrk", 4 ) != 0 )
        return 0;

    //return iTotal + ParseEvents( pcData + iTotal, iMaxSize - iTotal, iTrack );
    ParseEvents(pcData + iTotal, iMaxSize - iTotal, iTrack);
    return iTotal + iTrkSize;
}

size_t MIDITrack::ParseEvents( const unsigned char *pcData, size_t iMaxSize, size_t iTrack )
{
    int iDTCode = 0, iPrevEventCode = 0;
    uint32_t iAbsTicks = 0;
    size_t iTotal = 0, iCount = 0;
    MIDIChannelEvent iEvent = UINT32_MAX;
    MIDIEvent *pEvent = NULL;
    m_TrackInfo.iSequenceNumber = iTrack;

    m_iRowStart = m_MIDI.GetEventPoolCount();

    do
    {
        iCount = 0;
        iDTCode = MIDIEvent::MakeNextEvent( m_MIDI, pcData + iTotal, iMaxSize - iTotal, iTrack,
                                            &iAbsTicks, &iPrevEventCode, &iEvent, &pEvent );
        if ( iDTCode > 0 )
        {
            if ( pEvent )
            {
                switch (pEvent->GetEventType())
                {
                case MIDIEvent::MetaEvent: iCount = ((MIDIMetaEvent*)pEvent)->ParseEvent(pcData + iDTCode + iTotal, iMaxSize - iDTCode - iTotal); break;
                case MIDIEvent::SysExEvent: iCount = ((MIDISysExEvent*)pEvent)->ParseEvent(pcData + iDTCode + iTotal, iMaxSize - iDTCode - iTotal); break;
                default: break;
                }
                if ( iCount > 0 )
                {
                    iTotal += iDTCode + iCount;
                    m_vMetas.push_back( pEvent );
                    m_TrackInfo.AddEventInfo( *pEvent );
                }
                else
                    MIDIEvent::DestroyHeap( pEvent );
            }
            else
            {
                unsigned char cCode = m_MIDI.GetEventCode( iEvent );
                int iNumParams = ( cCode & 0xF0 ) == 0xC0 || ( cCode & 0xF0 ) == 0xD0 ? 1 : 2;
                if ( iMaxSize - iTotal - iDTCode < (size_t)iNumParams ) break;
                m_MIDI.SetEventParam1( iEvent, pcData[ iTotal + iDTCode ] );
                if ( iNumParams > 1 )
                    m_MIDI.SetEventParam2( iEvent, pcData[ iTotal + iDTCode + 1 ] );

                iTotal += iDTCode + iNumParams;
                iCount = iNumParams;

                m_TrackInfo.iEventCount++;
                m_TrackInfo.iTotalTicks = max( m_TrackInfo.iTotalTicks, (int)m_MIDI.GetEventTicks( iEvent ) );
                MIDI::ChannelEventType eType = m_MIDI.GetEventChannelEventType( iEvent );
                int iChannel = m_MIDI.GetEventChannel( iEvent );
                int iParam1 = m_MIDI.GetEventParam1( iEvent );
                int iParam2 = m_MIDI.GetEventParam2( iEvent );
                switch ( eType )
                {
                    case MIDI::NoteOn:
                        if ( iParam2 > 0 )
                        {
                            if ( !m_TrackInfo.iNoteCount )
                            {
                                m_TrackInfo.iMinNote = m_TrackInfo.iMaxNote = iParam1;
                                m_TrackInfo.iMaxVolume = iParam2;
                            }
                            else
                            {
                                m_TrackInfo.iMinNote = min( iParam1, m_TrackInfo.iMinNote );
                                m_TrackInfo.iMaxNote = max( iParam1, m_TrackInfo.iMaxNote );
                                m_TrackInfo.iMaxVolume = max( iParam2, m_TrackInfo.iMaxVolume );
                            }
                            m_TrackInfo.iNoteCount++;
                            m_TrackInfo.iVolumeSum += iParam2;
                            if ( !m_TrackInfo.aNoteCount[ iChannel ] )
                                m_TrackInfo.iNumChannels++;
                            m_TrackInfo.aNoteCount[ iChannel ]++;
                        }
                        break;
                    case MIDI::ProgramChange:
                        if ( m_TrackInfo.aProgram[ iChannel ] != iParam1 )
                        {
                            if ( m_TrackInfo.aNoteCount[ iChannel ] > 0 )
                                m_TrackInfo.aProgram[ iChannel ] = 128; // Various
                            else
                                m_TrackInfo.aProgram[ iChannel ] = iParam1;
                        }
                        break;
                    default:
                        break;
                }
            }
        }
    }
    while ( iMaxSize - iTotal > 0 && iCount > 0 &&
            ( !pEvent || pEvent->GetEventType() != MIDIEvent::MetaEvent ||
              reinterpret_cast< MIDIMetaEvent* >( pEvent )->GetMetaEventType() != MIDIMetaEvent::EndOfTrack ) );

    m_iRowEnd = m_MIDI.GetEventPoolCount();
    return iTotal;
}

void MIDITrack::MIDITrackInfo::AddEventInfo( const MIDIEvent &mEvent )
{
    this->iEventCount++;
    this->iTotalTicks = max( this->iTotalTicks, mEvent.GetAbsT() );

    switch ( mEvent.GetEventType() )
    {
        case MIDIEvent::MetaEvent:
        {
            const MIDIMetaEvent &mMetaEvent = reinterpret_cast< const MIDIMetaEvent & >( mEvent );
            MIDIMetaEvent::MetaEventType eMetaEventType = mMetaEvent.GetMetaEventType();
            switch ( eMetaEventType )
            {
                case MIDIMetaEvent::SetTempo:
                    this->iTempoCount++;
                    break;
                case MIDIMetaEvent::SequenceName:
                    this->sSequenceName.assign( reinterpret_cast< char* >( mMetaEvent.GetData() ), mMetaEvent.GetDataLen() );
                    break;
                case MIDIMetaEvent::SequenceNumber:
                    if ( mMetaEvent.GetDataLen() == 2)
                        MIDI::Parse16Bit( mMetaEvent.GetData(), 2, &this->iSequenceNumber );
                    break;
                default:
                    break;
            }
            break;
        }
        case MIDIEvent::ChannelEvent:
            break; // Channel event info is accumulated inline in ParseEvents (no MIDIEvent object exists anymore)
        default:
            break;
    }
}


void MIDIEvent::DestroyInPlace( MIDIEvent *pEvent )
{
    if ( !pEvent ) return;
    if ( pEvent->GetEventType() == MetaEvent ) static_cast< MIDIMetaEvent* >( pEvent )->~MIDIMetaEvent();
    else static_cast< MIDISysExEvent* >( pEvent )->~MIDISysExEvent();
}

void MIDIEvent::DestroyHeap( MIDIEvent *pEvent )
{
    if ( !pEvent ) return;
    if ( pEvent->GetEventType() == MetaEvent ) delete static_cast< MIDIMetaEvent* >( pEvent );
    else delete static_cast< MIDISysExEvent* >( pEvent );
}

MIDIEvent::EventType MIDIEvent::DecodeEventType( int iEventCode )
{
    if ( iEventCode < 0x80 ) return RunningStatus;
    if ( iEventCode < 0xF0 ) return ChannelEvent;
    if ( iEventCode < 0xFF ) return SysExEvent;
    return MetaEvent;
}

int MIDIEvent::MakeNextEvent( MIDI& midi, const unsigned char *pcData, size_t iMaxSize, int iTrack,
                              uint32_t *piAbsTicks, int *piPrevEventCode, MIDIChannelEvent *pPoolRow,
                              MIDIEvent **pOutEvent )
{
    *pPoolRow = UINT32_MAX;
    *pOutEvent = NULL;

    uint32_t iDT;
    uint32_t iTotal = MIDI::ParseVarNum ( pcData, iMaxSize, &iDT );
    if (iTotal == 0 || iMaxSize - iTotal < 1 ) return 0;

    int iEventCode = pcData[iTotal];
    EventType eEventType = DecodeEventType( iEventCode );
    iTotal++;

    if ( eEventType == RunningStatus && *piPrevEventCode != 0)
    {
        iEventCode = *piPrevEventCode;
        eEventType = DecodeEventType( iEventCode );
        iTotal--;
    }
    if ( eEventType == RunningStatus ) return 0;

    *piAbsTicks += iDT;

    switch ( eEventType )
    {
        case ChannelEvent: *pPoolRow = midi.AppendChannelEvent( iTrack, *piAbsTicks ); break;
        case MetaEvent: *pOutEvent = new MIDIMetaEvent(); break;
        case SysExEvent: *pOutEvent = new MIDISysExEvent(); break;
        default: return 0;
    }

    if ( *pPoolRow != UINT32_MAX )
        midi.SetEventCode( *pPoolRow, (unsigned char)iEventCode );
    else if ( *pOutEvent )
    {
        (*pOutEvent)->m_eEventType = eEventType;
        (*pOutEvent)->m_iEventCode = (unsigned char)iEventCode;
        (*pOutEvent)->m_iTrack = iTrack;
        (*pOutEvent)->m_iAbsT = (int)*piAbsTicks;
    }

    if ( *pPoolRow != UINT32_MAX )
        *piPrevEventCode = iEventCode;

    return iTotal;
}

int MIDIMetaEvent::ParseEvent( const unsigned char *pcData, size_t iMaxSize )
{
    if ( iMaxSize < 1 ) return 0;

    m_eMetaEventType = static_cast< MetaEventType >( pcData[0] );
    uint32_t iCount = MIDI::ParseVarNum( pcData + 1, iMaxSize - 1, &m_iDataLen );
    if ( iCount == 0 || iMaxSize < 1 + iCount + m_iDataLen ) return 0;

    if ( m_iDataLen > sizeof(m_aInline) )
    {
        m_pcData = new unsigned char[m_iDataLen];
        memcpy( m_pcData, pcData + 1 + iCount, m_iDataLen );
    }
    else if ( m_iDataLen > 0 )
        memcpy( m_aInline, pcData + 1 + iCount, m_iDataLen );

    return 1 + iCount + m_iDataLen;
}

int MIDISysExEvent::ParseEvent( const unsigned char *pcData, size_t iMaxSize )
{
    if ( iMaxSize < 1 ) return 0;

    uint32_t iCount = MIDI::ParseVarNum( pcData, iMaxSize, &m_iDataLen );
    if ( iCount == 0 || iMaxSize < iCount + m_iDataLen ) return 0;

    if ( m_iDataLen > 0 )
    {
        m_pcData = new unsigned char[m_iDataLen];
        memcpy( m_pcData, pcData + iCount, m_iDataLen );
        if ( m_iEventCode == 0xF0 && m_pcData[ m_iDataLen - 1 ] != 0xF7 )
            m_bHasMoreData = true;
    }

    return iCount + m_iDataLen;
}



uint32_t MIDI::ParseVarNum( const unsigned char *pcData, size_t iMaxSize, uint32_t *piOut )
{
    if ( !pcData || !piOut || iMaxSize <= 0 )
        return 0;

    *piOut = 0;
    uint32_t i = 0;
    do
    {
        *piOut = ( *piOut << 7 ) | ( pcData[i] & 0x7F );
        i++;
    }
    while ( i < 4 && i < iMaxSize && ( pcData[i - 1] & 0x80 ) );

    return i;
}

uint32_t MIDI::Parse32Bit( const unsigned char *pcData, size_t iMaxSize, uint32_t *piOut )
{
    if ( !pcData || !piOut || iMaxSize < 4 )
        return 0;

    *piOut = pcData[0];
    *piOut = ( *piOut << 8 ) | pcData[1];
    *piOut = ( *piOut << 8 ) | pcData[2];
    *piOut = ( *piOut << 8 ) | pcData[3];

    return 4;
}

uint32_t MIDI::Parse24Bit( const unsigned char *pcData, size_t iMaxSize, uint32_t *piOut )
{
    if ( !pcData || !piOut || iMaxSize < 3 )
        return 0;

    *piOut = pcData[0];
    *piOut = ( *piOut << 8 ) | pcData[1];
    *piOut = ( *piOut << 8 ) | pcData[2];

    return 3;
}

uint32_t MIDI::Parse16Bit( const unsigned char *pcData, size_t iMaxSize, uint32_t *piOut )
{
    if ( !pcData || !piOut || iMaxSize < 2 )
        return 0;

    *piOut = pcData[0];
    *piOut = ( *piOut << 8 ) | pcData[1];

    return 2;
}

uint32_t MIDI::ParseNChars( const unsigned char *pcData, size_t iNChars, size_t iMaxSize, char *pcOut )
{
    if ( !pcData || !pcOut || iMaxSize <= 0 )
        return 0;

    size_t iSize = min( iNChars, iMaxSize );
    memcpy( pcOut, pcData, iSize );

    return iSize;
}


int MIDIOutDevice::GetNumDevs() const
{
    return midiOutGetNumDevs();
}

wstring MIDIOutDevice::GetDevName( int iDev ) const
{
    MIDIOUTCAPS moc;
    if ( midiOutGetDevCaps( iDev, &moc, sizeof( MIDIOUTCAPS ) ) == MMSYSERR_NOERROR )
        return moc.szPname;
    return wstring();
}

bool MIDIOutDevice::Open( int iDev )
{
    if ( m_bIsOpen ) Close();
    m_sDevice = GetDevName( iDev );
    m_bIsKDMAPI = false;

    MMRESULT mmResult = midiOutOpen( &m_hMIDIOut, iDev, ( DWORD_PTR )MIDIOutProc, ( DWORD_PTR )this, CALLBACK_FUNCTION );
    m_bIsOpen = ( mmResult == MMSYSERR_NOERROR );
    return m_bIsOpen;
}

bool MIDIOutDevice::OpenKDMAPI() {
    if (m_bIsOpen)
        Close();

    m_sDevice = L"KDMAPI";
    m_bIsKDMAPI = true;

    auto InitializeKDMAPIStream = (int(WINAPI*)())GetOmniMIDIProc("InitializeKDMAPIStream");
    *(FARPROC*)&SendDirectData = GetOmniMIDIProc("SendDirectData");
    return m_bIsOpen = (SendDirectData && InitializeKDMAPIStream && InitializeKDMAPIStream());
}

void MIDIOutDevice::Close()
{
    if ( !m_bIsOpen ) return;

    if (m_bIsKDMAPI) {
        auto TerminateKDMAPIStream = (int(WINAPI*)())GetOmniMIDIProc("TerminateKDMAPIStream");
        if (TerminateKDMAPIStream)
            TerminateKDMAPIStream();
    } else {
        midiOutReset(m_hMIDIOut);
        midiOutClose(m_hMIDIOut);
    }
    m_bIsOpen = false;
}

void MIDIOutDevice::Reset()
{
    if (!m_bIsOpen) return;

    if (m_bIsKDMAPI) {
        auto ResetKDMAPIStream = (void(WINAPI*)())GetOmniMIDIProc("ResetKDMAPIStream");
        if (ResetKDMAPIStream)
            ResetKDMAPIStream();
    } else {
        midiOutReset(m_hMIDIOut);
    }
}

void MIDIOutDevice::AllNotesOff()
{
    PlayEventAcrossChannels( 0xB0, 0x7B, 0x00 ); // All notes off
    PlayEventAcrossChannels( 0xB0, 0x40, 0x00 ); // Sustain off
}

void MIDIOutDevice::AllNotesOff( const vector< int > &vChannels )
{
    PlayEventAcrossChannels( 0xB0, 0x7B, 0x00, vChannels );
    PlayEventAcrossChannels( 0xB0, 0x40, 0x00, vChannels );
}

void MIDIOutDevice::SetVolume( double dVolume )
{
    if (!m_bIsKDMAPI) {
        DWORD dwVolume = static_cast<DWORD>(0xFFFF * dVolume + 0.5);
        midiOutSetVolume(m_hMIDIOut, dwVolume | (dwVolume << 16));
    }
}

bool MIDIOutDevice::PlayEventAcrossChannels( unsigned char cStatus, unsigned char cParam1, unsigned char cParam2 )
{
    if ( !m_bIsOpen ) return false;

    cStatus &= 0xF0;
    bool bResult = true;
    for ( int i = 0; i < 16; i++ )
        bResult &= PlayEvent( cStatus + i, cParam1, cParam2 );

    return bResult;
}

bool MIDIOutDevice::PlayEventAcrossChannels( unsigned char cStatus, unsigned char cParam1, unsigned char cParam2, const vector< int > &vChannels )
{
    if ( !m_bIsOpen ) return false;

    cStatus &= 0xF0;
    bool bResult = true;
    for ( vector< int >::const_iterator it = vChannels.begin(); it != vChannels.end(); ++it )
        bResult &= PlayEvent( cStatus + *it, cParam1, cParam2 );

    return bResult;
}

bool MIDIOutDevice::PlayEvent( unsigned char cStatus, unsigned char cParam1, unsigned char cParam2 )
{
    if ( !m_bIsOpen ) return false;
    if (m_bIsKDMAPI) {
        SendDirectData((cParam2 << 16) + (cParam1 << 8) + cStatus);
        m_ullEventsSent++;
        return true;
    } else {
        bool bOk = midiOutShortMsg(m_hMIDIOut, (cParam2 << 16) + (cParam1 << 8) + cStatus) == MMSYSERR_NOERROR;
        m_ullEventsSent++;
        if (!bOk)
            m_ullSendFailures++;
        return bOk;
    }
}

FARPROC MIDIOutDevice::GetOmniMIDIProc(const char* func) {
    auto dll = GetModuleHandle(L"OmniMIDI");
    if (!dll)
        dll = LoadLibrary(L"OmniMIDI");
    return GetProcAddress(dll, func);
}

void CALLBACK MIDIOutDevice::MIDIOutProc(HMIDIOUT, UINT wMsg, DWORD_PTR, DWORD_PTR, DWORD_PTR)
{
    switch ( wMsg )
    {
        case MOM_CLOSE:
        {
        }
    }
}
