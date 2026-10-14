/*************************************************************************************************
*
* File: MIDI.h
*
* Description: Defines the MIDI objects
*
* Copyright (c) 2010 Brian Pantano. All rights reserved.
*
*************************************************************************************************/
#pragma once

#include <Windows.h>
#include <vector>
#include <string>
#include <map>
#include <atomic>
#include <stdint.h>
#include <climits>
#include <stdexcept>
#include <intrin.h>
#include <memory>
using namespace std;

#include "Misc.h"

// Classes defined in this file
class MIDI;
class MIDITrack;
class MIDIEvent;
class MIDIMetaEvent;
class MIDISysExEvent;
class MIDIPos;
class ParallelMIDIPos;
namespace PGFAPacked { struct PackedSong; }

// the MIDI object (SoA). The handle itself is 4 bytes; before this change every
typedef uint32_t MIDIChannelEvent;

#define CHANNEL_EVENT_FLAG_PASS_DONE (1u << 24)
// MIDI File Classes


class MIDIPos
{
public:
    MIDIPos( MIDI &midi );
    ~MIDIPos();

    // *pOutMeta left NULL; meta/sysex events with *pOutMeta set and *pOutRow
    int GetNextEvent( int iMicroSecs, MIDIEvent **pOutMeta, MIDIChannelEvent *pOutRow );

    bool IsStandard() const { return m_bIsStandard; }
    int GetTicksPerBeat() const { return m_iTicksPerBeat; }
    int GetTicksPerSecond() const { return m_iTicksPerSecond; }
    int GetMicroSecsPerBeat() const { return m_iMicroSecsPerBeat; }

    int* m_pTrackTime;

private:
    MIDI &m_MIDI;
    vector< size_t > m_vRowPos;
    vector< size_t > m_vThinPos;
    vector< size_t > m_vMetaPos;

    bool m_bIsStandard;
    uint32_t m_iTicksPerBeat, m_iMicroSecsPerBeat; // For standard division
    int m_iTicksPerSecond; // For SMPTE division

    int m_iCurrTick;
    int m_iCurrMicroSec;

    void PrimeTime( size_t iTrack );
};

typedef vector< pair< long long, int > > eventvec_t;

// std::vector whose resize() leaves new elements uninitialized. The event pool columns are
// filled in full by the parallel parse, so zeroing hundreds of MB on one thread first (and
// touching every page from that thread) was pure overhead.
template< typename T >
struct DefaultInitAllocator : std::allocator< T >
{
    template< typename U > struct rebind { using other = DefaultInitAllocator< U >; };
    DefaultInitAllocator() = default;
    template< typename U > DefaultInitAllocator( const DefaultInitAllocator< U >& ) noexcept {}
    template< typename U > void construct( U *p ) noexcept { ::new ( (void*)p ) U; }
    template< typename U, typename... Args > void construct( U *p, Args&&... args ) { ::new ( (void*)p ) U( std::forward< Args >( args )... ); }
};
template< typename T > using PoolVec = std::vector< T, DefaultInitAllocator< T > >;

class MIDI
{
public:
    enum Note { A, AS, B, C, CS, D, DS, E, F, FS, G, GS };

    static const int KEYS = 129; // One extra because 128th is a sharp
    static const int C8 = 108;
    static const int C4 = C8 - 4 * 12;
    static const int A0 = C8 - 7 * 12 - 3;
    static const int Drums = 0x09;
    static const wstring Instruments[129];
    static const wstring &NoteName( int iNote );
    static Note NoteVal( int iNote );
    static bool IsSharp( int iNote );
    static int WhiteCount( int iMinNote, int iMaxNote );

    static uint32_t ParseVarNum( const unsigned char *pcData, size_t iMaxSize, uint32_t *piOut );
    static uint32_t Parse32Bit( const unsigned char *pcData, size_t iMaxSize, uint32_t *piOut );
    static uint32_t Parse24Bit( const unsigned char *pcData, size_t iMaxSize, uint32_t *piOut );
    static uint32_t Parse16Bit( const unsigned char *pcData, size_t iMaxSize, uint32_t *piOut );
    static uint32_t ParseNChars( const unsigned char *pcData, size_t iNChars, size_t iMaxSize, char *pcOut );

    MIDI( void ) {};
    MIDI( const wstring &sFilename );
    ~MIDI( void );

    // Reads + decompresses a file, then parses; wrapped in try/catch by the
    void InitFromFile( const wstring &sFilename );
    void InitFromFileCore( const wstring &sFilename ); // one load attempt

    MIDIChannelEvent AppendChannelEvent(int iTrack, uint32_t iAbsTicks);
    size_t GetEventPoolBytes() const;
    size_t GetEventPoolCount() const;

