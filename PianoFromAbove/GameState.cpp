#include <algorithm>
#include <cmath>
#include <tchar.h>
#include <ppl.h>
#include <dwmapi.h>
#include <fstream>
#include <pdh.h>
#include <thread>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <type_traits>
#include <deque>
#include <chrono>
#include "Globals.h"
#include "GameState.h"
#include "Config.h"
#include "resource.h"
#include "ConfigProcs.h"
#include "MainProcs.h"
#include "MIDIPreRenderPlayer.h"
#include <d3d9types.h>
#include "ImageBufferMultipass.h"
#include "ImageBufferOverlapIndex.h"
#include "ImageBufferPreparedChunks.h"
#include "ImageBufferGpuPins.h"

// The immutable full-song prepared source is deliberately a memory-for-speed
// optimization. Its persistent cost is driven by the number of notes stored in
// the compact prepared source. The observed 59.4M-event stress case contains
// 29.7M notes, so keep enough headroom for it while still bounding the extra
// prepared-source memory to roughly one GiB of raw-note records.
static constexpr size_t ImageBufferFullPreparedMaxEvents = 64000000;
static constexpr size_t ImageBufferFullPreparedMaxNotes = 32000000;

struct ImageBufferPrewarmGpuState {
    const void* owner = nullptr;
    Renderer* renderer = nullptr;
    bool initialized = false;
    bool cacheRequired = false;
    bool playRequested = false;
    bool wakeIssued = false;
    size_t cached = 0;
    size_t total = 0;
    std::vector<long long> chunks;
};

static std::mutex s_ImageBufferPrewarmGpuMutex;
static ImageBufferPrewarmGpuState s_ImageBufferPrewarmGpu;

static bool ImageBufferFullPreparedSupported(
    const std::vector<MIDIChannelEvent>& events,
    const MIDI& midi)
{
    return events.size() <= ImageBufferFullPreparedMaxEvents &&
        midi.GetInfo().iNoteCount <= ImageBufferFullPreparedMaxNotes;
}

static void ImageBufferLogFullPrewarmSkip(
    const void* owner, size_t events, size_t notes)
{
    static const void* lastOwner = nullptr;
    static size_t lastEvents = 0;
    static size_t lastNotes = 0;
    if (owner == lastOwner && events == lastEvents && notes == lastNotes)
        return;
    lastOwner = owner;
    lastEvents = events;
    lastNotes = notes;
    char log[160];
    sprintf_s(log, "imgprep:skip-full events=%zu notes=%zu fallback=local", events, notes);
    HeartbeatLog(log);
}

template <typename BuildFn>
static void ImageBufferCollectHugeLocal(
    const std::vector<MIDIChannelEvent>& events,
    MIDI& midi,
    std::vector<NoteData>& out,
    long long chunk,
    long long timeSpan,
    long long corruptionMargin,
    long long maxNoteLength,
    bool tickMode,
    BuildFn&& buildNote)
{
    out.clear();
    if (events.empty() || timeSpan <= 0)
        return;

    const long long chunkStart = chunk * timeSpan;
    const long long chunkEnd = ImageBufferOverlapSaturatingAdd(chunkStart, timeSpan);
    const long long lookBack = ImageBufferOverlapSaturatingAdd(
        (std::max)(maxNoteLength, 0LL), corruptionMargin);
    const long long lo = ImageBufferOverlapSaturatingAdd(chunkStart, -lookBack);
    const long long hi = ImageBufferOverlapSaturatingAdd(chunkEnd, corruptionMargin);

    auto eventLessTime = [&](MIDIChannelEvent event, long long value) {
        return (tickMode ? (long long)midi.GetEventAbsT(event) : midi.GetEventTime(event)) < value;
    };
    auto itLo = std::lower_bound(events.begin(), events.end(), lo, eventLessTime);
    auto itHi = std::lower_bound(events.begin(), events.end(), hi, eventLessTime);

    for (auto it = itHi; it != itLo; ) {
        --it;
        const MIDIChannelEvent event = *it;
        if (midi.GetEventChannelEventType(event) != MIDI::NoteOn ||
            midi.GetEventParam2(event) <= 0 || !midi.EventHasSister(event))
            continue;

        NoteData data = buildNote(event, chunkStart);
        if (data.pos < (float)timeSpan &&
            data.pos + (std::max)(data.length, 0.0f) >= 0.0f)
            out.push_back(data);
    }
}

static size_t CountImageBufferPrewarmCached(
    Renderer* renderer, const std::vector<long long>& chunks)
{
    if (!renderer)
        return 0;
    size_t cached = 0;
    for (long long chunk : chunks)
        if (renderer->ImageBufferChunkCached(chunk))
            ++cached;
    return cached;
}

