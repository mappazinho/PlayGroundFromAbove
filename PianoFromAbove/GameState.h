/*************************************************************************************************
*
* File: GameState.h
*
* Description: Defines the game states and objects rendered into the graphics window
*
* Copyright (c) 2010 Brian Pantano. All rights reserved.
*
*************************************************************************************************/
#pragma once

#include <Windows.h>
#include <map>
#include <string>
#include <functional>
#include <deque>
#include <vector>
#include <tuple>
#include <unordered_map>
#include <cstdint>
#include <thread>
#include <atomic>
#include <memory>
using namespace std;

//#include "ProtoBuf\MetaData.pb.h"
#include "Renderer.h"
#include "MIDI.h"
#include "Misc.h"

// Abstract base class
class GameState
{
public:
    enum GameError { Success = 0, BadPointer, OutOfMemory, DirectXError };
    enum State { Intro = 0, Splash, Practice };

    // Static methods
    static const wstring Errors[];
    static GameError ChangeState( GameState *pNextState, GameState **pDestObj );

    // Constructors
    GameState( HWND hWnd, Renderer *pRenderer ) : m_hWnd( hWnd ), m_pRenderer( pRenderer ), m_pNextState( NULL ) {}
    virtual ~GameState( void ) {}

    virtual const char* DebugName() const = 0;
    virtual bool IsFreePlay() const { return false; }

    virtual GameError Init() = 0;

    virtual GameError MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) = 0;

    virtual GameError Logic() = 0;

    virtual GameError Render() = 0;

    virtual void Discard() { m_bDiscarded = true; }
    bool IsDiscarded() const { return m_bDiscarded; }

    GameState *NextState() { return m_pNextState; };

    void SetHWnd( HWND hWnd ) { m_hWnd = hWnd; }
    void SetRenderer( Renderer *pRenderer ) { m_pRenderer = pRenderer; }

protected:
    HWND m_hWnd;

    Renderer *m_pRenderer;

    GameState *m_pNextState;

    bool m_bDiscarded = false;

    static const int QueueSize = 50;
};

struct ChannelSettings
{
    ChannelSettings() { bHidden = bMuted = false; SetColor( 0x00000000 ); }
    void SetColor();
    void SetColor( unsigned int iColor, double dDark = 0.5, double dVeryDark = 0.2 );

    bool bHidden, bMuted;
    unsigned int iPrimaryRGB, iDarkRGB, iVeryDarkRGB, iOrigBGR;
};
struct TrackSettings { ChannelSettings aChannels[16]; };

class SplashScreen : public GameState
{
public:
    SplashScreen( HWND hWnd, Renderer *pRenderer );

    const char* DebugName() const override { return "SplashScreen"; }

    GameError MsgProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam ) override;
    GameError MsgProcLegacy( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );
    GameError Init() override;
    GameError Logic() override;
    GameError LogicLegacy();
    GameError Render() override;

private:
    void InitNotes( const vector< MIDIEvent* > &vEvents );
    void InitState();
    void ColorChannel( int iTrack, int iChannel, unsigned int iColor, bool bRandom = false );
    void SetChannelSettings( const vector< bool > &vMuted, const vector< bool > &vHidden, const vector< unsigned > &vColor );

    void UpdateState( int iPos );

    void RenderGlobals();
    void RenderNotes();
    void RenderNote(MIDIChannelEvent pNote);
    float GetNoteX( int iNote );
    void GenNoteXTable();

    MIDI m_MIDI; // The song to display
    vector< MIDIChannelEvent > m_vEvents; // The channel events of the song
    int m_iStartPos;
    int m_iEndPos;
    long long m_llStartTime;
    vector<int> m_vState[128];  // The notes that are on at time m_llStartTime.
    Timer m_Timer; // Frame timers
    double m_dVolume;
    bool m_bPaused;
    bool m_bMute;
    bool m_bAudioStarted = false;

    MIDIOutDevice m_OutDevice;

    static const float SharpRatio;
    static const long long TimeSpan = 3000000;
    vector< TrackSettings > m_vTrackSettings;

    int m_iStartNote, m_iEndNote; // Start and end notes of the songs
    float m_fNotesX, m_fNotesY, m_fNotesCX, m_fNotesCY; // Notes position
    int m_iAllWhiteKeys; // Number of white keys are on the screen
    float m_fWhiteCX; // Width of the white keys
    long long m_llRndStartTime; // Rounded start time to make stuff drop at the same time

    float notex_table[128];
};