    // own storage is just {tick, owner, sister, length} and every other
    enum ChannelEventType { NoteOff = 0x8, NoteOn, NoteAftertouch, Controller, ProgramChange, ChannelAftertouch, PitchBend };
    static const uint32_t THIN_OWNER_NOTEON_FLAG = 0x80000000u;
    static const uint32_t THIN_SISTER_PASS_FLAG = 0x80000000u;
    static const uint16_t THIN_DELTA_ESC = 0xFFFF;
    inline bool IsThinRow(MIDIChannelEvent row) const
    {
        return row >= m_iFullRows && row < (MIDIChannelEvent)(m_iFullRows + m_vThinOwners.size());
    }
    // A folded note-off's absolute tick. Stored as a 16-bit offset from its note-on's tick (0xFFFF =
    // longer note: exact tick in the sorted escape table); in "wide" mode (files full of very long
    // notes, where the table would cost more than it saves) it is a plain 32-bit column instead.
    inline uint32_t GetThinTick(size_t thin) const
    {
        if ( m_bThinWide ) return m_vThinTicks[thin];
        const uint16_t d = m_vThinDelta[thin];
        if ( d != THIN_DELTA_ESC )
            return m_vTicks[ m_vThinOwners[thin] & ~THIN_OWNER_NOTEON_FLAG ] + d;
        return ThinEscTick( thin );
    }
    uint32_t ThinEscTick(size_t thin) const;
    inline MIDIChannelEvent GetThinOwner(MIDIChannelEvent row) const
    {
        return (MIDIChannelEvent)(m_vThinOwners[row - m_iFullRows] & ~THIN_OWNER_NOTEON_FLAG);
    }
    // Tempo map used to derive row times from ticks when there are only a few tempo events (the
    // common case for huge files): the 8-byte per-row time column then isn't stored at all.
    static const size_t MaxComputedAnchors = 7; // slot 7 is always an INT_MAX sentinel
    __forceinline int64_t TimeFromTick( uint32_t iTicks ) const
    {
        const int iTick = (int)iTicks;
        // Index of the last anchor whose tick <= iTick. The lookup keys are sorted (slot 0 is INT_MIN,
        // unused slots INT_MAX), so the keys greater than iTick form a suffix that always includes the
        // sentinel; its first slot is the answer + 1.
        const __m128i vTick = _mm_set1_epi32( iTick == INT_MAX ? INT_MAX - 1 : iTick );
        const unsigned uGt = (unsigned)_mm_movemask_ps( _mm_castsi128_ps( _mm_cmpgt_epi32( _mm_loadu_si128( (const __m128i*)&m_aAnchorKey[0] ), vTick ) ) ) |
                             ( (unsigned)_mm_movemask_ps( _mm_castsi128_ps( _mm_cmpgt_epi32( _mm_loadu_si128( (const __m128i*)&m_aAnchorKey[4] ), vTick ) ) ) << 4 );
        unsigned long iFirstGt;
        _BitScanForward( &iFirstGt, uGt );
        const size_t a = iFirstGt - 1;

        const int64_t iNum = m_aAnchorMul[a] * ( (int64_t)iTick - m_aAnchorTick[a] );
        // Numerators outside [0, 2^57) (negative ticks, or no usable magic: m_uNumLimit == 0) take the
        // plain divide, which truncates exactly like the stored-time path did.
        if ( (unsigned __int64)iNum >= m_uNumLimit )
            return m_iTimeDivisor ? m_aAnchorTime[a] + iNum / m_iTimeDivisor : m_aAnchorTime[a];
        // Exactly iNum / divisor without a hardware divide (~40 cycles, and this runs per event in the
        // hot loops): multiply by a precomputed magic number and shift (Granlund-Montgomery).
        unsigned __int64 iHi;
        _umul128( (unsigned __int64)iNum, m_uTimeMagic, &iHi );
        return m_aAnchorTime[a] + (int64_t)( iHi >> m_iTimeShift );
    }
    // Picks the magic number/shift for dividing numerators < 2^57 by iDiv: with 2^L >= iDiv and
    // m = ceil(2^(57+L) / iDiv), floor(x / iDiv) == (x * m) >> (57 + L) for every x < 2^57.
    // L is at least 7 so the shift is >= 64 (just the high word of the product); iDiv == 1 would
    // need a 65-bit m and uses the plain divide instead (m == 0).
    void SetTimeDivisor( int iDiv )
    {
        m_uTimeMagic = 0;
        m_iTimeShift = 0;
        m_uNumLimit = 0;
        m_iTimeDivisor = iDiv > 0 ? iDiv : 0;
        if ( iDiv <= 1 ) return;
        unsigned L = 7;
        while ( ( 1u << L ) < (unsigned)iDiv ) L++;
        const unsigned k = 57 + L;
        unsigned __int64 q = 0, r = 1;
        for ( unsigned i = 0; i < k; i++ )
        {
            r <<= 1; q <<= 1;
            if ( r >= (unsigned)iDiv ) { r -= iDiv; q |= 1; }
        }
        if ( r != 0 ) q++;
        m_uTimeMagic = q;
        m_iTimeShift = k - 64;
        m_uNumLimit = 1ull << 57;
    }
    // iStandard: numerators are microSecsPerBeat * dTicks (divided by ticks per beat); otherwise
    // 1000000 * dTicks (divided by ticks per second).
    void SetTimeAnchors( size_t n, const int *pTicks, const int64_t *pTimes, const uint32_t *pMicroSecsPerBeat )
    {
        for ( size_t i = 0; i <= MaxComputedAnchors; i++ )
        {
            const bool bUsed = i < n && i < MaxComputedAnchors;
            m_aAnchorKey[i] = i == 0 ? INT_MIN : ( bUsed ? pTicks[i] : INT_MAX );
            m_aAnchorTick[i] = bUsed ? pTicks[i] : 0;
            m_aAnchorTime[i] = bUsed ? pTimes[i] : 0;
            m_aAnchorMul[i] = m_bTimeStandard ? (int64_t)( bUsed ? pMicroSecsPerBeat[i] : 500000 ) : 1000000LL;
        }
        m_nAnchors = n;
    }
    // Identical notes (same track, channel, key, velocity, start and end tick) are stored as one row
    // that carries a repeat count in the top 7 bits of its pack word. A folded note-off reports its
    // note-on's count. Anything that sends or counts notes must honour it; visuals draw the row once.
    static const unsigned REPEAT_SHIFT = 25;
    static const unsigned MAX_REPEAT = 128;
    inline unsigned GetEventRepeat(MIDIChannelEvent row) const
    {
        return ( ( m_vPack[ IsThinRow(row) ? GetThinOwner(row) : row ] >> REPEAT_SHIFT ) & 0x7Fu ) + 1;
    }
    inline void AddRowRepeat(MIDIChannelEvent row)
    {
        if ( ( ( m_vPack[row] >> REPEAT_SHIFT ) & 0x7Fu ) == 0x7Fu ) throw std::runtime_error( "MIDI repeat count overflow" );
        m_vPack[row] += 1u << REPEAT_SHIFT;
    }
    inline int64_t GetPoolTime(MIDIChannelEvent row) const { return m_bComputedTimes ? TimeFromTick( m_vTicks[row] ) : m_vTimes[row]; }
    inline int64_t GetEventTime(MIDIChannelEvent row) const
    {
        if ( IsThinRow(row) )
        {
            // A folded note-off ends its note-on's length after the note-on.
            size_t owner = m_vThinOwners[row - m_iFullRows] & ~THIN_OWNER_NOTEON_FLAG;
            return GetPoolTime( (MIDIChannelEvent)owner ) + m_vLengths[owner];
        }
        return GetPoolTime( row );
    }
    inline void SetEventTime(MIDIChannelEvent row, int64_t iTime) { if ( !IsThinRow(row) && !m_bComputedTimes ) m_vTimes[row] = iTime; }
    inline int GetEventAbsT(MIDIChannelEvent row) const { return IsThinRow(row) ? (int)GetThinTick(row - m_iFullRows) : (int)m_vTicks[row]; }
    inline uint32_t GetEventTicks(MIDIChannelEvent row) const { return IsThinRow(row) ? GetThinTick(row - m_iFullRows) : m_vTicks[row]; }
    inline void SetEventTicks(MIDIChannelEvent row, uint32_t iTicks) { if ( !IsThinRow(row) ) m_vTicks[row] = iTicks; }
    // A folded note-off has the same length as its note-on, so it is stored once, on the note-on.
    inline uint32_t GetEventLength(MIDIChannelEvent row) const { return IsThinRow(row) ? m_vLengths[GetThinOwner(row)] : m_vLengths[row]; }
    inline void SetEventLength(MIDIChannelEvent row, uint32_t iLength) { if ( IsThinRow(row) ) m_vLengths[GetThinOwner(row)] = iLength; else m_vLengths[row] = iLength; }
    inline unsigned GetEventSisterIdx(MIDIChannelEvent row) const { return IsThinRow(row) ? (m_vThinSisters[row - m_iFullRows] & ~THIN_SISTER_PASS_FLAG) : m_vSisters[row]; }
    inline void SetEventSisterIdx(MIDIChannelEvent row, unsigned iSister)
    {
        if ( IsThinRow(row) )
        {
            size_t t = row - m_iFullRows;
            m_vThinSisters[t] = (iSister & ~THIN_SISTER_PASS_FLAG) | (m_vThinSisters[t] & THIN_SISTER_PASS_FLAG);
        }
        else
            m_vSisters[row] = iSister;
    }
    inline bool EventHasSister(MIDIChannelEvent row) const { return IsThinRow(row) ? (m_vThinSisters[row - m_iFullRows] & ~THIN_SISTER_PASS_FLAG) != UINT32_MAX : m_vSisters[row] != UINT32_MAX; }
    inline unsigned GetEventSimult(MIDIChannelEvent row) const { return m_vSimult[IsThinRow(row) ? GetThinOwner(row) : row]; }
    inline void SetEventSimult(MIDIChannelEvent row, unsigned iSimult) { if ( !IsThinRow(row) ) m_vSimult[row] = static_cast<uint16_t>(min<unsigned>(iSimult, UINT16_MAX)); }
    inline unsigned short GetEventTrack(MIDIChannelEvent row) const { return m_vEventTrack[IsThinRow(row) ? GetThinOwner(row) : row]; }
    inline void SetEventTrack(MIDIChannelEvent row, unsigned short iTrack) { if ( !IsThinRow(row) ) m_vEventTrack[row] = iTrack; }
    inline unsigned char GetEventCode(MIDIChannelEvent row) const { return IsThinRow(row) ? (unsigned char)((GetEventChannelEventType(row) << 4) | GetEventChannel(row)) : (unsigned char)(m_vPack[row] & 0xFF); }
    inline void SetEventCode(MIDIChannelEvent row, unsigned char iCode) { if ( !IsThinRow(row) ) m_vPack[row] = (m_vPack[row] & 0xFFFFFF00) | iCode; }
    inline unsigned char GetEventParam1(MIDIChannelEvent row) const { return IsThinRow(row) ? GetEventParam1(GetThinOwner(row)) : (unsigned char)((m_vPack[row] >> 8) & 0xFF); }
    inline void SetEventParam1(MIDIChannelEvent row, unsigned char iParam) { if ( !IsThinRow(row) ) m_vPack[row] = (m_vPack[row] & 0xFFFF00FF) | ((uint32_t)iParam << 8); }
    inline unsigned char GetEventParam2(MIDIChannelEvent row) const { return IsThinRow(row) ? 0 : (unsigned char)((m_vPack[row] >> 16) & 0xFF); }
    inline void SetEventParam2(MIDIChannelEvent row, unsigned char iParam) { if ( !IsThinRow(row) ) m_vPack[row] = (m_vPack[row] & 0xFF00FFFF) | ((uint32_t)iParam << 16); }
    inline unsigned char GetEventChannel(MIDIChannelEvent row) const { return IsThinRow(row) ? GetEventChannel(GetThinOwner(row)) : (unsigned char)(m_vPack[row] & 0xF); }
    inline void SetEventChannel(MIDIChannelEvent row, unsigned char iChannel) { SetEventCode(row, (GetEventCode(row) & 0xF0) | iChannel); }
    inline ChannelEventType GetEventChannelEventType(MIDIChannelEvent row) const
    {
        if ( IsThinRow(row) )
        {
            size_t t = row - m_iFullRows;
            return (m_vThinOwners[t] & THIN_OWNER_NOTEON_FLAG) ? NoteOn : NoteOff;
        }
        return (ChannelEventType)((m_vPack[row] & 0xFF) >> 4);
    }
    inline void SetEventChannelEventType(MIDIChannelEvent row, ChannelEventType eType) { if ( !IsThinRow(row) ) SetEventCode(row, (GetEventCode(row) & 0xF) | ((unsigned char)eType << 4)); }
    inline bool GetEventPassDone(MIDIChannelEvent row) const { return IsThinRow(row) ? (m_vThinSisters[row - m_iFullRows] & THIN_SISTER_PASS_FLAG) != 0 : (m_vPack[row] & CHANNEL_EVENT_FLAG_PASS_DONE) != 0; }
    inline void SetEventPassDone(MIDIChannelEvent row, bool bDone)
    {
        if ( IsThinRow(row) )
        {
            size_t t = row - m_iFullRows;
            m_vThinSisters[t] = bDone ? (m_vThinSisters[t] | THIN_SISTER_PASS_FLAG) : (m_vThinSisters[t] & ~THIN_SISTER_PASS_FLAG);
        }
        else
            m_vPack[row] = bDone ? (m_vPack[row] | CHANNEL_EVENT_FLAG_PASS_DONE) : (m_vPack[row] & ~CHANNEL_EVENT_FLAG_PASS_DONE);
    }
    // Sets the thin row's owner reference (with the NTE type bit); used only
    inline void SetThinOwner(MIDIChannelEvent row, MIDIChannelEvent iOwner, bool bNoteOn)
    {
        m_vThinOwners[row - m_iFullRows] = (uint32_t)iOwner | (bNoteOn ? THIN_OWNER_NOTEON_FLAG : 0);
    }

