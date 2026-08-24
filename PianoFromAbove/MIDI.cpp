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
#include <fstream>
#include <stack>
#include <array>
#include <exception>
#include <ppl.h>
#include <thread>
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


MIDI::MIDI ( const wstring &sFilename )
{
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
        if (m_pcOwnedData)
        {
            delete[] m_pcOwnedData;
            m_pcOwnedData = nullptr;
        }
        clear();
        Sleep(300);
        InitFromFileCore(sFilename);
    }
}

void MIDI::InitFromFileCore( const wstring &sFilename )
{
    FILE* stream = nullptr;

    errno_t eOpen = _wfopen_s(&stream, sFilename.c_str(), L"rb");
    PRE_DbgLog("InitFromFileCore: open '%ls' -> %d", sFilename.c_str(), eOpen);
    if (eOpen == 0)
    {
        _fseeki64(stream, 0, SEEK_END);
        size_t iSize = static_cast<size_t>(_ftelli64(stream));
        unsigned char* pcMemBlock = nullptr;

        if (_fseeki64(stream, 0, SEEK_SET)) {
            MessageBoxA(NULL, "_fseeki64 encountered an error.", "Piano From Above", MB_OK | MB_ICONERROR);
            return;
        }

        constexpr uint8_t lzma_magic[] = {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00};
        unsigned char aPeek[6] = {};
        size_t iPeek = fread(aPeek, 1, sizeof(aPeek), stream);
        _fseeki64(stream, 0, SEEK_SET);
        bool bIsLzma = iPeek == sizeof(aPeek) && !memcmp(aPeek, lzma_magic, sizeof(lzma_magic));

        // cache pages mid-parse, causing re-reads; a single linear read is
        if (!bIsLzma)
        {
            unsigned char* pBuf = nullptr;
            try { pBuf = new unsigned char[iSize]; }
            catch ( const std::bad_alloc & ) { pcMemBlock = nullptr; }
            if (pBuf)
            {
                fread(reinterpret_cast<char*>(pBuf), 1, iSize, stream);
                pcMemBlock = pBuf;
            }
        }
        if (!pcMemBlock)
        {
            pcMemBlock = new unsigned char[iSize];
            fread(reinterpret_cast<char*>(pcMemBlock), 1, iSize, stream);
        }

        fclose(stream);

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
            lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED);
            strm.next_in = compressed;
            strm.avail_in = iSize;
            bool done = false;
            lzma_action action = LZMA_RUN;
            while (!done) {
                if (strm.avail_in == 0)
                    action = LZMA_FINISH;
                lzma_ret ret = lzma_code(&strm, action);
                if (strm.avail_out == 0) {
                    auto remaining = min(decompressed_size - (write_ptr - pcMemBlock), 1 << 20);
                    strm.next_out = write_ptr;
                    strm.avail_out = remaining;
                    g_LoadingProgress.progress = write_ptr - pcMemBlock;
                    write_ptr += remaining;
                }
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
        ParseMIDI(pcMemBlock, iSize);
        m_Info.sFilename = sFilename;

        // heap blocks; if the parse didn't already take ownership and free it
        if (m_pcOwnedData)
        {
            delete[] m_pcOwnedData;
            m_pcOwnedData = nullptr;
        }
    }
}

MIDI::~MIDI( void )
{
    if ( m_pcOwnedData )
    {
        delete[] m_pcOwnedData;
        m_pcOwnedData = nullptr;
    }
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
    return static_cast< MIDIChannelEvent >( m_vTicks.size() - 1 + m_vThinTicks.size() );
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
           m_vThinTicks.size() * 4 * sizeof( uint32_t );
}

size_t MIDI::GetEventPoolCount() const
{
    return m_vTicks.size() + m_vThinTicks.size();
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
    vector<int64_t>().swap( m_vTimes );
    vector<uint32_t>().swap( m_vTicks );
    vector<uint32_t>().swap( m_vLengths );
    vector<uint32_t>().swap( m_vSisters );
    vector<uint16_t>().swap( m_vSimult );
    vector<uint16_t>().swap( m_vEventTrack );
    vector<uint32_t>().swap( m_vPack );
    vector<uint32_t>().swap( m_vThinTicks );
    vector<uint32_t>().swap( m_vThinOwners );
    vector<uint32_t>().swap( m_vThinSisters );
    vector<uint32_t>().swap( m_vThinLengths );
    m_iFullRows = 0;
}