class IntroScreen : public GameState
{
public:
    IntroScreen( HWND hWnd, Renderer *pRenderer ) : GameState( hWnd, pRenderer ) {}

    const char* DebugName() const override { return "IntroScreen"; }

    GameError MsgProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam ) override;
    GameError MsgProcLegacy( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );
    GameError Init() override;
    GameError Logic() override;
    GameError LogicLegacy();
    GameError Render() override;
};

typedef struct {
    unsigned idx;
    unsigned sister_idx;
} thread_work_t;

// MainScreen's legacy Logic() still performs visual event/state bookkeeping. Once the
// independent playback thread takes ownership, these hidden methods make every legacy output
class MainPlaybackMIDIOutDevice : public MIDIOutDevice
{
public:
    void SetThreadOwned(bool owned, bool kdmapi)
    {
        if (owned && !m_bThreadOwned)
            MIDIOutDevice::Close();
        m_bThreadOwned = owned;
        m_bThreadKDMAPI = kdmapi;
    }

    bool Open(int iDev)
    {
        return m_bThreadOwned ? true : MIDIOutDevice::Open(iDev);
    }
    bool OpenKDMAPI()
    {
        return m_bThreadOwned ? true : MIDIOutDevice::OpenKDMAPI();
    }
    void Close()
    {
        if (!m_bThreadOwned)
            MIDIOutDevice::Close();
    }
    void Reset()
    {
        if (!m_bThreadOwned)
            MIDIOutDevice::Reset();
    }
    bool IsOpen() const
    {
        return m_bThreadOwned ? true : MIDIOutDevice::IsOpen();
    }
    bool IsKDMAPI() const
    {
        return m_bThreadOwned ? m_bThreadKDMAPI : MIDIOutDevice::IsKDMAPI();
    }
    void SetVolume(double dVolume)
    {
        if (!m_bThreadOwned)
            MIDIOutDevice::SetVolume(dVolume);
    }
    void AllNotesOff()
    {
        if (!m_bThreadOwned)
            MIDIOutDevice::AllNotesOff();
    }
    void AllNotesOff(const vector<int>& channels)
    {
        if (!m_bThreadOwned)
            MIDIOutDevice::AllNotesOff(channels);
    }
    bool PlayEventAcrossChannels(unsigned char status, unsigned char p1, unsigned char p2)
    {
        return m_bThreadOwned ? true : MIDIOutDevice::PlayEventAcrossChannels(status, p1, p2);
    }
    bool PlayEventAcrossChannels(unsigned char status, unsigned char p1, unsigned char p2, const vector<int>& channels)
    {
        return m_bThreadOwned ? true : MIDIOutDevice::PlayEventAcrossChannels(status, p1, p2, channels);
    }
    bool PlayEvent(unsigned char status, unsigned char p1, unsigned char p2 = 0)
    {
        return m_bThreadOwned ? true : MIDIOutDevice::PlayEvent(status, p1, p2);
    }

private:
    bool m_bThreadOwned = false;
    bool m_bThreadKDMAPI = false;
};

class MainScreen : public GameState
{
public:
    static const float KBPercent;

    MainScreen( wstring sMIDIFile, State eGameMode, HWND hWnd, Renderer *pRenderer );
    ~MainScreen() override;

    const char* DebugName() const override { return "MainScreen"; }
    virtual bool IsFreePlay() const { return false; }