    size_t ParseMIDI( const unsigned char *pcData, size_t iMaxSize );
    size_t ParseMIDICore( const unsigned char *pcData, size_t iMaxSize );
    size_t ParseTracks( const unsigned char *pcData, size_t iMaxSize );
    // phase-2 walk; each worker owns a disjoint row range, so no locking).
    void SetPoolRow( size_t iRow, uint32_t iTicks, uint32_t iLengths, uint32_t iSisters,
                     uint32_t iSimult, uint16_t iEventTrack, uint32_t iPack );
    // walk; each worker owns a disjoint thin range, so no locking). The full row id is iThin + m_iFullRows; the owner references a compacted full row.
    void SetThinRow( size_t iThin, uint32_t iTicks, uint32_t iOwners, uint32_t iSisters, size_t &iEscNext, size_t iEscEnd );
    size_t ParseEvents( const unsigned char *pcData, size_t iMaxSize );
    bool IsValid() const { return ( m_vTracks.size() > 0 && ( m_Info.iNoteCount > 0 || m_pPacked ) && m_Info.iDivision > 0 ); } // a packed song may have a note-free window

    void PostProcess(vector<MIDIChannelEvent>& vChannelEvents, eventvec_t* vProgramChanges = nullptr,
        vector<MIDIMetaEvent*>* vMetaEvents = nullptr, eventvec_t* vTempo = nullptr, eventvec_t* vSignature = nullptr, eventvec_t* vMarkers = nullptr);
    void PostProcessParallel(vector<MIDIChannelEvent>& vChannelEvents, eventvec_t* vProgramChanges = nullptr,
        vector<MIDIMetaEvent*>* vMetaEvents = nullptr, eventvec_t* vTempo = nullptr, eventvec_t* vSignature = nullptr, eventvec_t* vMarkers = nullptr);
    void ConnectNotes();
    void clear( void );
    void ReleaseOwnedData( void );

