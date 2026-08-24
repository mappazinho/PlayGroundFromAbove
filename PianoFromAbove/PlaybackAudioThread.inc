// Independent live-MIDI scheduler for MainScreen.
//
// This file is included by GameState.cpp after GameStateLegacy.inc. Keeping it
// in the same translation unit lets the scheduler reuse the existing corruption
// helpers without duplicating their behavior, while the immutable MIDI/event
// arrays remain safe to read until StopPlaybackAudioThread() joins the worker.

#include <mmsystem.h>
#include <memory>
#include <vector>
#include <cmath>
#include <algorithm>

namespace {

using PGFASvmsResult = uint32_t;
using PGFASvmsSession = uint64_t;
static constexpr PGFASvmsResult PGFA_SVMS_OK = 0u;
static constexpr uint32_t PGFA_SVMS_ABI_V1 = 1u;
static constexpr uint32_t PGFA_SVMS_STRUCT_V1 = 1u;
static constexpr uint64_t PGFA_SVMS_CAP_EXACT_QPC = UINT64_C(1) << 0;
static constexpr uint64_t PGFA_SVMS_CAP_SHORT_BATCH = UINT64_C(1) << 1;
static constexpr uint64_t PGFA_SVMS_CAP_QUEUE_CONTROL = UINT64_C(1) << 7;
static constexpr uint32_t PGFA_SVMS_INGRESS_LOSSLESS = 1u;

struct PGFASvmsSessionConfig {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t reserved[6];
};
static_assert(sizeof(PGFASvmsSessionConfig) == 64, "SVMS session ABI mismatch");

struct PGFASvmsShortEvent {
    uint64_t timestamp_qpc;
    uint32_t packed_message;
    uint32_t reserved;
};
static_assert(sizeof(PGFASvmsShortEvent) == 16, "SVMS short-event ABI mismatch");

using PGFASvmsCreateSessionFn = PGFASvmsResult (__cdecl *)(const PGFASvmsSessionConfig*, PGFASvmsSession*);
using PGFASvmsDestroySessionFn = PGFASvmsResult (__cdecl *)(PGFASvmsSession);
using PGFASvmsSendShortFn = PGFASvmsResult (__cdecl *)(PGFASvmsSession, uint32_t);
using PGFASvmsSendShortAtQpcFn = PGFASvmsResult (__cdecl *)(PGFASvmsSession, uint32_t, uint64_t);
using PGFASvmsSendShortBatchFn = PGFASvmsResult (__cdecl *)(PGFASvmsSession, const PGFASvmsShortEvent*, uint32_t);
using PGFASvmsSendSystemExclusiveFn = PGFASvmsResult (__cdecl *)(PGFASvmsSession, const uint8_t*, uint32_t);
using PGFASvmsResetFn = PGFASvmsResult (__cdecl *)(PGFASvmsSession);
using PGFASvmsGetTelemetryFn = PGFASvmsResult (__cdecl *)(PGFASvmsSession, void*);
using PGFASvmsGetRuntimeClockFn = PGFASvmsResult (__cdecl *)(uint64_t*, uint64_t*);
using PGFASvmsSendTimedShortBatchFn = PGFASvmsResult (__cdecl *)(PGFASvmsSession, const void*, uint32_t);
using PGFASvmsGetOutputClockFn = PGFASvmsResult (__cdecl *)(PGFASvmsSession, uint64_t*, uint32_t*);
using PGFASvmsGetMonotonicClockFn = PGFASvmsResult (__cdecl *)(uint64_t*);
using PGFASvmsSetIngressModeFn = PGFASvmsResult (__cdecl *)(PGFASvmsSession, uint32_t);

struct PGFASvmsInterfacePrefix {
    uint32_t struct_size;
    uint32_t struct_version;
    uint32_t abi_version;
    uint32_t reserved0;
    uint64_t capabilities;
    uint32_t product_major;
    uint32_t product_minor;
    uint32_t product_patch;
    uint32_t build_number;
    PGFASvmsCreateSessionFn create_session;
    PGFASvmsDestroySessionFn destroy_session;
    PGFASvmsSendShortFn send_short;
    PGFASvmsSendShortAtQpcFn send_short_at_qpc;
    PGFASvmsSendShortBatchFn send_short_batch;
    PGFASvmsSendSystemExclusiveFn send_system_exclusive;
    PGFASvmsResetFn reset;
    PGFASvmsGetTelemetryFn get_telemetry;
    PGFASvmsGetRuntimeClockFn get_runtime_clock;
    PGFASvmsSendTimedShortBatchFn send_timed_short_batch;
    PGFASvmsGetOutputClockFn get_output_clock;
    PGFASvmsGetMonotonicClockFn get_monotonic_clock;
    PGFASvmsSetIngressModeFn set_ingress_mode;
};

using PGFASvmsGetInterfaceFn = PGFASvmsResult (__cdecl *)(uint32_t, uint32_t, PGFASvmsInterfacePrefix*);
using PGFAInitializeKDMAPIFn = void* (WINAPI*)();
using PGFATerminateKDMAPIFn = void (WINAPI*)();
using PGFASendDirectDataFn = void (WINAPI*)(DWORD);
using PGFAResetKDMAPIFn = void (WINAPI*)();

static HMODULE PGFALoadSiblingModule(const wchar_t* name)
{
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) {
            *(slash + 1) = L'\0';
            if (wcslen(path) + wcslen(name) + 1 < MAX_PATH) {
                wcscat_s(path, name);
                if (HMODULE module = LoadLibraryW(path))
                    return module;
            }
        }
    }
    return LoadLibraryW(name);
}