    GameError MsgProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam ) override;
    GameError MsgProcLegacy( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );
    GameError Init() override;
    GameError Logic( void ) override;
    GameError LogicLegacy( void );
    GameError Render( void ) override;
    void Discard() override;
    void DiscardLegacy();

    bool IsValid() const { return m_MIDI.IsValid(); }
    const MIDI& GetMIDI() const { return m_MIDI; }
    float GetStatsBounceScaleForOverlay() const { return GetStatsBounceScale(); }
    double GetAudioSchedulerFPSForOverlay() const { return m_dPlaybackAudioSchedulerHz.load(std::memory_order_relaxed); }

    void ToggleMuted( int iTrack, int iChannel ) { m_vTrackSettings[iTrack].aChannels[iChannel].bMuted =
                                                  !m_vTrackSettings[iTrack].aChannels[iChannel].bMuted; }
    void ToggleHidden( int iTrack, int iChannel ) { m_vTrackSettings[iTrack].aChannels[iChannel].bHidden =
                                                   !m_vTrackSettings[iTrack].aChannels[iChannel].bHidden; }
    void MuteChannel( int iTrack, int iChannel, bool bMuted ) { m_vTrackSettings[iTrack].aChannels[iChannel].bMuted = bMuted; }
    void HideChannel( int iTrack, int iChannel, bool bHidden ) { m_vTrackSettings[iTrack].aChannels[iChannel].bHidden = bHidden; }
    void ColorChannel( int iTrack, int iChannel, unsigned int iColor, bool bRandom = false );
    ChannelSettings* GetChannelSettings( int iChannel );
    void SetChannelSettings( const vector< bool > &vMuted, const vector< bool > &vHidden, const vector< unsigned > &vColor );

    bool m_bUseCustomAudio = false;
    wstring m_sCustomAudioPath;

    // Video render: capture playback + prerendered audio into an mp4 via FFmpeg.
    void StartVideoRender();
    void BeginVideoRender();
    void FinishVideoRender();
    void RestoreMainWindowAfterRender();

protected:
    void InitColors();
    void InitState();

    void UpdateState(int key, const thread_work_t& work);
    void JumpTo(long long llStartTime, bool bUpdateGUI = true);

    void StartPlaybackAudioThread();
    void StopPlaybackAudioThread();
    void SyncPlaybackAudioThread(bool forceSeek = false);
    void PlaybackAudioThreadMain();
    void PlaybackAudioSeek(long long songUs);
    long long PlaybackAudioClockNow() const;

    void PlaySkippedEvents(eventvec_t::const_iterator itOldProgramChange);
    void ApplyMarker(unsigned char* data, size_t size);
    void AdvanceIterators( long long llTime, bool bIsJump );
    MIDIMetaEvent* GetPrevious( eventvec_t::const_iterator &itCurrent,
                                const eventvec_t &vEventMap, int iDataLen );

    int GetCurrentTick( long long llStartTime );
    int GetCurrentTick( long long llStartTime, int iLastTempoTick, long long llLastTempoTime, int iMicroSecsPerBeat );
    long long GetTickTime( int iTick );
    long long GetTickTime( int iTick, int iLastTempoTick, long long llLastTempoTime, int iMicroSecsPerBeat );
    int GetBeat( int iTick, int iBeatType, int iLastTempoTick );
    int GetBeatTick( int iTick, int iBeatType, int iLastTempoTick );
    long long GetMinTime() const { return m_MIDI.GetInfo().llFirstNote - 3000000; }
    long long GetMaxTime() const { return m_MIDI.GetInfo().llTotalMicroSecs + 500000; }
    float GetCorruptorAmount() const;

    // Rendering
    void RenderGlobals();
    void RenderLines();
    void RenderNotes();
    void RenderNote(MIDIChannelEvent pNote);
    void RenderPianoRollStripNote(MIDIChannelEvent pNote);
    virtual NoteData BuildRenderNoteData(MIDIChannelEvent pNote) const;
    void RenderNotesImageBuffer();
    void RenderPianoRollStripImageBuffer();
    NoteData BuildChunkNoteData(MIDIChannelEvent pNote, long long chunkStart) const;
    void GenNoteXTable();
    float GetNoteX( int iNote );
    void RenderKeys();
    void RenderBorder();