static void UpdateImageBufferPrewarmGpuProgress(
    const void* owner,
    Renderer* renderer,
    long long timeSpan,
    long long margin,
    bool tickMode)
{
    if (!owner || !renderer || !ImageBufferPreparedGetWaitBeforePlayback())
        return;

    const ImageBufferPreparedProgress cpu = ImageBufferPreparedGetFullProgress();
    if (!cpu.initialized)
        return;

    if (cpu.unsupported) {
        ImageBufferClearPinnedChunks(renderer);
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        s_ImageBufferPrewarmGpu.owner = owner;
        s_ImageBufferPrewarmGpu.renderer = renderer;
        s_ImageBufferPrewarmGpu.initialized = true;
        s_ImageBufferPrewarmGpu.cacheRequired = false;
        s_ImageBufferPrewarmGpu.cached = 0;
        s_ImageBufferPrewarmGpu.total = 0;
        s_ImageBufferPrewarmGpu.chunks.clear();
        return;
    }

    // Full-prime discovers every truly dense center first. Runtime preparation
    // deliberately expands each center by +/- ImageBufferPreparedPreloadRadius,
    // so pre-play GPU work must use that same expanded set or playback will
    // gradually discover and bake extra chunks after the gate has opened.
    if (cpu.done < cpu.total)
        return;

    auto& overlap = ImageBufferOverlapIndexGet();
    const auto source = overlap.preparedSource;
    if (overlap.owner != owner || !source || source->notes.empty() || timeSpan <= 0)
        return;

    std::vector<long long> prewarmChunks;
    const long long firstStart = ImageBufferPreparedStartValue(source->notes.front(), tickMode);
    const long long lastStart = ImageBufferPreparedStartValue(source->notes.back(), tickMode);
    const long long first = ImageBufferPreparedFloorDiv(
        ImageBufferOverlapSaturatingAdd(firstStart, -margin), timeSpan) - 1;
    const long long last = ImageBufferPreparedFloorDiv(
        ImageBufferOverlapSaturatingAdd(lastStart, margin), timeSpan) + 1;

    for (long long center = first; center <= last; ++center) {
        const size_t estimate = ImageBufferPreparedEstimateStarts(
            *source, center, timeSpan, margin, tickMode);
        if (estimate < ImageBufferPreparedDenseThreshold)
            continue;

        const long long lo = (std::max)(first,
            center - (long long)ImageBufferPreparedPreloadRadius);
        const long long hi = (std::min)(last,
            center + (long long)ImageBufferPreparedPreloadRadius);
        long long appendFrom = lo;
        if (!prewarmChunks.empty())
            appendFrom = (std::max)(appendFrom, prewarmChunks.back() + 1);
        for (long long chunk = appendFrom; chunk <= hi; ++chunk)
            prewarmChunks.push_back(chunk);
    }

    // The cache is associative: sparse chunks elsewhere in the song do not
    // matter. Only the dense/preload working set has to fit in the 64 slots.
    // Keep a few slots free for visible sparse chunks. Without this
    // reserve, a fully pinned dense set could leave ordinary playback nowhere
    // to cache the current screen.
    static constexpr size_t kImageBufferRuntimeReserveSlots = 8;
    const bool fitsTextureCache =
        prewarmChunks.size() <= (size_t)Renderer::ChunkPoolSize - kImageBufferRuntimeReserveSlots;

    // A failed full-prime center falls back to exact rendering. Do not make Play
    // wait forever for a texture that cannot be produced.
    const bool requireGpu = fitsTextureCache && cpu.failed == 0;
    const size_t cached = requireGpu
        ? CountImageBufferPrewarmCached(renderer, prewarmChunks)
        : 0;

    // Protect the exact set Play waited for from speculative-lookahead LRU
    // eviction. This is what turns prewarm into lasting residency rather than
    // work that can be thrown away before the dense passage is reached.
    if (requireGpu)
        ImageBufferSetPinnedChunks(renderer, prewarmChunks);
    else
        ImageBufferClearPinnedChunks(renderer);

    std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
    const bool keepPlayRequest = s_ImageBufferPrewarmGpu.playRequested;
    s_ImageBufferPrewarmGpu.owner = owner;
    s_ImageBufferPrewarmGpu.renderer = renderer;
    s_ImageBufferPrewarmGpu.initialized = true;
    s_ImageBufferPrewarmGpu.cacheRequired = requireGpu && !prewarmChunks.empty();
    s_ImageBufferPrewarmGpu.cached = cached;
    s_ImageBufferPrewarmGpu.total = prewarmChunks.size();
    s_ImageBufferPrewarmGpu.chunks = std::move(prewarmChunks);
    s_ImageBufferPrewarmGpu.playRequested = keepPlayRequest;
}