void MIDI::ReleaseOwnedData( void )
{
    if ( m_pcOwnedData )
    {
        delete[] m_pcOwnedData;
        m_pcOwnedData = nullptr;
    }
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
    size_t iRet = iTotal + ParseTracks( pcData + iTotal, iMaxSize - iTotal );
    // bytes; free the owned copy now so the merged list doesn't share the load
    if ( m_pcOwnedData )
    {
        delete[] m_pcOwnedData;
        m_pcOwnedData = nullptr;
    }
    LogLoadStage("ParseTracks end", this);
    return iRet;
}

namespace
{
    // nothing row-sized; phase 2 (pMIDI != nullptr) runs the identical walk and
    size_t ParseTrackChunk( const unsigned char *pcData, size_t iMaxSize, size_t iTrack, size_t iRowBase,
                            MIDI *pMIDI, MIDITrack::MIDITrackInfo &ti, vector< MIDIEvent* > &vMetas,
                            size_t iThinBase, size_t &iThinOut )
{
        ti.iSequenceNumber = (uint32_t)iTrack;
        int iPrevEventCode = 0;
        uint32_t iAbsTicks = 0;
        size_t iTotal = 0, nRows = 0, nThin = 0;
        int iCount = 0;
        MIDIEvent *pEvent = NULL;
        // row is written; phase 2 writes each kept row at its compacted id and
        array< array< stack< MIDIChannelEvent >, 128 >, 16 > vStacks;
        do
        {
            iCount = 0;
            pEvent = NULL;
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
                    nRows++;
                    MIDIChannelEvent iKeepId = (MIDIChannelEvent)( iRowBase + nRows - nThin - 1 );
                    MIDI::ChannelEventType eType = (MIDI::ChannelEventType)( ( uPack >> 4 ) & 0xF );
                    int iChannel = (int)( uPack & 0xF );
                    int iParam1 = (int)( ( uPack >> 8 ) & 0xFF );
                    int iParam2 = (int)( ( uPack >> 16 ) & 0xFF );
                    auto &sStack = vStacks[ iChannel ][ iParam1 ];
                    if ( eType == MIDI::NoteOn && iParam2 > 0 )
                    {
                        if ( pMIDI )
                        {
                            pMIDI->SetPoolRow( iKeepId, iAbsTicks, 0, UINT32_MAX, 0, (uint16_t)iTrack, uPack );
                            sStack.push( iKeepId );
                        }
                        else
                            sStack.push( 0 );
                    }
                    else if ( eType == MIDI::NoteOff || eType == MIDI::NoteOn )
                    {
                        if ( !sStack.empty() )
                        {
                            MIDIChannelEvent iOwner = sStack.top();
                            sStack.pop();
                            if ( pMIDI )
                            {
                                pMIDI->SetThinRow( iThinBase + nThin, iAbsTicks,
                                    (uint32_t)iOwner | ( ( eType == MIDI::NoteOn ) ? MIDI::THIN_OWNER_NOTEON_FLAG : 0 ),
                                    UINT32_MAX, 0 );
                            }
                            nThin++;
                        }
                        else if ( pMIDI )
                        {
                            pMIDI->SetPoolRow( iKeepId, iAbsTicks, 0, UINT32_MAX, 0, (uint16_t)iTrack, uPack );
                        }
                    }
                    else if ( pMIDI )
                    {
                        pMIDI->SetPoolRow( iKeepId, iAbsTicks, 0, UINT32_MAX, 0, (uint16_t)iTrack, uPack );
                    }

                    if ( !bParams ) break;
                    iTotal += iDTCode + iNumParams;
                    iCount = iNumParams;

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
                    pEvent = ( eEventType == MIDIEvent::MetaEvent ) ? (MIDIEvent*)new MIDIMetaEvent() : (MIDIEvent*)new MIDISysExEvent();
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
                        ti.AddEventInfo( *pEvent );
                        if ( pMIDI )
                            vMetas.push_back( pEvent );
                        else
                            delete pEvent;
                    }
                    else
                        delete pEvent;
                    break;
                }
                default:
                    break;
            }
            if ( eEventType == MIDIEvent::ChannelEvent )
                iPrevEventCode = iEventCode;
        }
        while ( iMaxSize - iTotal > 0 && iCount > 0 &&
                ( !pEvent || pEvent->GetEventType() != MIDIEvent::MetaEvent ||
                  reinterpret_cast< MIDIMetaEvent* >( pEvent )->GetMetaEventType() != MIDIMetaEvent::EndOfTrack ) );
        iThinOut = nThin;
        return nRows;
    }
}