class PGFAPlaybackOutputProvider {
public:
    ~PGFAPlaybackOutputProvider() { Close(); }

    bool Open(bool wantKDMAPI, int device)
    {
        Close();
        m_wantKDMAPI = wantKDMAPI;
        m_deviceIndex = device;
        if (wantKDMAPI) {
            if (OpenSvmsNative())
                return true;
            if (OpenKDMAPI())
                return true;
            return false;
        }
        if (device < 0)
            return false;
        m_mode = Mode::WinMM;
        if (!m_winmm.Open(device)) {
            m_mode = Mode::None;
            return false;
        }
        return true;
    }

    void Close()
    {
        if (m_mode == Mode::SvmsNative) {
            if (m_svmsSession && m_svms.destroy_session)
                m_svms.destroy_session(m_svmsSession);
            m_svmsSession = 0;
        } else if (m_mode == Mode::KDMAPI) {
            if (m_terminateKDMAPI)
                m_terminateKDMAPI();
        } else if (m_mode == Mode::WinMM) {
            m_winmm.Close();
        }
        m_mode = Mode::None;
        m_sendDirectData = nullptr;
        m_resetKDMAPI = nullptr;
        m_terminateKDMAPI = nullptr;
        memset(&m_svms, 0, sizeof(m_svms));
        if (m_module) {
            FreeLibrary(m_module);
            m_module = nullptr;
        }
    }

    bool Matches(bool wantKDMAPI, int device) const
    {
        if (wantKDMAPI != m_wantKDMAPI || device != m_deviceIndex)
            return false;
        return m_mode != Mode::None;
    }

    bool IsOpen() const { return m_mode != Mode::None; }
    bool HasExactQpcBatch() const
    {
        return m_mode == Mode::SvmsNative &&
            (m_svms.capabilities & PGFA_SVMS_CAP_EXACT_QPC) != 0 &&
            (m_svms.capabilities & PGFA_SVMS_CAP_SHORT_BATCH) != 0 &&
            m_svms.send_short_batch != nullptr;
    }

    bool Send(uint32_t packed, uint64_t qpc)
    {
        switch (m_mode) {
        case Mode::SvmsNative:
            if (qpc && (m_svms.capabilities & PGFA_SVMS_CAP_EXACT_QPC) && m_svms.send_short_at_qpc)
                return m_svms.send_short_at_qpc(m_svmsSession, packed, qpc) == PGFA_SVMS_OK;
            return m_svms.send_short && m_svms.send_short(m_svmsSession, packed) == PGFA_SVMS_OK;
        case Mode::KDMAPI:
            if (!m_sendDirectData) return false;
            m_sendDirectData((DWORD)packed);
            return true;
        case Mode::WinMM:
            return m_winmm.PlayEvent((unsigned char)(packed & 0xffu),
                (unsigned char)((packed >> 8) & 0xffu),
                (unsigned char)((packed >> 16) & 0xffu));
        default:
            return false;
        }
    }