static void DrawImageBufferPrewarmProgress(
    Renderer* renderer, float notesX, float notesCX, float keyboardY)
{
    if (!renderer || !ImageBufferPreparedGetWaitBeforePlayback())
        return;

    const ImageBufferPreparedProgress cpu = ImageBufferPreparedGetFullProgress();

    bool playRequested = false;
    bool gpuStage = false;
    bool initializing = false;
    size_t done = cpu.done;
    size_t total = cpu.total;
    size_t failed = cpu.failed;
    {
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        playRequested = s_ImageBufferPrewarmGpu.playRequested;
        if (cpu.initialized && cpu.done >= cpu.total && s_ImageBufferPrewarmGpu.cacheRequired) {
            gpuStage = true;
            done = s_ImageBufferPrewarmGpu.cached;
            total = s_ImageBufferPrewarmGpu.total;
            failed = 0;
        }
    }

    // Never leave a stale 0/N prewarm bar on screen after the Play
    // request has been cancelled or the gate has legitimately opened.
    if (!playRequested)
        return;

    if (!cpu.initialized) {
        initializing = true;
        done = 0;
        total = 1;
        failed = 0;
    } else if (cpu.unsupported) {
        return;
    }

    if (!initializing && (total == 0 || done >= total))
        return;

    const float scale = (std::max)(Config::GetConfig().GetVizSettings().fUIScale, 0.5f);
    const float bufferW = (float)renderer->GetBufferWidth();
    float x0 = (std::max)(12.0f * scale, notesX);
    float x1 = (std::min)(bufferW - 12.0f * scale, notesX + notesCX);
    if (x1 - x0 < 160.0f * scale) {
        x0 = 12.0f * scale;
        x1 = bufferW - 12.0f * scale;
    }

    const float barH = 8.0f * scale;
    const float y1 = keyboardY - 8.0f * scale;
    const float y0 = y1 - barH;
    const float fraction = (float)done / (float)(std::max)((size_t)1, total);
    auto* draw = renderer->GetDrawList();
    if (!draw)
        return;

    const float rounding = barH * 0.5f;
    draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 180), rounding);
    draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + (x1 - x0) * fraction, y1),
        IM_COL32(235, 235, 235, 230), rounding);

    char text[128];
    if (initializing) {
        sprintf_s(text, "Initializing dense image buffers...");
    } else if (gpuStage) {
        sprintf_s(text, "Prewarming dense image buffers  %zu / %zu", done, total);
    } else if (failed > 0) {
        sprintf_s(text, "Preparing dense image buffers  %zu / %zu  (%zu fallback)",
            done, total, failed);
    } else {
        sprintf_s(text, "Preparing dense image buffers  %zu / %zu", done, total);
    }
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float tx = x0 + ((x1 - x0) - textSize.x) * 0.5f;
    const float ty = y0 - textSize.y - 3.0f * scale;
    draw->AddText(ImVec2(tx + 1.0f, ty + 1.0f), IM_COL32(0, 0, 0, 220), text);
    draw->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, 255), text);
}

// RenderText() resets the same ImDrawList used by the note pass. Draw the
// indicator only after EndText so the reset cannot erase it before Present.
static void DrawImageBufferPrewarmProgressLate(Renderer* renderer)
{
    if (!renderer)
        return;
    const float bufferW = (float)renderer->GetBufferWidth();
    const float bufferH = (float)renderer->GetBufferHeight();
    DrawImageBufferPrewarmProgress(renderer, 0.0f, bufferW, bufferH * 0.75f);
}

static void DrawFrameTimeGraphLate(Renderer* renderer);

static bool ImageBufferPrewarmPlayRequestedFor(const void* owner)
{
    std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
    return s_ImageBufferPrewarmGpu.playRequested &&
        (!s_ImageBufferPrewarmGpu.owner || s_ImageBufferPrewarmGpu.owner == owner);
}