    // --- packed mode (prototype): the song lives in a compact packed store and only a window of it
    // is expanded into the normal row pool (see PackedMIDI.h). Enabled with PGFA_PACKED=1.
    bool IsPackedPending() const { return m_bPackedPending; }
    bool IsPacked() const { return (bool)m_pPacked; }
    void SetPackedSong( std::shared_ptr<PGFAPacked::PackedSong> p ) { m_pPacked = std::move( p ); }
    std::shared_ptr<PGFAPacked::PackedSong> GetPackedSong() const { return m_pPacked; }
    long long PackedSongEndUs() const { return m_llPackedSongEndUs; }
    bool LoadPackedWindow( long long llStartUs, uint64_t iMaxNotes, long long llMinSpanUs );
    void ReleasePacked();
    void ApplyPackedSongInfo(); // after PostProcess: first-note time and length are the whole song's, not the window's
    long long PackedWindowStartUs() const { return m_llPackedStartUs; }
    long long PackedWindowEndUs() const { return m_llPackedEndUs; }

    friend class MIDIPos;
    friend class ParallelMIDIPos;
    friend class MIDITrack;
    friend class MIDIEvent;

    struct MIDIInfo
    {
        MIDIInfo() { clear(); }
        void clear() { llTotalMicroSecs = llFirstNote = iFormatType = iNumTracks = iNumChannels = iDivision = iMinNote =
                       iMaxNote = iNoteCount = iEventCount = iMaxVolume = iVolumeSum = iTotalTicks = iTotalBeats = 0;
                       sFilename.clear(); }
        void AddTrackInfo( const MIDITrack &mTrack);