    bool SendBatch(const PGFASvmsShortEvent* events, size_t count)
    {
        if (!events || count == 0)
            return true;
        if (HasExactQpcBatch()) {
            const uint32_t n = (uint32_t)(std::min)(count, (size_t)UINT32_MAX);
            return n == count && m_svms.send_short_batch(m_svmsSession, events, n) == PGFA_SVMS_OK;
        }
        for (size_t i = 0; i < count; ++i)
            if (!Send(events[i].packed_message, events[i].timestamp_qpc))
                return false;
        return true;
    }

    // Pause/mute must silence currently sounding notes without resetting
    // channel/program state. The scheduler never queues future host events, so
    // All Notes Off + sustain off is sufficient and avoids expensive synth resets.
    void Silence()
    {
        if (m_mode == Mode::SvmsNative) {
            if (!m_svms.send_short) return;
            for (unsigned channel = 0; channel < 16; ++channel) {
                m_svms.send_short(m_svmsSession,
                    (uint32_t)(0xB0u | channel | (0x7Bu << 8)));
                m_svms.send_short(m_svmsSession,
                    (uint32_t)(0xB0u | channel | (0x40u << 8)));
            }
            return;
        }
        if (m_mode == Mode::KDMAPI) {
            if (!m_sendDirectData) return;
            for (unsigned channel = 0; channel < 16; ++channel) {
                m_sendDirectData((DWORD)(0xB0u | channel | (0x7Bu << 8)));
                m_sendDirectData((DWORD)(0xB0u | channel | (0x40u << 8)));
            }
            return;
        }
        if (m_mode == Mode::WinMM)
            m_winmm.AllNotesOff();
    }

    void Panic()
    {
        if (m_mode == Mode::SvmsNative) {
            if (m_svms.reset)
                m_svms.reset(m_svmsSession);
            return;
        }
        if (m_mode == Mode::KDMAPI) {
            if (m_resetKDMAPI) {
                m_resetKDMAPI();
                return;
            }
            if (m_sendDirectData) {
                for (unsigned channel = 0; channel < 16; ++channel) {
                    m_sendDirectData((DWORD)(0xB0u | channel | (0x7Bu << 8)));
                    m_sendDirectData((DWORD)(0xB0u | channel | (0x40u << 8)));
                }
            }
            return;
        }
        if (m_mode == Mode::WinMM) {
            m_winmm.AllNotesOff();
            m_winmm.Reset();
        }
    }

private:
    enum class Mode { None, WinMM, KDMAPI, SvmsNative };

    bool OpenSvmsNative()
    {
        static const wchar_t* names[] = { L"SVMS.dll", L"SVMSAPI.dll" };
        for (const wchar_t* name : names) {
            HMODULE module = PGFALoadSiblingModule(name);
            if (!module)
                continue;
            auto getInterface = reinterpret_cast<PGFASvmsGetInterfaceFn>(
                GetProcAddress(module, "SVMS_GetInterface"));
            if (!getInterface) {
                FreeLibrary(module);
                continue;
            }
            PGFASvmsInterfacePrefix api = {};
            if (getInterface(PGFA_SVMS_ABI_V1, (uint32_t)sizeof(api), &api) != PGFA_SVMS_OK ||
                api.abi_version != PGFA_SVMS_ABI_V1 || !api.create_session || !api.destroy_session || !api.send_short) {
                FreeLibrary(module);
                continue;
            }
            PGFASvmsSessionConfig cfg = {};
            cfg.struct_size = sizeof(cfg);
            cfg.struct_version = PGFA_SVMS_STRUCT_V1;
            PGFASvmsSession session = 0;
            if (api.create_session(&cfg, &session) != PGFA_SVMS_OK || session == 0) {
                FreeLibrary(module);
                continue;
            }
            if ((api.capabilities & PGFA_SVMS_CAP_QUEUE_CONTROL) && api.set_ingress_mode)
                api.set_ingress_mode(session, PGFA_SVMS_INGRESS_LOSSLESS);
            m_module = module;
            m_svms = api;
            m_svmsSession = session;
            m_mode = Mode::SvmsNative;
            return true;
        }
        return false;
    }