// Dense chunks use a separate CPU preparation path. Sparse chunks keep the
// exact overlap collector and existing multipass behavior. Prepared chunks are
// built from an immutable compact note source on worker threads, rasterized to
// the vertical note resolution, and merged back into a small set of NoteData
// runs before they ever reach the renderer.
#define CollectChunk(k) ([&]() { \
    const bool imageBufferPreparedAllowed = ImageBufferFullPreparedSupported(m_vEvents, m_MIDI); \
    auto ImageBufferExactCollector = [&](long long imageBufferChunk) { \
        if (imageBufferPreparedAllowed) { \
            ImageBufferOverlapCollect(this, m_vEvents, m_MIDI, chunkNotes, imageBufferChunk, T, E, bTickMode, \
                [&](MIDIChannelEvent imageBufferNote, long long imageBufferChunkStart) { \
                    return BuildChunkNoteData(imageBufferNote, imageBufferChunkStart); \
                }); \
        } else { \
            const long long imageBufferMaxLen = bTickMode ? m_llMaxNoteLenTicks : m_llMaxNoteLen; \
            ImageBufferCollectHugeLocal(m_vEvents, m_MIDI, chunkNotes, imageBufferChunk, T, E, \
                imageBufferMaxLen, bTickMode, \
                [&](MIDIChannelEvent imageBufferNote, long long imageBufferChunkStart) { \
                    return BuildChunkNoteData(imageBufferNote, imageBufferChunkStart); \
                }); \
        } \
    }; \
    bool imageBufferAnyHidden = false; \
    for (const auto& imageBufferTrack : m_vTrackSettings) { \
        for (int imageBufferChannel = 0; imageBufferChannel < 16; ++imageBufferChannel) { \
            if (imageBufferTrack.aChannels[imageBufferChannel].bHidden) { \
                imageBufferAnyHidden = true; \
                break; \
            } \
        } \
        if (imageBufferAnyHidden) break; \
    } \
    if (imageBufferAnyHidden || !imageBufferPreparedAllowed) \
        ImageBufferPreparedGet().ClearPending(m_pRenderer, (k)); \
    if ((imageBufferAnyHidden || !imageBufferPreparedAllowed) && (k) == kFirst) \
        ImageBufferPreparedMarkPrewarmUnavailable(this); \
    const int imageBufferPrepRows = (int)std::ceil(std::fabs(notesCY)); \
    const bool imageBufferPreparedHandled = imageBufferPreparedAllowed && !imageBufferAnyHidden && ImageBufferPreparedTryCollect( \
        this, m_pRenderer, m_vEvents, m_MIDI, chunkNotes, (k), \
        kFirst, kLast, kMax, T, E, bTickMode, fCorrupt, \
        m_vTrackSettings.size(), imageBufferPrepRows); \
    if ((k) == kFirst) \
        UpdateImageBufferPrewarmGpuProgress(this, m_pRenderer, T, E, bTickMode); \
    if (!imageBufferPreparedHandled) \
        ImageBufferMPCollectDispatch(m_pRenderer, chunkNotes, ImageBufferExactCollector, (k)); \
}())