void RenderText();
    void RenderStatusLine(int line, float width, float yOffset, float rightEdge, const char* left, const char* format, ...);
    void RenderStatus(int lines);
    void RenderSysStats();
    float GetStatsBounceScale() const;
    void RenderMarker(const char* str);
    void RenderMessage( LPRECT prcMsg, TCHAR *sMsg );

    // MIDI info
    MIDI m_MIDI; // The song to display
    vector< MIDIChannelEvent > m_vEvents; // The rows of the song's channel events
    vector< MIDIMetaEvent* > m_vMetaEvents; // The meta events of the song
    eventvec_t m_vNoteOns; // Map: note->time->Event pos. Used for fast(er) random access to the song.
    eventvec_t m_vNonNotes; // Tracked for jumping
    eventvec_t m_vProgramChange; // Tracked so we don't jump over them during random access
    eventvec_t m_vTempo; // Tracked for drawing measure lines
    eventvec_t m_vSignature; // Measure lines again
    eventvec_t m_vMarkers; // Tracked for section names in some longer MIDIs
    eventvec_t::const_iterator m_itNextProgramChange;
    eventvec_t::const_iterator m_itNextTempo;
    eventvec_t::const_iterator m_itNextSignature;
    eventvec_t::const_iterator m_itNextMarker;
    uint32_t m_iMicroSecsPerBeat, m_iLastTempoTick; // Tempo
    long long m_llLastTempoTime; // Tempo
    int m_CurBeat, m_iBeatsPerMeasure, m_iBeatType, m_iClocksPerMet, m_iLastSignatureTick; // Time signature
    std::string m_sMarker; // Current marker to display on the screen
    unsigned char* m_pMarkerData = nullptr; // Used for refreshing marker data when changing encoding on the fly
    size_t m_iMarkerSize = 0;
    int m_iCurEncoding;

    // Playback
    State m_eGameMode;
    long long m_iStartPos, m_iEndPos; // Postions of the start and end events that occur in the current window
    long long m_llStartTime, m_llTimeSpan;  // Times of the start and end events of the current window
    long long m_llDisplayTime; // Wall-clock song position for the status bar:
    int m_iStartTick; // Tick that corresponds with m_llStartTime. Used to help with beat and metronome detection
    vector<int> m_vState[128];  // The notes that are on at time m_llStartTime.
    vector<thread_work_t> m_vThreadWork[128];
    int m_pNoteState[128]; // The last note that was turned on
    double m_dSpeed; // Speed multiplier
    bool m_bPaused; // Paused state
    Timer m_Timer; // Frame timers
    Timer m_RealTimer;
    bool m_bMute;
    bool m_bAnyChannelMuted;
    double m_dVolume;
    bool m_bTickMode = false;
    bool m_bAudioStarted = false; // Pre-rendered audio: started once per screen

    // Independent live-MIDI clock/scheduler. The renderer only snapshots control changes; this
    // thread owns the output provider and advances from QPC even when Present/Render is blocked
    std::thread m_PlaybackAudioThread;
    std::atomic<bool> m_bPlaybackAudioExit{ false };
    std::atomic<bool> m_bPlaybackAudioRunning{ false };
    std::atomic<bool> m_bPlaybackAudioLiveEnabled{ false };
    std::atomic<bool> m_bPlaybackAudioPaused{ true };
    std::atomic<bool> m_bPlaybackAudioMute{ false };
    std::atomic<bool> m_bPlaybackAudioPianoOverride{ false };
    std::atomic<bool> m_bPlaybackAudioWantKDMAPI{ false };
    std::atomic<int> m_iPlaybackAudioDevice{ -1 };
    std::atomic<double> m_dPlaybackAudioSpeed{ 1.0 };
    std::atomic<double> m_dPlaybackAudioVelocityScale{ 1.0 };
    std::atomic<float> m_fPlaybackAudioCorruption{ 0.0f };
    std::atomic<long long> m_llPlaybackAudioControlSongUs{ 0 };
    std::atomic<long long> m_llPlaybackAudioClockUs{ 0 };
    std::atomic<unsigned long long> m_uPlaybackAudioControlSerial{ 0 };
    std::atomic<unsigned long long> m_uPlaybackAudioSeekSerial{ 0 };
    std::atomic<double> m_dPlaybackAudioSchedulerHz{ 0.0 };
    std::shared_ptr<const std::vector<uint16_t>> m_pPlaybackAudioMuteMask;
    std::vector<uint16_t> m_vPlaybackAudioLastMuteMask;
    bool m_bPlaybackAudioControlInitialized = false;
    bool m_bPlaybackAudioLastPaused = true;
    bool m_bPlaybackAudioLastLive = false;
    bool m_bPlaybackAudioLastKDMAPI = false;
    int m_iPlaybackAudioLastDevice = -2;
    double m_dPlaybackAudioLastSpeed = 1.0;

    // FPS variables
    bool m_bShowFPS;
    int m_iFPSCount;
    long long m_llFPSTime;
    double m_dFPS;

    long long m_llFrameMaxLate = 0;
    unsigned long long m_ullFrameLateCount = 0;
    long long m_llMaxLateMicros = 0;
    unsigned long long m_ullLateEvents = 0;

    MainPlaybackMIDIOutDevice m_OutDevice;

    static const float SharpRatio;
    static const float KeyRatio;
    bool m_bShowKB;
    int m_eKeysShown;
    int m_eTransitionSpeed;
    float m_fKeysTransition = 0.0f;
    ChannelSettings m_csBackground;
    ChannelSettings m_csKBRed, m_csKBWhite, m_csKBSharp, m_csKBBackground;
    vector< TrackSettings > m_vTrackSettings;
    float m_pBends[16] = {};
    deque<tuple<long long, long long>> m_dNPSNotes;
    deque<long long> m_dNPSHistory;
    long long m_llMaxNPS = 1;
    long long m_llMaxNoteLen = 0;       // longest note, microseconds
    long long m_llMaxNoteLenTicks = 0;  // longest note, ticks (tick mode)
    vector<long long> m_vImageBufferMaxEndTime;
    vector<long long> m_vImageBufferMaxEndTick;
    vector<long long> m_vImageBufferPrefixEndTime;
    vector<long long> m_vImageBufferPrefixEndTick;
    bool m_bImageBufferNeedsInvalidate = true; // defer until renderer is attached
    std::wstring m_sCurBackground;
    bool m_bBackgroundLoaded;
    float m_fLastBGBlur = -1.0f;

    float m_fZoomX, m_fOffsetX, m_fOffsetY;
    float m_fTempZoomX, m_fTempOffsetX, m_fTempOffsetY;
    bool m_bZoomMove, m_bTrackPos, m_bTrackZoom;
    POINT m_ptStartZoom, m_ptLastPos;

    float notex_table[128];

    int m_iStartNote, m_iEndNote; // Start and end notes of the songs
    float m_fNotesX, m_fNotesY, m_fNotesCX, m_fNotesCY; // Notes position
    int m_iAllWhiteKeys; // Number of white keys are on the screen
    float m_fWhiteCX; // Width of the white keys
    float m_fViewStartX = 0.0f; // Start position in white-key units
    static float GetNoteCoord(int iNote); // Coordinate of note iNote in white-key units
    long long m_llRndStartTime; // Rounded start time to make stuff drop at the same time
    uint64_t m_aSkipRender[4];
    
    bool m_bDumpFrames = false;
    std::vector<unsigned char> m_vImageData;
    HANDLE m_hVideoPipe;

    // Video render state (FFmpeg capture)
    bool m_bRenderVideo = false;
    HANDLE m_hFFPipeWrite = NULL;
    HANDLE m_hFFProc = NULL;
    std::wstring m_sFFVideoRaw, m_sFFVideoOut, m_sFFWav;
    bool m_bRenderPending = false;
    bool m_bRenderMainRectSaved = false;
    RECT m_rcRenderMainSaved = {};

    // Debug assertion fail workaround
    bool m_bNextMarkerInited = false;
};