    bool OpenKDMAPI()
    {
        HMODULE module = nullptr;
        static const wchar_t* names[] = { L"SVMS.dll", L"SVMSAPI.dll", L"OmniMIDI.dll", L"OmniMIDI" };
        for (const wchar_t* name : names) {
            module = PGFALoadSiblingModule(name);
            if (!module)
                continue;
            auto initialize = reinterpret_cast<PGFAInitializeKDMAPIFn>(GetProcAddress(module, "InitializeKDMAPIStream"));
            auto terminate = reinterpret_cast<PGFATerminateKDMAPIFn>(GetProcAddress(module, "TerminateKDMAPIStream"));
            auto send = reinterpret_cast<PGFASendDirectDataFn>(GetProcAddress(module, "SendDirectData"));
            if (!initialize || !terminate || !send) {
                FreeLibrary(module);
                module = nullptr;
                continue;
            }
            if (!initialize()) {
                FreeLibrary(module);
                module = nullptr;
                continue;
            }
            m_module = module;
            m_terminateKDMAPI = terminate;
            m_sendDirectData = send;
            m_resetKDMAPI = reinterpret_cast<PGFAResetKDMAPIFn>(GetProcAddress(module, "ResetKDMAPIStream"));
            m_mode = Mode::KDMAPI;
            return true;
        }
        return false;
    }

    Mode m_mode = Mode::None;
    bool m_wantKDMAPI = false;
    int m_deviceIndex = -1;
    MIDIOutDevice m_winmm;
    HMODULE m_module = nullptr;
    PGFASvmsInterfacePrefix m_svms = {};
    PGFASvmsSession m_svmsSession = 0;
    PGFATerminateKDMAPIFn m_terminateKDMAPI = nullptr;
    PGFASendDirectDataFn m_sendDirectData = nullptr;
    PGFAResetKDMAPIFn m_resetKDMAPI = nullptr;
};

static uint64_t PGFAQpcNow()
{
    LARGE_INTEGER value = {};
    QueryPerformanceCounter(&value);
    return (uint64_t)value.QuadPart;
}

static uint64_t PGFAQpcFrequency()
{
    static uint64_t frequency = []() -> uint64_t {
        LARGE_INTEGER value = {};
        QueryPerformanceFrequency(&value);
        return (uint64_t)(value.QuadPart > 0 ? value.QuadPart : 1);
    }();
    return frequency;
}

static long long PGFAClampSongTime(long long value, long long lo, long long hi)
{
    return (std::max)(lo, (std::min)(value, hi));
}

} // namespace

MainScreen::~MainScreen()
{
    StopPlaybackAudioThread();
}

void MainScreen::StartPlaybackAudioThread()
{
    if (IsFreePlay() || m_PlaybackAudioThread.joinable())
        return;
    m_bPlaybackAudioExit.store(false, std::memory_order_release);
    m_llPlaybackAudioControlSongUs.store(m_llStartTime, std::memory_order_relaxed);
    m_llPlaybackAudioClockUs.store(m_llStartTime, std::memory_order_relaxed);
    m_uPlaybackAudioControlSerial.fetch_add(1, std::memory_order_release);
    m_uPlaybackAudioSeekSerial.fetch_add(1, std::memory_order_release);
    m_PlaybackAudioThread = std::thread(&MainScreen::PlaybackAudioThreadMain, this);
}

void MainScreen::StopPlaybackAudioThread()
{
    m_bPlaybackAudioExit.store(true, std::memory_order_release);
    if (m_PlaybackAudioThread.joinable() && m_PlaybackAudioThread.get_id() != std::this_thread::get_id())
        m_PlaybackAudioThread.join();
    m_bPlaybackAudioRunning.store(false, std::memory_order_release);
    m_bPlaybackAudioLiveEnabled.store(false, std::memory_order_release);
    m_dPlaybackAudioSchedulerHz.store(0.0, std::memory_order_relaxed);
}

void MainScreen::PlaybackAudioSeek(long long songUs)
{
    if (IsFreePlay())
        return;
    songUs = PGFAClampSongTime(songUs, GetMinTime(), GetMaxTime());
    m_llPlaybackAudioControlSongUs.store(songUs, std::memory_order_relaxed);
    m_llPlaybackAudioClockUs.store(songUs, std::memory_order_relaxed);
    m_uPlaybackAudioSeekSerial.fetch_add(1, std::memory_order_release);
    m_uPlaybackAudioControlSerial.fetch_add(1, std::memory_order_release);
}