// Prewarm cannot depend on RenderNotesImageBuffer(): while Play is gated the
// song can sit in its empty pre-roll, where RenderNotes() returns before image
// buffers are touched. Preserve the renderer member call, then run a
// MainScreen-only preparation/bake step immediately after the frame begins.
// This avoids rewriting the token after `->`, which is invalid C++.
#define ClearAndBeginScene(...) ClearAndBeginScene(__VA_ARGS__); \
([&](auto* imageBufferPrewarmSelf) { \
    using ImageBufferPrewarmSelfT = std::remove_pointer_t<decltype(imageBufferPrewarmSelf)>; \
    if constexpr (std::is_same_v<ImageBufferPrewarmSelfT, MainScreen>) { \
        const bool imageBufferPrewarmActive = !imageBufferPrewarmSelf->m_bDiscarded && \
            ImageBufferPreparedGetWaitBeforePlayback() && \
            Config::GetConfig().GetVizSettings().bImageBufferNotes && \
            !g_bVideoRendering && \
            ImageBufferPrewarmPlayRequestedFor(imageBufferPrewarmSelf); \
        if (imageBufferPrewarmActive) { \
            if (imageBufferPrewarmSelf->m_bImageBufferNeedsInvalidate) { \
                imageBufferPrewarmSelf->m_pRenderer->ImageBufferInvalidate(); \
                imageBufferPrewarmSelf->m_bImageBufferNeedsInvalidate = false; \
            } \
            imageBufferPrewarmSelf->m_pRenderer->ImageBufferSetEventCount( \
                (unsigned long long)imageBufferPrewarmSelf->m_vEvents.size()); \
            if (!imageBufferPrewarmSelf->m_pRenderer->ImageBufferCanRender()) { \
                ImageBufferPreparedMarkPrewarmUnavailable(imageBufferPrewarmSelf); \
                UpdateImageBufferPrewarmGpuProgress( \
                    imageBufferPrewarmSelf, imageBufferPrewarmSelf->m_pRenderer, 1, 0, false); \
            } else { \
                bool imageBufferPrewarmAnyHidden = false; \
                for (const auto& imageBufferPrewarmTrack : imageBufferPrewarmSelf->m_vTrackSettings) { \
                    for (int imageBufferPrewarmChannel = 0; imageBufferPrewarmChannel < 16; ++imageBufferPrewarmChannel) { \
                        if (imageBufferPrewarmTrack.aChannels[imageBufferPrewarmChannel].bHidden) { \
                            imageBufferPrewarmAnyHidden = true; \
                            break; \
                        } \
                    } \
                    if (imageBufferPrewarmAnyHidden) break; \
                } \
                if (imageBufferPrewarmAnyHidden) { \
                    ImageBufferPreparedMarkPrewarmUnavailable(imageBufferPrewarmSelf); \
                } else { \
                    const long long imageBufferPrewarmT = imageBufferPrewarmSelf->m_llTimeSpan; \
                    if (imageBufferPrewarmT > 0 && !imageBufferPrewarmSelf->m_vEvents.empty()) { \
                        const bool imageBufferPrewarmTickMode = imageBufferPrewarmSelf->m_bTickMode; \
                        const float imageBufferPrewarmCorrupt = imageBufferPrewarmSelf->GetCorruptorAmount(); \
                        const long long imageBufferPrewarmE = 1 + (long long)std::ceil( \
                            (double)imageBufferPrewarmT * 0.10 * (double)imageBufferPrewarmCorrupt); \
                        int imageBufferPrewarmRows = (int)std::ceil(std::fabs(imageBufferPrewarmSelf->m_fNotesCY)); \
                        imageBufferPrewarmRows = (std::min)((std::max)(imageBufferPrewarmRows, 64), \
                            ImageBufferPreparedMaxRows); \
                        if (!ImageBufferFullPreparedSupported( \
                            imageBufferPrewarmSelf->m_vEvents, imageBufferPrewarmSelf->m_MIDI)) { \
                            ImageBufferLogFullPrewarmSkip(imageBufferPrewarmSelf, \
                                imageBufferPrewarmSelf->m_vEvents.size(), \
                                imageBufferPrewarmSelf->m_MIDI.GetInfo().iNoteCount); \
                            ImageBufferPreparedMarkPrewarmUnavailable(imageBufferPrewarmSelf); \
                            UpdateImageBufferPrewarmGpuProgress( \
                                imageBufferPrewarmSelf, imageBufferPrewarmSelf->m_pRenderer, 1, 0, false); \
                        } else { \
                            auto& imageBufferPrewarmOverlap = ImageBufferOverlapEnsureIndex( \
                                imageBufferPrewarmSelf, imageBufferPrewarmSelf->m_vEvents, imageBufferPrewarmSelf->m_MIDI); \
                            const auto imageBufferPrewarmSource = imageBufferPrewarmOverlap.preparedSource; \
                            if (!imageBufferPrewarmSource || imageBufferPrewarmSource->notes.empty() || \
                                imageBufferPrewarmCorrupt > 1.0f || \
                                (imageBufferPrewarmTickMode ? imageBufferPrewarmSource->tickOverflow : imageBufferPrewarmSource->timeOverflow)) { \
                                ImageBufferPreparedMarkPrewarmUnavailable(imageBufferPrewarmSelf); \
                            } else { \
                                const uint64_t imageBufferPrewarmSignature = ImageBufferPreparedSignature( \
                                    imageBufferPrewarmSource.get(), imageBufferPrewarmT, imageBufferPrewarmTickMode, \
                                    imageBufferPrewarmCorrupt, imageBufferPrewarmSelf->m_vTrackSettings.size(), imageBufferPrewarmRows); \
                                auto& imageBufferPrewarmManager = ImageBufferPreparedGet(); \
                                imageBufferPrewarmManager.Activate(imageBufferPrewarmSource.get(), imageBufferPrewarmSignature); \
                                ImageBufferPreparedPrimeAllDense( \
                                    imageBufferPrewarmSelf, imageBufferPrewarmSource, imageBufferPrewarmT, imageBufferPrewarmE, \
                                    imageBufferPrewarmTickMode, imageBufferPrewarmCorrupt, \
                                    imageBufferPrewarmSelf->m_vTrackSettings.size(), imageBufferPrewarmRows, \
                                    imageBufferPrewarmSignature); \
                                UpdateImageBufferPrewarmGpuProgress( \
                                    imageBufferPrewarmSelf, imageBufferPrewarmSelf->m_pRenderer, \
                                    imageBufferPrewarmT, imageBufferPrewarmE, imageBufferPrewarmTickMode); \
                                long long imageBufferPrewarmBakeChunk = Renderer::ImageBufferInvalidChunk; \
                                { \
                                    std::lock_guard<std::mutex> imageBufferPrewarmLock(s_ImageBufferPrewarmGpuMutex); \
                                    if (s_ImageBufferPrewarmGpu.owner == imageBufferPrewarmSelf && \
                                        s_ImageBufferPrewarmGpu.cacheRequired) { \
                                        s_ImageBufferPrewarmGpu.cached = CountImageBufferPrewarmCached( \
                                            imageBufferPrewarmSelf->m_pRenderer, s_ImageBufferPrewarmGpu.chunks); \
                                        for (long long imageBufferPrewarmChunk : s_ImageBufferPrewarmGpu.chunks) { \
                                            if (!imageBufferPrewarmSelf->m_pRenderer->ImageBufferChunkCached(imageBufferPrewarmChunk)) { \
                                                imageBufferPrewarmBakeChunk = imageBufferPrewarmChunk; \
                                                break; \
                                            } \
                                        } \
                                    } \
                                } \
                                if (imageBufferPrewarmBakeChunk != Renderer::ImageBufferInvalidChunk) { \
                                    const ImageBufferPreparedKey imageBufferPrewarmBakeKey{ \
                                        imageBufferPrewarmSource.get(), imageBufferPrewarmBakeChunk, imageBufferPrewarmSignature }; \
                                    if (auto imageBufferPrewarmReady = imageBufferPrewarmManager.Ready(imageBufferPrewarmBakeKey)) { \
                                        imageBufferPrewarmSelf->m_pRenderer->ImageBufferRenderChunk( \
                                            imageBufferPrewarmBakeChunk, imageBufferPrewarmReady->data(), \
                                            (unsigned)imageBufferPrewarmReady->size()); \
                                    } \
                                } \
                            } \
                        } \
                    } \
                } \
            } \
        } \
    } \
}(this))
#define EndText(...) EndText(__VA_ARGS__); DrawImageBufferPrewarmProgressLate(m_pRenderer); DrawFrameTimeGraphLate(m_pRenderer)
// Compile the original game-state implementation under private legacy method
// names. PlaybackAudioThread.inc supplies the public wrappers so visual logic can
// remain frame-driven without letting the frame loop own live synth ingress.
#define Logic LogicLegacy
#define MsgProc MsgProcLegacy
#define Discard DiscardLegacy
#include "GameStateLegacy.inc"
#undef Discard
#undef MsgProc
#undef Logic
#undef EndText
#undef ClearAndBeginScene
#undef CollectChunk