class FreePlayScreen : public MainScreen
{
public:
    FreePlayScreen(HWND hWnd, Renderer* pRenderer);

    GameError Init() override;
    GameError MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) override;
    GameError MsgProcLegacy(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    GameError Logic() override;
    GameError LogicLegacy();
    GameError Render() override { return Success; }
    bool IsFreePlay() const override { return true; }
    const char* DebugName() const override { return "FreePlayScreen"; }

private:
    int NoteFromMousePos(int mx, int my) const;
    void NoteOn(int note, int velocity, long long llStamp = -1);
    void NoteOnSingle(int note, int velocity, long long llStamp = -1);
    void NoteOff(int note, bool bStretch = false, long long llStamp = -1);
    void NoteOffSingle(int note, bool bStretch = false, long long llStamp = -1, int iSpecificIdx = -1);
    void SlideTo(int note);   // legato: move the chord, only strike/release the edge keys
    void ChordRelease(bool bStretch = false, long long llStamp = -1); // release this chord's own notes
    int m_iChordEvent[128] = {}; // per key: event this key was struck with by the current chord, or -1

    NoteData BuildRenderNoteData(MIDIChannelEvent pNote) const override;
    void RenderNoteIdx(int idx);

    struct ReleasedNote {
        long long releaseTime;
        long long finalLength;
    };
    std::unordered_map<int, ReleasedNote> m_mReleasedNotes; // eventIdx -> release info
    std::deque<int> m_dReleaseOrder; // eventIdx of released notes, oldest first, for O(1) slot recycling

    bool m_bShowColorPicker = true;
    float m_fFreePlayColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f }; // RGBA
    float m_fFreePlaySpeed = 1.0f; // 1.0 = real time, 2.0 = twice as fast
    float m_fRepeaterNPS = 0.0f;   // note repeater: repeats per second while held (0 = off)
    long long m_llRepeaterLast = 0; // last repeater fire time (us)
    int m_iFreePlayRange = 1;    // keys struck at once per key press (1..128)
    bool m_bSlamHeld = false;
    bool m_bRainbow = false;     // rainbow colors per key press
    int m_iRainbowOffset = 0;    // advances by one hue step per press
    bool m_bMirrorKeys = false;  // duplicate each press at the opposite side of the piano
    std::vector<int> m_vFreeSlots; // free color-slot indices for new notes
    struct LoopEvent {
        bool isOn;
        unsigned char note;
        unsigned char velocity;
        unsigned int color;  // 0x00BBGGRR, captured when the note was recorded
        long long time;      // usec from loop start
    };
    struct Loop {
        char name[64] = {};
        long long duration = 0;          // usec
        std::vector<LoopEvent> events;   // sorted by time
        bool playing = true;
        long long playhead = 0;          // usec into the current iteration
        long long lastTick = 0;
        int nextEvent = 0;
        int held[128];          // event index this loop currently holds per note, or -1
        float velocity = 1.0f;           // playback velocity multiplier (0..2)
        bool bColorOverride = false;      // when set, recolor the whole loop
        unsigned int uColorOverride = 0;  // 0x00BBGGRR
        float color[3] = { 1.0f, 1.0f, 1.0f }; // UI mirror of the override color
    };
    std::vector<Loop> m_vLoops;
    bool m_bRecording = false;
    bool m_bCountdown = false;
    bool m_bPlayback = false; // true while loop playback drives note events (not captured)
    bool m_bPlaybackColorPinned = false; // loop playback pins the note color instead of picker/rainbow
    unsigned int m_uPlaybackColor = 0;   // color to use while pinned
    long long m_llCountdownStart = 0;
    long long m_llRecordStart = 0;
    long long m_llRecordDuration = 4000000LL; // default 4 seconds
    float m_fRecordSeconds = 4.0f;            // UI mirror
    int m_iLoopCounter = 0;
    std::vector<LoopEvent> m_vRecordingEvents;
    void StartLoopRecording();
    void StopLoopRecording();
    void TickLooper();
    void DeleteLoop(int i);
    int m_iFreePlayNoteCount = 0;  // notes played this frame, for the NPS stat

    bool m_bMouseDown = false;
    int m_iLastClickedNote = -1;
    long long m_llFreePlayTime = 0;
    long long m_llFreePlayLastFrame = 0;
};