long long MainScreen::PlaybackAudioClockNow() const
{
    return m_llPlaybackAudioClockUs.load(std::memory_order_acquire);
}

void MainScreen::SyncPlaybackAudioThread(bool forceSeek)
{
    if (IsFreePlay() || m_bDiscarded)
        return;

    Config& config = Config::GetConfig();
    PlaybackSettings& playback = config.GetPlaybackSettings();
    const AudioSettings& audio = config.GetAudioSettings();
    const VizSettings& viz = config.GetVizSettings();

    const bool customAudio = m_bUseCustomAudio && !m_sCustomAudioPath.empty();
    const bool preConfigured = !customAudio && audio.bPreRenderAudio;
    // s_bPreFailed is owned by the legacy prerender path in this translation
    // unit. When it gives up, the scheduler takes over from the current clock.
    const bool live = !g_bVideoRendering && !customAudio && (!preConfigured || s_bPreFailed);
    const bool paused = playback.GetPaused();
    const bool muted = playback.GetMute();
    const bool pianoOverride = config.m_bPianoOverride;
    const bool wantKDMAPI = viz.bKDMAPI;
    const int device = audio.iOutDevice;
    const double speed = (std::max)(0.0001, playback.GetSpeed());

    const MIDI::MIDIInfo& info = m_MIDI.GetInfo();
    const double maxCorrect = info.iMaxVolume > 0 ? 127.0 / info.iMaxVolume : 1.0;
    double velocityScale = info.iVolumeSum > 0
        ? (playback.GetVolume() * 127.0 * info.iNoteCount) / info.iVolumeSum
        : 1.0;
    velocityScale = (std::min)(velocityScale, maxCorrect);

    m_bPlaybackAudioLiveEnabled.store(live, std::memory_order_release);
    m_bPlaybackAudioPaused.store(paused, std::memory_order_release);
    m_bPlaybackAudioMute.store(muted, std::memory_order_release);
    m_bPlaybackAudioPianoOverride.store(pianoOverride, std::memory_order_release);
    m_bPlaybackAudioWantKDMAPI.store(wantKDMAPI, std::memory_order_release);
    m_iPlaybackAudioDevice.store(device, std::memory_order_release);
    m_dPlaybackAudioSpeed.store(speed, std::memory_order_release);
    m_dPlaybackAudioVelocityScale.store(velocityScale, std::memory_order_release);
    m_fPlaybackAudioCorruption.store(GetCorruptorAmount(), std::memory_order_release);

    std::vector<uint16_t> muteMask(m_vTrackSettings.size(), 0u);
    for (size_t track = 0; track < m_vTrackSettings.size(); ++track) {
        uint16_t mask = 0u;
        for (unsigned channel = 0; channel < 16; ++channel)
            if (m_vTrackSettings[track].aChannels[channel].bMuted)
                mask |= (uint16_t)(1u << channel);
        muteMask[track] = mask;
    }
    if (muteMask != m_vPlaybackAudioLastMuteMask) {
        m_vPlaybackAudioLastMuteMask = muteMask;
        std::shared_ptr<const std::vector<uint16_t>> snapshot =
            std::make_shared<const std::vector<uint16_t>>(std::move(muteMask));
        std::atomic_store_explicit(&m_pPlaybackAudioMuteMask, snapshot, std::memory_order_release);
    }

    const bool controlChanged = !m_bPlaybackAudioControlInitialized || forceSeek ||
        paused != m_bPlaybackAudioLastPaused || live != m_bPlaybackAudioLastLive ||
        wantKDMAPI != m_bPlaybackAudioLastKDMAPI || device != m_iPlaybackAudioLastDevice ||
        std::fabs(speed - m_dPlaybackAudioLastSpeed) > 1e-9;

    if (controlChanged) {
        long long anchor = m_llStartTime;
        if (m_bPlaybackAudioControlInitialized && !forceSeek)
            anchor = PlaybackAudioClockNow();
        anchor = PGFAClampSongTime(anchor, GetMinTime(), GetMaxTime());
        m_llPlaybackAudioControlSongUs.store(anchor, std::memory_order_relaxed);
        if (forceSeek || (!m_bPlaybackAudioLastLive && live))
            m_uPlaybackAudioSeekSerial.fetch_add(1, std::memory_order_release);
        m_uPlaybackAudioControlSerial.fetch_add(1, std::memory_order_release);
        m_bPlaybackAudioControlInitialized = true;
        m_bPlaybackAudioLastPaused = paused;
        m_bPlaybackAudioLastLive = live;
        m_bPlaybackAudioLastKDMAPI = wantKDMAPI;
        m_iPlaybackAudioLastDevice = device;
        m_dPlaybackAudioLastSpeed = speed;
    }
}