#include "PlaybackAudioThread.inc"

// Frametime is sampled at the same late-render point as the system-stat panel,
// so it measures real presented-frame cadence without perturbing MainScreen's
// playback timer. The graph is visually attached immediately below Sys Stats.
static void DrawFrameTimeGraphLate(Renderer* renderer)
{
    using Clock = std::chrono::steady_clock;
    static Clock::time_point s_last;
    static std::deque<float> s_history;
    static float s_rangeMs = 33.333f;

    const Clock::time_point now = Clock::now();
    float frameMs = 0.0f;
    if (s_last.time_since_epoch().count() != 0)
        frameMs = (float)std::chrono::duration<double, std::milli>(now - s_last).count();
    s_last = now;

    if (!renderer)
        return;

    const VizSettings& viz = Config::GetConfig().GetVizSettings();
    if (!viz.bSysStats || g_bVideoRendering)
        return;

    const MainScreen* statsScreen = dynamic_cast<const MainScreen*>(g_pGameState);
    const float bounceScale = statsScreen ? statsScreen->GetStatsBounceScaleForOverlay() : 1.0f;

    if (frameMs > 0.05f && frameMs < 5000.0f) {
        s_history.push_back(frameMs);
        if (s_history.size() > 600)
            s_history.pop_front();
    }

    ImDrawList* dl = renderer->GetDrawList();
    if (!dl)
        return;
    const int frameTimeVtxStart = dl->VtxBuffer.Size;

    const float scale = (std::max)(viz.fUIScale, 0.5f);
    const float bh = (float)renderer->GetBufferHeight();
    const float graphH = 64.0f * scale;
    const float panelW = 250.0f * scale;
    const float textH = (6.0f + 16.0f * 4.0f) * scale;

    const float contentTop = ImGui::GetFrameHeight() + 35.0f;
    float toolbarBottom = contentTop + 10.0f;
    if (viz.bDualPianoRoll) {
        const float stripH = (std::max)(190.0f,
            (std::min)(bh * 0.45f, bh * 0.28f));
        toolbarBottom = 20.0f + 35.0f + stripH + 10.0f;
    }

    const float sysStatsH = textH + graphH + 10.0f * scale;
    const float panelLeft = 10.0f;
    const float panelTop = toolbarBottom + sysStatsH + 10.0f * scale;
    const float panelRight = panelLeft + panelW;
    const float panelBottom = panelTop + graphH + 26.0f * scale;
    DrawBlurPanel(renderer, panelLeft, panelTop, panelRight, panelBottom, 10.0f * scale);

    const ImVec2 g0(panelLeft + 6.0f * scale, panelTop + 20.0f * scale);
    const ImVec2 g1(panelRight - 6.0f * scale, g0.y + graphH);
    dl->AddRectFilled(g0, g1, 0x30000000);
    for (int i = 0; i <= 4; ++i) {
        const float y = g0.y + (g1.y - g0.y) * (float)i / 4.0f;
        dl->AddLine(ImVec2(g0.x, y), ImVec2(g1.x, y), 0x20FFFFFF);
    }

    float maxSeen = 0.0f;
    for (float ms : s_history)
        maxSeen = (std::max)(maxSeen, ms);
    const float targetRange = (std::max)(33.333f,
        (float)std::ceil((double)maxSeen / 5.0) * 5.0f);
    if (targetRange > s_rangeMs)
        s_rangeMs += (targetRange - s_rangeMs) * 0.35f;
    else
        s_rangeMs += (targetRange - s_rangeMs) * 0.10f;
    s_rangeMs = (std::max)(s_rangeMs, 33.333f);

    auto drawReference = [&](float ms, ImU32 color) {
        if (ms > s_rangeMs)
            return;
        const float y = g1.y - ms / s_rangeMs * (g1.y - g0.y);
        dl->AddLine(ImVec2(g0.x, y), ImVec2(g1.x, y), color);
    };
    drawReference(16.6667f, IM_COL32(255, 255, 255, 60));
    drawReference(33.3333f, IM_COL32(255, 255, 255, 38));

    const size_t n = s_history.size();
    if (n > 0) {
        const float step = (g1.x - g0.x) / 600.0f;
        ImVec2 pts[600];
        for (size_t i = 0; i < n; ++i) {
            const float x = g1.x - (float)(n - i) * step;
            const float ms = (std::min)(s_history[i], s_rangeMs);
            pts[i] = ImVec2(x, g1.y - ms / s_rangeMs * (g1.y - g0.y));
        }
        for (size_t i = 0; i + 1 < n; ++i)
            dl->AddRectFilled(ImVec2(pts[i].x, pts[i].y),
                ImVec2(pts[i + 1].x, g1.y), IM_COL32(255, 255, 255, 36));
        dl->AddRectFilled(ImVec2(pts[n - 1].x, pts[n - 1].y),
            ImVec2(g1.x, g1.y), IM_COL32(255, 255, 255, 36));
        dl->AddPolyline(pts, (int)n, IM_COL32(255, 255, 255, 220), 0, 2.0f * scale);
    }

    const float currentMs = n > 0 ? s_history.back() : 0.0f;
    const float currentFps = currentMs > 0.001f ? 1000.0f / currentMs : 0.0f;
    char label[96];
    snprintf(label, sizeof(label) - 1, "Frame %.1f ms / %.0f FPS", currentMs, currentFps);
    dl->AddText(ImVec2(panelLeft + 6.0f * scale, panelTop + 3.0f * scale),
        0xFF9A9A9A, label);

    char rangeLabel[48];
    snprintf(rangeLabel, sizeof(rangeLabel) - 1, "max %.0f ms", s_rangeMs);
    dl->AddText(ImVec2(g1.x - ImGui::CalcTextSize(rangeLabel).x,
        panelTop + 3.0f * scale), 0xFF9A9A9A, rangeLabel);

    // Match Sys Stats exactly: same yaw, same beat scale, same perspective.
    const int frameTimeVtxEnd = dl->VtxBuffer.Size;
    const ImVec2 frameTimeCenter((panelLeft + panelRight) * 0.5f,
        (panelTop + panelBottom) * 0.5f);
    Apply3DTilt(dl, frameTimeVtxStart, frameTimeVtxEnd, frameTimeCenter,
        0.20f, bounceScale, 750.0f);
}