        wstring sFilename;
        string sMd5;
        uint32_t iFormatType;
        uint32_t iNumTracks, iNumChannels;
        uint32_t iDivision;
        int iMinNote, iMaxNote;
        size_t iNoteCount, iEventCount;
        int iMaxVolume, iVolumeSum;
        int iTotalTicks, iTotalBeats;
        long long llTotalMicroSecs, llFirstNote;
    };

    const MIDIInfo& GetInfo() const { return m_Info; }
    const vector< MIDITrack* >& GetTracks() const { return m_vTracks; }

private:
    static void InitArrays();
    static wstring aNoteNames[KEYS + 1];
    static Note aNoteVal[KEYS];
    static bool aIsSharp[KEYS];
    static int aWhiteCount[KEYS + 1];

    unsigned char *m_pcOwnedData = nullptr;
    bool m_bOwnedMapped = false; // m_pcOwnedData is a mapped file view (UnmapViewOfFile), not a new[] block
    std::shared_ptr<PGFAPacked::PackedSong> m_pPacked;
    std::wstring m_sPackedPath;
    bool m_bPackedPending = false;
    long long m_llPackedStartUs = 0, m_llPackedEndUs = 0, m_llPackedSongEndUs = 0;

    MIDIInfo m_Info;
    vector< MIDITrack* > m_vTracks;