void MainScreen::PlaybackAudioThreadMain()
{
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    m_bPlaybackAudioRunning.store(true, std::memory_order_release);

    PGFAPlaybackOutputProvider output;
    const uint64_t qpcFreq = PGFAQpcFrequency();
    uint64_t anchorQpc = PGFAQpcNow();
    long long anchorSongUs = m_llPlaybackAudioControlSongUs.load(std::memory_order_acquire);
    double localSpeed = m_dPlaybackAudioSpeed.load(std::memory_order_acquire);
    bool localPaused = true;
    bool localLive = false;
    bool lastPanicPaused = true;
    bool lastPanicMute = false;
    unsigned long long controlSerial = ~0ULL;
    unsigned long long seekSerial = ~0ULL;
    size_t eventPos = 0;
    bool providerWasLive = false;

    uint64_t hzStart = PGFAQpcNow();
    uint64_t hzTicks = 0;
    std::vector<PGFASvmsShortEvent> eventBatch;
    eventBatch.reserve(4096);

    auto seekEventPos = [&](long long songUs) {
        auto it = std::lower_bound(m_vEvents.begin(), m_vEvents.end(), songUs,
            [&](MIDIChannelEvent event, long long value) {
                return m_MIDI.GetEventTime(event) < value;
            });
        eventPos = (size_t)(it - m_vEvents.begin());
    };

    while (!m_bPlaybackAudioExit.load(std::memory_order_acquire)) {
        const uint64_t nowQpc = PGFAQpcNow();
        ++hzTicks;
        if (nowQpc - hzStart >= qpcFreq / 2) {
            const double elapsed = (double)(nowQpc - hzStart) / (double)qpcFreq;
            m_dPlaybackAudioSchedulerHz.store(elapsed > 0.0 ? hzTicks / elapsed : 0.0,
                std::memory_order_relaxed);
            hzStart = nowQpc;
            hzTicks = 0;
        }

        const unsigned long long newControl = m_uPlaybackAudioControlSerial.load(std::memory_order_acquire);
        const unsigned long long newSeek = m_uPlaybackAudioSeekSerial.load(std::memory_order_acquire);
        const bool live = m_bPlaybackAudioLiveEnabled.load(std::memory_order_acquire);
        const bool paused = m_bPlaybackAudioPaused.load(std::memory_order_acquire);
        const bool globalMute = m_bPlaybackAudioMute.load(std::memory_order_acquire);
        const double speed = (std::max)(0.0001, m_dPlaybackAudioSpeed.load(std::memory_order_acquire));

        if (newControl != controlSerial) {
            anchorSongUs = m_llPlaybackAudioControlSongUs.load(std::memory_order_acquire);
            anchorQpc = nowQpc;
            localSpeed = speed;
            localPaused = paused;
            controlSerial = newControl;
        } else {
            // These atomics normally change together with controlSerial; loading
            // them here also makes the thread robust to a partially observed UI
            // frame without ever consulting renderer-owned state directly.
            localPaused = paused;
            localSpeed = speed;
        }

        long long songUs = anchorSongUs;
        if (!localPaused) {
            const double wallUs = (double)(nowQpc - anchorQpc) * 1000000.0 / (double)qpcFreq;
            songUs += (long long)(wallUs * localSpeed + 0.5);
        }
        songUs = PGFAClampSongTime(songUs, GetMinTime(), GetMaxTime());
        m_llPlaybackAudioClockUs.store(songUs, std::memory_order_release);

        if (newSeek != seekSerial) {
            seekSerial = newSeek;
            seekEventPos(songUs);
            output.Panic();
        }

        const bool wantKDMAPI = m_bPlaybackAudioWantKDMAPI.load(std::memory_order_acquire);
        const int device = m_iPlaybackAudioDevice.load(std::memory_order_acquire);
        if (!live) {
            if (providerWasLive) {
                output.Panic();
                output.Close();
                providerWasLive = false;
            }
            localLive = false;
            lastPanicPaused = paused;
            lastPanicMute = globalMute;
            Sleep(1);
            continue;
        }

        if (!localLive) {
            seekEventPos(songUs);
            localLive = true;
        }

        if (!output.Matches(wantKDMAPI, device)) {
            output.Panic();
            output.Close();
            output.Open(wantKDMAPI, device);
        }
        providerWasLive = output.IsOpen();

        // Pause/mute transitions silence once without resetting the synth.
        // No future host MIDI is queued, so this cannot leave scheduled notes
        // behind and it preserves program/controller state across resume.
        if ((localPaused && !lastPanicPaused) || (globalMute && !lastPanicMute))
            output.Silence();
        lastPanicPaused = localPaused;
        lastPanicMute = globalMute;

        if (localPaused || !output.IsOpen()) {
            Sleep(1);
            continue;
        }

        const double velocityScale = m_dPlaybackAudioVelocityScale.load(std::memory_order_acquire);
        const float corruptor = m_fPlaybackAudioCorruption.load(std::memory_order_acquire);
        const auto muteSnapshot = std::atomic_load_explicit(&m_pPlaybackAudioMuteMask, std::memory_order_acquire);

        // The scheduler itself runs independently at ~1 kHz. Keep the host
        // cursor strictly due-now even for native SVMS: dense equal-time events
        // are still submitted as one exact-QPC batch, but pause/mute/provider
        // changes can never leave already-queued future MIDI behind.
        const bool exactBatch = output.HasExactQpcBatch();
        const long long lookAheadWallUs = 0LL;
        const long long deadlineSongUs = songUs + (long long)(lookAheadWallUs * localSpeed + 0.5);
        const size_t startPos = eventPos;
        eventBatch.clear();
        size_t processed = 0;
        const size_t maxEventsThisPass = exactBatch ? 4096u : 131072u;

        while (eventPos < m_vEvents.size() && processed < maxEventsThisPass) {
            const MIDIChannelEvent event = m_vEvents[eventPos];
            const long long eventTime = m_MIDI.GetEventTime(event);
            if (eventTime > deadlineSongUs)
                break;

            int p1 = m_MIDI.GetEventParam1(event);
            int p2 = m_MIDI.GetEventParam2(event);
            uint64_t corruptSeed = 0;
            const MIDI::ChannelEventType type = m_MIDI.GetEventChannelEventType(event);
            if (type == MIDI::NoteOn || type == MIDI::NoteOff) {
                MIDIChannelEvent sister = UINT32_MAX;
                if (m_MIDI.EventHasSister(event)) {
                    const unsigned sisterIdx = m_MIDI.GetEventSisterIdx(event);
                    if (sisterIdx < m_vEvents.size())
                        sister = m_vEvents[sisterIdx];
                }
                corruptSeed = CorruptSeed(event, sister);
                p1 = CorruptPitch(corruptor, corruptSeed, p1);
            } else if (type == MIDI::Controller || type == MIDI::ProgramChange) {
                corruptSeed = CorruptSeed(event, UINT32_MAX);
                p1 = CorruptValue(corruptor, corruptSeed, p1);
            } else if (type == MIDI::PitchBend) {
                corruptSeed = CorruptSeed(event, UINT32_MAX);
                p2 = CorruptPitchBend(corruptor, corruptSeed,
                    (m_MIDI.GetEventParam2(event) << 7) | m_MIDI.GetEventParam1(event));
            } else if (type == MIDI::NoteAftertouch || type == MIDI::ChannelAftertouch) {
                corruptSeed = CorruptSeed(event, UINT32_MAX);
                p1 = CorruptPitch(corruptor, corruptSeed, p1);
                p2 = CorruptValue(corruptor, corruptSeed, p2);
            }

            bool send = true;
            if (type == MIDI::ProgramChange && m_MIDI.GetEventChannel(event) != MIDI::Drums &&
                m_bPlaybackAudioPianoOverride.load(std::memory_order_acquire))
                p1 = 0;
            if (type == MIDI::PitchBend) {
                const int bend = p2;
                p1 = bend & 0x7f;
                p2 = (bend >> 7) & 0x7f;
            } else if (type == MIDI::NoteOn) {
                const unsigned track = m_MIDI.GetEventTrack(event);
                const unsigned channel = m_MIDI.GetEventChannel(event);
                const bool channelMuted = muteSnapshot && track < muteSnapshot->size() &&
                    (((*muteSnapshot)[track] >> channel) & 1u) != 0;
                if (globalMute || channelMuted) {
                    send = false;
                } else {
                    p2 = (int)(CorruptVelocity(corruptor, corruptSeed, m_MIDI.GetEventParam2(event)) * velocityScale + 0.5);
                    p2 = (std::max)(0, (std::min)(p2, 127));
                }
            }

            if (send) {
                p1 = (std::max)(0, (std::min)(p1, 127));
                p2 = (std::max)(0, (std::min)(p2, 127));
                const uint32_t packed = (uint32_t)m_MIDI.GetEventCode(event) |
                    ((uint32_t)p1 << 8) | ((uint32_t)p2 << 16);
                uint64_t eventQpc = 0;
                if (exactBatch) {
                    const double eventWallUs = (double)(eventTime - anchorSongUs) / localSpeed;
                    const double qpcDelta = eventWallUs * (double)qpcFreq / 1000000.0;
                    const double qpcAbsolute = (double)anchorQpc + qpcDelta;
                    eventQpc = qpcAbsolute > 0.0 ? (uint64_t)(qpcAbsolute + 0.5) : nowQpc;
                }
                eventBatch.push_back({ eventQpc, packed, 0u });
            }
            ++eventPos;
            ++processed;
        }

        if (!eventBatch.empty() && output.IsOpen()) {
            if (!output.SendBatch(eventBatch.data(), eventBatch.size())) {
                // Exact SVMS submits this pass atomically as one batch. Generic
                // output can only fail on an actual device error; rewinding is
                // still preferable to silently dropping the remainder.
                eventPos = startPos;
                output.Close();
            }
        }

        if (processed >= maxEventsThisPass) {
            SwitchToThread();
            continue;
        }

        Sleep(1);
    }

    output.Panic();
    output.Close();
    m_bPlaybackAudioRunning.store(false, std::memory_order_release);
    timeEndPeriod(1);
}