VOID ImageBufferPrewarmRendererSeen(Renderer* pRenderer)
{
    if (!pRenderer)
        return;
    std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
    s_ImageBufferPrewarmGpu.renderer = pRenderer;
}

VOID ImageBufferPrewarmPlaybackRequested(BOOL bPlaying)
{
    if (!bPlaying) {
        {
            std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
            s_ImageBufferPrewarmGpu.playRequested = false;
            ImageBufferClearPinnedChunks(s_ImageBufferPrewarmGpu.renderer);
        }
        ImageBufferPreparedCancelPlaybackGate();
        return;
    }

    const auto& viz = Config::GetConfig().GetVizSettings();
    if (!viz.bImageBufferNotes || !ImageBufferPreparedGetWaitBeforePlayback() || g_bVideoRendering) {
        {
            std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
            s_ImageBufferPrewarmGpu.playRequested = false;
            ImageBufferClearPinnedChunks(s_ImageBufferPrewarmGpu.renderer);
        }
        ImageBufferPreparedCancelPlaybackGate();
        return;
    }

    const MainScreen* screen = dynamic_cast<const MainScreen*>(g_pGameState);
    const void* owner = nullptr;
    if (screen && !screen->IsFreePlay() && screen->IsValid() && !screen->IsDiscarded())
        owner = screen;

    const ImageBufferPreparedProgress cpu = ImageBufferPreparedGetFullProgress();
    {
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        const bool ownerChanged = s_ImageBufferPrewarmGpu.owner != owner;
        if (ownerChanged) {
            s_ImageBufferPrewarmGpu.owner = owner;
            s_ImageBufferPrewarmGpu.initialized = false;
            s_ImageBufferPrewarmGpu.cacheRequired = false;
            s_ImageBufferPrewarmGpu.cached = 0;
            s_ImageBufferPrewarmGpu.total = 0;
            s_ImageBufferPrewarmGpu.chunks.clear();
        }
        if (ownerChanged || !cpu.initialized)
            s_ImageBufferPrewarmGpu.wakeIssued = false;
        s_ImageBufferPrewarmGpu.playRequested = true;
    }
    ImageBufferPreparedArmPlaybackGate(owner);
}