// Writes one channel-event row into the pool columns (parallel phase-2 walk;
void MIDI::SetPoolRow( size_t iRow, uint32_t iTicks, uint32_t iLengths, uint32_t iSisters,
                       uint32_t iSimult, uint16_t iEventTrack, uint32_t iPack )
{
    m_vTimes[iRow] = 0;
    m_vTicks[iRow] = iTicks;
    m_vLengths[iRow] = iLengths;
    m_vSisters[iRow] = iSisters;
    m_vSimult[iRow] = iSimult;
    m_vEventTrack[iRow] = iEventTrack;
    m_vPack[iRow] = iPack;
}

void MIDI::SetThinRow( size_t iThin, uint32_t iTicks, uint32_t iOwners, uint32_t iSisters,
                       uint32_t iLengths )
{
    m_vThinTicks[iThin] = iTicks;
    m_vThinOwners[iThin] = iOwners;
    m_vThinSisters[iThin] = iSisters;
    m_vThinLengths[iThin] = iLengths;
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
    while ( iMaxSize - iTotal > 0 && iCount > 0 && m_Info.iFormatType != 2 );

    const size_t nChunks = vOffsets.size();
    vector< MIDITrack::MIDITrackInfo > vTrackInfo( nChunks );
    vector< size_t > vRows( nChunks );
    vector< size_t > vThin( nChunks );
    vector< exception_ptr > vErrors( nChunks );
    PRE_DbgLog("LOAD [ParseTracks chunks=%zu]", nChunks);
    unsigned int nThreads = std::thread::hardware_concurrency();
    if ( nThreads == 0 ) nThreads = 4;
    nThreads = min( nThreads, (unsigned int)min< size_t >( nChunks, 16 ) );
    if ( nThreads == 0 ) nThreads = 1;
    {
        atomic< size_t > iNext( 0 );
        atomic< size_t > iDone( 0 );
        vector< thread > vThreads;
        vThreads.reserve( nThreads );
        for ( unsigned int t = 0; t < nThreads; t++ )
            vThreads.emplace_back( [&]() {
                size_t i;
                while ( ( i = iNext.fetch_add( 1, memory_order_relaxed ) ) < nChunks )
                {
                    vector< MIDIEvent* > scratch;
                    scratch.reserve( 64 );
                    try
                    {
                        vRows[i] = ParseTrackChunk( pcData + vOffsets[i] + 8, iMaxSize - vOffsets[i] - 8,
                                                    i, 0, nullptr, vTrackInfo[i], scratch, 0, vThin[i] );
                    }
                    catch ( ... )
                    {
                        vErrors[i] = current_exception();
                    }
                    g_LoadingProgress.progress = min( (uint64_t)iDone.fetch_add( 1, memory_order_relaxed ) + 1, g_LoadingProgress.max );
                }
            } );
        for ( auto &th : vThreads )
            th.join();
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
        iTotalRows += vRows[c] - vThin[c];
        vThinBases[c] = iThinTotal;
        iThinTotal += vThin[c];
    }
    m_iFullRows = (uint32_t)iTotalRows;
    PRE_DbgLog("LOAD [ParseTracks rows=%zu thin=%zu]", iTotalRows, iThinTotal);

    try
    {
        m_vTimes.reserve( iTotalRows );
        m_vTicks.reserve( iTotalRows );
        m_vLengths.reserve( iTotalRows );
        m_vSisters.reserve( iTotalRows );
        m_vSimult.reserve( iTotalRows );
        m_vEventTrack.reserve( iTotalRows );
        m_vPack.reserve( iTotalRows );
        m_vThinTicks.reserve( iThinTotal );
        m_vThinOwners.reserve( iThinTotal );
        m_vThinSisters.reserve( iThinTotal );
        m_vThinLengths.reserve( iThinTotal );
        PRE_DbgLog("LOAD [ParseTracks reserve done]");
    }
    catch ( const std::exception &e )
    {
        PRE_DbgLog("LOAD [ParseTracks reserve FAILED: %s]", e.what());
        throw;
    }

    m_vTimes.resize( iTotalRows );
    m_vTicks.resize( iTotalRows );
    m_vLengths.resize( iTotalRows );
    m_vSisters.resize( iTotalRows );
    m_vSimult.resize( iTotalRows );
    m_vEventTrack.resize( iTotalRows );
    m_vPack.resize( iTotalRows );
    m_vThinTicks.resize( iThinTotal );
    m_vThinOwners.resize( iThinTotal );
    m_vThinSisters.resize( iThinTotal );
    m_vThinLengths.resize( iThinTotal );

    // worker owns a disjoint full and thin range, so no locks are needed; the
    vector< vector< MIDIEvent* > > vMetasAll( nChunks );
    {
        atomic< size_t > iNext( 0 );
        atomic< size_t > iDone2( 0 );
        vector< thread > vThreads;
        vThreads.reserve( nThreads );
        for ( unsigned int t = 0; t < nThreads; t++ )
            vThreads.emplace_back( [&]() {
                size_t i;
                while ( ( i = iNext.fetch_add( 1, memory_order_relaxed ) ) < nChunks )
                {
                    vector< MIDIEvent* > metas;
                    metas.reserve( 64 );
                    MIDITrack::MIDITrackInfo tiThrowaway;
                    size_t iThinDummy = 0;
                    try
                    {
                        size_t n = ParseTrackChunk( pcData + vOffsets[i] + 8, iMaxSize - vOffsets[i] - 8,
                                                    i, vFullBases[i], this, tiThrowaway, metas,
                                                    vThinBases[i], iThinDummy );
                        if ( n != vRows[i] )
                            PRE_DbgLog("LOAD [ParseTracks count mismatch track=%zu: %zu vs %zu]", i, n, vRows[i]);
                        if ( iThinDummy != vThin[i] )
                            PRE_DbgLog("LOAD [ParseTracks thin mismatch track=%zu: %zu vs %zu]", i, iThinDummy, vThin[i]);
                        vMetasAll[i].swap( metas );
                    }
                    catch ( ... )
                    {
                        vErrors[i] = current_exception();
                    }
                    g_LoadingProgress.progress = min( nChunks + (uint64_t)iDone2.fetch_add( 1, memory_order_relaxed ) + 1, g_LoadingProgress.max );
                }
            } );
        for ( auto &th : vThreads )
            th.join();
        for ( size_t c = 0; c < nChunks; c++ )
            if ( vErrors[c] )
                rethrow_exception( vErrors[c] );
    }

    for ( size_t c = 0; c < nChunks; c++ )
    {
        MIDITrack *track = new MIDITrack( *this );
        track->m_iRowStart = vFullBases[c];
        track->m_iRowEnd = vFullBases[c] + vRows[c] - vThin[c];
        track->m_iThinStart = vThinBases[c];
        track->m_iThinEnd = vThinBases[c] + vThin[c];
        track->m_TrackInfo = std::move( vTrackInfo[c] );
        track->m_vMetas = std::move( vMetasAll[c] );
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
                    SetEventLength(iRow, (uint32_t)max(0LL, llTime - m_vTimes[iOwner]));
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
    g_LoadingProgress.max = m_vTracks.size();

    // Connect just links both directions on the row ids; PostProcess later
    concurrency::parallel_for(size_t(0), m_vTracks.size(), [&](int track) {
        MIDITrack* pTrack = m_vTracks[track];
        for (size_t t = pTrack->GetThinStart(); t < pTrack->GetThinEnd(); t++) {
            MIDIChannelEvent iThin = (MIDIChannelEvent)(m_iFullRows + t);
            MIDIChannelEvent iOwner = GetThinOwner(iThin);
            SetEventSisterIdx(iThin, iOwner);
            SetEventSisterIdx(iOwner, iThin);
        }
        g_LoadingProgress.progress++;
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
        delete* it;
    m_vMetas.clear();
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
                    delete pEvent;
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