    // are full 30-byte+2 rows; note-offs (and vel-0 NoteOns) that got paired in
    PoolVec<int64_t> m_vTimes;   // absolute microsecond time
    PoolVec<uint32_t> m_vTicks;   // absolute tick time
    PoolVec<uint32_t> m_vLengths; // note length in microseconds
    PoolVec<uint32_t> m_vSisters; // merged-list position of the sister; the actual row can be resolved through the game's merged list. UINT32_MAX = none
    PoolVec<uint16_t> m_vSimult;  // simultaneous notes (uint16: clamped; only ever read on note-on rows)
    PoolVec<uint16_t> m_vEventTrack; // owning track
    PoolVec<uint32_t> m_vPack;       // code | param1 << 8 | param2 << 16 | flags << 24
    PoolVec<uint32_t> m_vThinTicks;   // absolute ticks; wide mode only (empty otherwise)
    PoolVec<uint16_t> m_vThinDelta;   // narrow mode: tick - owner tick, THIN_DELTA_ESC = see m_vThinEsc
    struct ThinEsc { uint32_t thin; uint32_t tick; };
    PoolVec<ThinEsc> m_vThinEsc;      // sorted by thin index
    bool m_bThinWide = false;
    PoolVec<uint32_t> m_vThinOwners;  // full row id of the note-on + THIN_OWNER_NOTEON_FLAG
    PoolVec<uint32_t> m_vThinSisters; // merged-list pos of the sister + THIN_SISTER_PASS_FLAG during PostProcess
    uint32_t m_iFullRows = 0;        // full row count after the fold = thin zone base

    bool m_bComputedTimes = false;   // m_vTimes is empty; times come from m_aAnchors + ticks
    bool m_bTimeStandard = true;
    int m_iTimeTicksPerBeat = 0, m_iTimeTicksPerSecond = 0;
    unsigned __int64 m_uTimeMagic = 0; // see SetTimeDivisor
    unsigned m_iTimeShift = 0;
    int m_aAnchorKey[8] = { INT_MIN, INT_MAX, INT_MAX, INT_MAX, INT_MAX, INT_MAX, INT_MAX, INT_MAX };
    int m_aAnchorTick[8] = {};
    int64_t m_aAnchorTime[8] = {};
    int64_t m_aAnchorMul[8] = { 500000, 500000, 500000, 500000, 500000, 500000, 500000, 500000 };
    int m_iTimeDivisor = 0;
    unsigned __int64 m_uNumLimit = 0;
    size_t m_nAnchors = 0;
};

// Bump allocator for a track's meta/sysex events: no per-event malloc header (16 bytes each, which
// is a third of a tempo event) and far fewer allocator calls on files with millions of tempo events.
// Blocks grow geometrically so tracks with only a handful of events stay small.
class MetaArena
{
public:
    MetaArena() {}
    MetaArena( const MetaArena& ) = delete;
    MetaArena& operator=( const MetaArena& ) = delete;
    MetaArena( MetaArena&& o ) noexcept { Take( o ); }
    MetaArena& operator=( MetaArena&& o ) noexcept { if ( this != &o ) { Release(); Take( o ); } return *this; }
    ~MetaArena() { Release(); }

    void* Alloc( size_t n )
    {
        n = ( n + 7 ) & ~(size_t)7;
        if ( m_iLeft < n )
        {
            size_t iSize = m_iNextBlock;
            while ( iSize < n ) iSize *= 2;
            unsigned char *p = new unsigned char[iSize];
            m_vBlocks.push_back( p );
            m_pCur = p;
            m_iLeft = iSize;
            if ( m_iNextBlock < ( (size_t)256 << 10 ) ) m_iNextBlock *= 2;
        }
        void *p = m_pCur;
        m_pCur += n;
        m_iLeft -= n;
        return p;
    }
    // Gives back the most recent allocation (an event that failed to parse).
    void Unalloc( void *p, size_t n )
    {
        n = ( n + 7 ) & ~(size_t)7;
        if ( m_pCur - n == p ) { m_pCur -= n; m_iLeft += n; }
    }
    void Release()
    {
        for ( unsigned char *p : m_vBlocks ) delete[] p;
        m_vBlocks.clear();
        m_pCur = nullptr;
        m_iLeft = 0;
        m_iNextBlock = 512;
    }

private:
    void Take( MetaArena& o )
    {
        m_vBlocks = std::move( o.m_vBlocks );
        m_pCur = o.m_pCur; m_iLeft = o.m_iLeft; m_iNextBlock = o.m_iNextBlock;
        o.m_vBlocks.clear(); o.m_pCur = nullptr; o.m_iLeft = 0; o.m_iNextBlock = 512;
    }
    vector< unsigned char* > m_vBlocks;
    unsigned char *m_pCur = nullptr;
    size_t m_iLeft = 0;
    size_t m_iNextBlock = 512;
};

class MIDITrack
{
public:
    MIDITrack(MIDI& midi);
    ~MIDITrack( void );

    size_t ParseTrack( const unsigned char *pcData, size_t iMaxSize, size_t iTrack );
    size_t ParseEvents( const unsigned char *pcData, size_t iMaxSize, size_t iTrack );
    void clear( void );