BOOL ImageBufferPrewarmPlaybackHold()
{
    if (!ImageBufferPreparedGetWaitBeforePlayback()) {
        {
            std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
            s_ImageBufferPrewarmGpu.playRequested = false;
        }
        ImageBufferPreparedCancelPlaybackGate();
        return FALSE;
    }

    const auto& viz = Config::GetConfig().GetVizSettings();
    if (!viz.bImageBufferNotes || g_bVideoRendering) {
        {
            std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
            s_ImageBufferPrewarmGpu.playRequested = false;
        }
        ImageBufferPreparedCancelPlaybackGate();
        return FALSE;
    }

    const MainScreen* screen = dynamic_cast<const MainScreen*>(g_pGameState);
    if (!screen || screen->IsFreePlay() || !screen->IsValid() || screen->IsDiscarded())
        return FALSE;

    Renderer* wakeRenderer = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        if (!s_ImageBufferPrewarmGpu.playRequested)
            return FALSE;
        if (!s_ImageBufferPrewarmGpu.owner)
            s_ImageBufferPrewarmGpu.owner = screen;
        if (s_ImageBufferPrewarmGpu.owner != screen)
            return FALSE;

        // The explicit frame-start kick is primary. Keep this invalidation as a
        // one-shot nudge for already-cached renderers, but prewarm no longer
        // depends on a subsequent visible CollectChunk call.
        if (!s_ImageBufferPrewarmGpu.initialized &&
            !s_ImageBufferPrewarmGpu.wakeIssued &&
            s_ImageBufferPrewarmGpu.renderer) {
            s_ImageBufferPrewarmGpu.wakeIssued = true;
            wakeRenderer = s_ImageBufferPrewarmGpu.renderer;
        }
    }
    if (wakeRenderer) {
        wakeRenderer->ImageBufferInvalidate();
        return TRUE;
    }

    if (ImageBufferPreparedShouldHoldPlayback(screen))
        return TRUE;

    {
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        // CPU preparation may have completed between Logic and the previous
        // render. Hold one more frame until the render path publishes whether a
        // texture-cache stage is required for this song.
        if (!s_ImageBufferPrewarmGpu.initialized)
            return TRUE;
        if (s_ImageBufferPrewarmGpu.cacheRequired) {
            s_ImageBufferPrewarmGpu.cached = CountImageBufferPrewarmCached(
                s_ImageBufferPrewarmGpu.renderer, s_ImageBufferPrewarmGpu.chunks);
            if (s_ImageBufferPrewarmGpu.cached < s_ImageBufferPrewarmGpu.total)
                return TRUE;
        }
        s_ImageBufferPrewarmGpu.playRequested = false;
    }
    return FALSE;
}
