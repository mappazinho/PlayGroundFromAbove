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
    inline bool IsThinRow(MIDIChannelEvent row) const
    {
        return row >= m_iFullRows && row < (MIDIChannelEvent)(m_iFullRows + m_vThinTicks.size());
    }
    inline MIDIChannelEvent GetThinOwner(MIDIChannelEvent row) const
    {
        return (MIDIChannelEvent)(m_vThinOwners[row - m_iFullRows] & ~THIN_OWNER_NOTEON_FLAG);
    }
    inline int64_t GetEventTime(MIDIChannelEvent row) const
    {
        if ( IsThinRow(row) )
        {
            size_t t = row - m_iFullRows;
            return m_vTimes[m_vThinOwners[t] & ~THIN_OWNER_NOTEON_FLAG] + m_vThinLengths[t];
        }
        return m_vTimes[row];
    }
    inline void SetEventTime(MIDIChannelEvent row, int64_t iTime) { if ( !IsThinRow(row) ) m_vTimes[row] = iTime; }
    inline int GetEventAbsT(MIDIChannelEvent row) const { return IsThinRow(row) ? (int)m_vThinTicks[row - m_iFullRows] : (int)m_vTicks[row]; }
    inline uint32_t GetEventTicks(MIDIChannelEvent row) const { return IsThinRow(row) ? m_vThinTicks[row - m_iFullRows] : m_vTicks[row]; }
    inline void SetEventTicks(MIDIChannelEvent row, uint32_t iTicks) { if ( !IsThinRow(row) ) m_vTicks[row] = iTicks; }
    inline uint32_t GetEventLength(MIDIChannelEvent row) const { return IsThinRow(row) ? m_vThinLengths[row - m_iFullRows] : m_vLengths[row]; }
    inline void SetEventLength(MIDIChannelEvent row, uint32_t iLength) { if ( IsThinRow(row) ) m_vThinLengths[row - m_iFullRows] = iLength; else m_vLengths[row] = iLength; }
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
    void SetThinRow( size_t iThin, uint32_t iTicks, uint32_t iOwners, uint32_t iSisters,
                     uint32_t iLengths );
    size_t ParseEvents( const unsigned char *pcData, size_t iMaxSize );
    bool IsValid() const { return ( m_vTracks.size() > 0 && m_Info.iNoteCount > 0 && m_Info.iDivision > 0 ); }

    void PostProcess(vector<MIDIChannelEvent>& vChannelEvents, eventvec_t* vProgramChanges = nullptr,
        vector<MIDIMetaEvent*>* vMetaEvents = nullptr, eventvec_t* vTempo = nullptr, eventvec_t* vSignature = nullptr, eventvec_t* vMarkers = nullptr);
    void PostProcessParallel(vector<MIDIChannelEvent>& vChannelEvents, eventvec_t* vProgramChanges = nullptr,
        vector<MIDIMetaEvent*>* vMetaEvents = nullptr, eventvec_t* vTempo = nullptr, eventvec_t* vSignature = nullptr, eventvec_t* vMarkers = nullptr);
    void ConnectNotes();
    void clear( void );
    void ReleaseOwnedData( void );

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

    MIDIInfo m_Info;
    vector< MIDITrack* > m_vTracks;

    // are full 30-byte+2 rows; note-offs (and vel-0 NoteOns) that got paired in
    vector<int64_t>  m_vTimes;   // absolute microsecond time
    vector<uint32_t> m_vTicks;   // absolute tick time
    vector<uint32_t> m_vLengths; // note length in microseconds
    vector<uint32_t> m_vSisters; // merged-list position of the sister; the actual row can be resolved through the game's merged list. UINT32_MAX = none
    vector<uint16_t> m_vSimult;  // simultaneous notes (uint16: clamped; only ever read on note-on rows)
    vector<uint16_t> m_vEventTrack; // owning track
    vector<uint32_t> m_vPack;       // code | param1 << 8 | param2 << 16 | flags << 24
    vector<uint32_t> m_vThinTicks;   // absolute ticks (needed by the merge walk and GetEventAbsT)
    vector<uint32_t> m_vThinOwners;  // full row id of the note-on + THIN_OWNER_NOTEON_FLAG
    vector<uint32_t> m_vThinSisters; // merged-list pos of the sister + THIN_SISTER_PASS_FLAG during PostProcess
    vector<uint32_t> m_vThinLengths; // note length in microseconds
    uint32_t m_iFullRows = 0;        // full row count after the fold = thin zone base
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
                       iEventCount = iMaxVolume = iVolumeSum = iTotalTicks = iNumChannels = 0;
                       memset( aNoteCount, 0, sizeof( aNoteCount ) ),
                       memset( aProgram, 0, sizeof( aProgram ) ),
                       sSequenceName.clear(); }
        void AddEventInfo( const MIDIEvent &mTrack );

        uint32_t iSequenceNumber;
        string sSequenceName;
        int iMinNote, iMaxNote;
        size_t iNoteCount, iEventCount;
        int iMaxVolume, iVolumeSum;
        int iTotalTicks;
        long long llTotalMicroSecs;
        size_t aNoteCount[16];
        int aProgram[16], iNumChannels;
    };
    const MIDITrackInfo& GetInfo() const { return m_TrackInfo; }

private:
    MIDITrackInfo m_TrackInfo;
    vector< MIDIEvent* > m_vMetas;     // meta/sysex heap objects (parse order)
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