    friend class MIDIPos;
    friend class ParallelMIDIPos;
    friend class MIDI;

    struct MIDITrackInfo
    {
        MIDITrackInfo() { clear(); }
        void clear() { llTotalMicroSecs = iSequenceNumber = iMinNote = iMaxNote = iNoteCount =
                       iEventCount = iMaxVolume = iVolumeSum = iTotalTicks = iNumChannels = iTempoCount = 0;
                       iEscCount = 0, memset( aNoteCount, 0, sizeof( aNoteCount ) ),
                       memset( aProgram, 0, sizeof( aProgram ) ),
                       sSequenceName.clear(); }
        void AddEventInfo( const MIDIEvent &mTrack );

        uint32_t iSequenceNumber;
        string sSequenceName;
        int iMinNote, iMaxNote;
        size_t iNoteCount, iEventCount;
        size_t iTempoCount = 0; // SetTempo meta events in this track
        size_t iEscCount = 0;   // notes longer than 0xFFFE ticks (upper bound; counting pass only)
        int iMaxVolume, iVolumeSum;
        int iTotalTicks;
        long long llTotalMicroSecs;
        size_t aNoteCount[16];
        int aProgram[16], iNumChannels;
    };
    const MIDITrackInfo& GetInfo() const { return m_TrackInfo; }

private:
    MIDITrackInfo m_TrackInfo;
    vector< MIDIEvent* > m_vMetas;     // meta/sysex objects (parse order); in m_Arena if m_bArenaMetas, else heap
    MetaArena m_Arena;
    bool m_bArenaMetas = false;
    MIDI& m_MIDI;
    int m_iCurrentEventPos;

    size_t m_iRowStart = 0;
    size_t m_iRowEnd = 0;
    size_t m_iThinStart = 0;
    size_t m_iThinEnd = 0;

public:
    size_t GetRowStart() const { return m_iRowStart; }
    size_t GetRowCount() const { return m_iRowEnd - m_iRowStart; }
    size_t GetThinStart() const { return m_iThinStart; }
    size_t GetThinEnd() const { return m_iThinEnd; }
    size_t GetThinCount() const { return m_iThinEnd - m_iThinStart; }
    size_t GetMetaCount() const { return m_vMetas.size(); }
};

class MIDIEvent
{
public:
    enum EventType { ChannelEvent, MetaEvent, SysExEvent, RunningStatus };
    static EventType DecodeEventType( int iEventCode );
    // Events have no virtual destructor (a vptr would add 8 bytes to every one of them), so they are
    // destroyed by their type tag. DestroyInPlace is for arena storage, DestroyHeap for `new`ed events.
    static void DestroyInPlace( MIDIEvent *pEvent );
    static void DestroyHeap( MIDIEvent *pEvent );

    // delta; *piPrevEventCode is the running-status source and is updated to
    static int MakeNextEvent( MIDI& midi, const unsigned char *pcData, size_t iMaxSize, int iTrack,
                              uint32_t *piAbsTicks, int *piPrevEventCode, MIDIChannelEvent *pPoolRow,
                              MIDIEvent **pOutEvent );

    EventType GetEventType() const { return (EventType)m_eEventType; }
    unsigned char GetEventCode() const { return m_iEventCode; }
    int GetTrack() const { return m_iTrack; }
    int GetAbsT() const { return m_iAbsT; }
    long long GetAbsMicroSec() const { return m_llAbsMicroSec; }
    void SetAbsMicroSec(long long llAbsMicroSec) { m_llAbsMicroSec = llAbsMicroSec; };

    long long m_llAbsMicroSec;
    int m_iAbsT;
    unsigned short m_iTrack;
    char m_eEventType;
    unsigned char m_iEventCode;
};

class MIDIMetaEvent : public MIDIEvent
{
public:
    MIDIMetaEvent() : m_eMetaEventType(SequenceNumber), m_iDataLen(0) { }
    ~MIDIMetaEvent() { if ( m_iDataLen > sizeof(m_aInline) ) delete[] m_pcData; }

    enum MetaEventType { SequenceNumber, TextEvent, Copyright, SequenceName, InstrumentName, Lyric, Marker,
                         CuePoint, ChannelPrefix = 0x20, PortPrefix = 0x21, EndOfTrack = 0x2F, SetTempo = 0x51,
                         SMPTEOffset = 0x54, TimeSignature = 0x58, KeySignature = 0x59, Proprietary = 0x7F };
    int ParseEvent( const unsigned char *pcData, size_t iMaxSize );

    MetaEventType GetMetaEventType() const { return m_eMetaEventType; }
    int GetDataLen() const { return m_iDataLen; }
    unsigned char *GetData() const {
        return m_iDataLen <= sizeof(m_aInline) ? (unsigned char*)m_aInline : m_pcData;
    }

private:
    MetaEventType m_eMetaEventType;
    uint32_t m_iDataLen;
    union {
        unsigned char* m_pcData;         // heap pointer, only used when escaped the inline
        unsigned char  m_aInline[8];    // tiny payloads (tempo, time sig, EoT) live here -- no heap alloc
    };
};