// The legacy implementation is compiled under LogicLegacy/MsgProcLegacy/
// DiscardLegacy aliases by GameState.cpp. These wrappers are the only bridge
// between renderer cadence and the independent output clock.
GameState::GameError SplashScreen::Logic() { return LogicLegacy(); }
GameState::GameError SplashScreen::MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return MsgProcLegacy(hWnd, msg, wParam, lParam);
}
GameState::GameError IntroScreen::Logic() { return LogicLegacy(); }
GameState::GameError IntroScreen::MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return MsgProcLegacy(hWnd, msg, wParam, lParam);
}

GameState::GameError MainScreen::MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    const long long before = m_llStartTime;
    GameError result = MsgProcLegacy(hWnd, msg, wParam, lParam);
    if (!IsFreePlay() && m_llStartTime != before) {
        StartPlaybackAudioThread();
        PlaybackAudioSeek(m_llStartTime);
    }
    return result;
}

GameState::GameError MainScreen::Logic(void)
{
    if (IsFreePlay())
        return LogicLegacy();

    const VizSettings& viz = Config::GetConfig().GetVizSettings();
    m_OutDevice.SetThreadOwned(true, viz.bKDMAPI);
    StartPlaybackAudioThread();
    SyncPlaybackAudioThread(false);

    // For live synth playback the audio QPC clock is authoritative, including
    // the exact pause position. Resetting the legacy frame timer here prevents
    // its old FPS-based elapsed step from double-advancing after the snap.
    if (m_bPlaybackAudioLiveEnabled.load(std::memory_order_acquire) && !g_bVideoRendering) {
        const long long audioClock = PlaybackAudioClockNow();
        if (audioClock >= GetMinTime() && audioClock <= GetMaxTime()) {
            m_llStartTime = audioClock;
            m_llDisplayTime = audioClock;
            m_Timer.Start();
        }
    }

    GameError result = LogicLegacy();
    SyncPlaybackAudioThread(false);
    return result;
}

void MainScreen::Discard()
{
    StopPlaybackAudioThread();
    m_OutDevice.SetThreadOwned(false, false);
    DiscardLegacy();
}

GameState::GameError FreePlayScreen::Logic() { return LogicLegacy(); }
GameState::GameError FreePlayScreen::MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return MsgProcLegacy(hWnd, msg, wParam, lParam);
}