class MIDISysExEvent : public MIDIEvent
{
public:
    MIDISysExEvent() : m_pcData( 0 ) { }
    ~MIDISysExEvent() { if ( m_pcData ) delete[] m_pcData; }

    int ParseEvent( const unsigned char *pcData, size_t iMaxSize );

private:
    uint32_t m_iDataLen;
    unsigned char *m_pcData;
    bool m_bHasMoreData;
};


// Loads a synth DLL (OmniMIDI / SVMS / KDMAPI provider) and pins it for the rest of the process.
// Synths start audio threads and register WASAPI device-notification callbacks that can outlive
// TerminateKDMAPIStream; unloading the DLL leaves MMDevAPI calling into unmapped code the next
// time an audio endpoint changes (an access violation inside MMDevAPI.dll on a thread-pool
// thread). Never FreeLibrary a module returned from here.
HMODULE PGFALoadSynthModule(const wchar_t* nameOrPath);

// The KDMAPI provider used by MIDIOutDevice: SVMSAPI.dll beside the exe if usable, else OmniMIDI.
extern int g_iKDMAPISynth; // AudioSettings::iKDMAPISynth, copied in at startup before the first open
HMODULE PGFAGetKDMAPIModule();

// A KDMAPI stream is initialized once and never terminated (all users share it; a terminate/initialize cycle is slow and has hung the UI).
// Per synth DLL (OmniMIDI vs SVMS are separate streams).
bool KDMAPIStreamAlive(HMODULE module);
void KDMAPIStreamSetAlive(HMODULE module);


// Packed short messages that return every channel to a freshly reset synth: sound off, all notes off,
// controllers reset, sustain/sostenuto/soft off, bank 0, program 0, volume 100, pan 64, expression 127,
// reverb 40 / chorus 0, pitch bend centered, bend range 2 semitones. A song inherits none of the
// previous song's controller state this way (ResetKDMAPIStream / midiOutReset leave it behind).
void PGFAAppendFullSynthReset( std::vector< uint32_t > &out );
class MIDIOutDevice
{
public:
    MIDIOutDevice() : m_bIsOpen(false), m_bIsKDMAPI(false), m_hMIDIOut( NULL ) { }
    virtual ~MIDIOutDevice() { Close(); }

    int GetNumDevs() const;
    wstring GetDevName( int iDev ) const;
    bool Open( int iDev );
    bool OpenKDMAPI();
    void Close();
    void Reset();

    bool IsOpen() const { return m_bIsOpen; }
    bool IsKDMAPI() const { return m_bIsKDMAPI; }
    unsigned long long GetEventsSent() const { return m_ullEventsSent; }
    unsigned long long GetSendFailures() const { return m_ullSendFailures; }
    const wstring& GetDevice() const { return m_sDevice; };

    void AllNotesOff();
    void FullReset(); // see PGFAAppendFullSynthReset
    void AllNotesOff( const vector< int > &vChannels );
    void SetVolume( double dVolume );

    bool PlayEventAcrossChannels( unsigned char cStatus, unsigned char cParam1, unsigned char cParam2 );
    bool PlayEventAcrossChannels( unsigned char cStatus, unsigned char cParam1, unsigned char cParam2, const vector< int > &vChannels );
    bool PlayEvent( unsigned char bStatus, unsigned char bParam1, unsigned char bParam2 = 0 );

private:
    static FARPROC GetOmniMIDIProc(const char* func);
    static void CALLBACK MIDIOutProc( HMIDIOUT hmo, UINT wMsg, DWORD_PTR dwInstance,
                                      DWORD_PTR dwParam1, DWORD_PTR dwParam2 );

    bool m_bIsOpen;
    bool m_bIsKDMAPI;
    void(WINAPI* SendDirectData)(DWORD);
    wstring m_sDevice;
    HMIDIOUT m_hMIDIOut;
    unsigned long long m_ullEventsSent = 0;
    unsigned long long m_ullSendFailures = 0;
};

class MIDILoadingProgress {
public:
    enum Stage { CopyToMem, Decompress, ParseTracks, ConnectNotes, SortEvents, Finalize, Done };
    static constexpr unsigned MaxSortWorkers = 64;

    Stage stage;
    std::wstring name;
    std::atomic<uint64_t> progress;
    uint64_t max;
    std::atomic<unsigned> sortWorkerCount{ 0 };
    std::atomic<uint64_t> sortProgress[MaxSortWorkers]{};
    std::atomic<uint64_t> sortMax[MaxSortWorkers]{};
};

extern MIDILoadingProgress g_LoadingProgress;