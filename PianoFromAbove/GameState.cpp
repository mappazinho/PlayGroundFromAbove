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

    // The CPU stage must settle first; until then the set of successfully
    // prepared dense chunks is still changing and the GPU stage is premature.
    if (cpu.done < cpu.total)
        return;

    auto& overlap = ImageBufferOverlapIndexGet();
    const auto source = overlap.preparedSource;
    if (overlap.owner != owner || !source || source->notes.empty() || timeSpan <= 0)
        return;

    std::vector<long long> denseChunks;
    const long long firstStart = ImageBufferPreparedStartValue(source->notes.front(), tickMode);
    const long long lastStart = ImageBufferPreparedStartValue(source->notes.back(), tickMode);
    const long long first = ImageBufferPreparedFloorDiv(
        ImageBufferOverlapSaturatingAdd(firstStart, -margin), timeSpan) - 1;
    const long long last = ImageBufferPreparedFloorDiv(
        ImageBufferOverlapSaturatingAdd(lastStart, margin), timeSpan) + 1;

    const bool fitsTextureCache = (last >= first) &&
        (uint64_t)(last - first + 1) <= (uint64_t)Renderer::ChunkPoolSize;

    // If worker preparation failed, retain the exact fallback and do not make
    // playback wait on a potentially huge raw multipass bake. The CPU wait has
    // still done everything it safely can ahead of time.
    const bool requireGpu = fitsTextureCache && cpu.failed == 0;
    if (requireGpu) {
        denseChunks.reserve(cpu.total);
        for (long long chunk = first; chunk <= last; ++chunk) {
            const size_t estimate = ImageBufferPreparedEstimateStarts(
                *source, chunk, timeSpan, margin, tickMode);
            if (estimate >= ImageBufferPreparedDenseThreshold)
                denseChunks.push_back(chunk);
        }
    }

    const size_t cached = requireGpu
        ? CountImageBufferPrewarmCached(renderer, denseChunks)
        : 0;

    std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
    const bool keepPlayRequest = s_ImageBufferPrewarmGpu.playRequested;
    s_ImageBufferPrewarmGpu.owner = owner;
    s_ImageBufferPrewarmGpu.renderer = renderer;
    s_ImageBufferPrewarmGpu.initialized = true;
    s_ImageBufferPrewarmGpu.cacheRequired = requireGpu && !denseChunks.empty();
    s_ImageBufferPrewarmGpu.cached = cached;
    s_ImageBufferPrewarmGpu.total = denseChunks.size();
    s_ImageBufferPrewarmGpu.chunks = std::move(denseChunks);
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

    if (!cpu.initialized) {
        if (!playRequested)
            return;
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
        sprintf_s(text, "Baking dense image buffers  %zu / %zu", done, total);
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

// Dense chunks use a separate CPU preparation path. Sparse chunks keep the
// exact overlap collector and existing multipass behavior. Prepared chunks are
// built from an immutable compact note source on worker threads, rasterized to
// the vertical note resolution, and merged back into a small set of NoteData
// runs before they ever reach the renderer.
#define CollectChunk(k) ([&]() { \
    auto ImageBufferExactCollector = [&](long long imageBufferChunk) { \
        ImageBufferOverlapCollect(this, m_vEvents, m_MIDI, chunkNotes, imageBufferChunk, T, E, bTickMode, \
            [&](MIDIChannelEvent imageBufferNote, long long imageBufferChunkStart) { \
                return BuildChunkNoteData(imageBufferNote, imageBufferChunkStart); \
            }); \
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
    if (imageBufferAnyHidden && (k) == kFirst) \
        ImageBufferPreparedMarkPrewarmUnavailable(this); \
    const int imageBufferPrepRows = (int)std::ceil(std::fabs(notesCY)); \
    const bool imageBufferPreparedHandled = !imageBufferAnyHidden && ImageBufferPreparedTryCollect( \
        this, m_pRenderer, m_vEvents, m_MIDI, chunkNotes, (k), \
        kFirst, kLast, kMax, T, E, bTickMode, fCorrupt, \
        m_vTrackSettings.size(), imageBufferPrepRows); \
    if ((k) == kFirst) \
        UpdateImageBufferPrewarmGpuProgress(this, m_pRenderer, T, E, bTickMode); \
    if (!imageBufferPreparedHandled) \
        ImageBufferMPCollectDispatch(m_pRenderer, chunkNotes, ImageBufferExactCollector, (k)); \
}())
#define EndText(...) EndText(__VA_ARGS__); DrawImageBufferPrewarmProgressLate(m_pRenderer)
#include "GameStateLegacy.inc"
#undef EndText
#undef CollectChunk

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
        }
        ImageBufferPreparedCancelPlaybackGate();
        return;
    }

    const auto& viz = Config::GetConfig().GetVizSettings();
    if (!viz.bImageBufferNotes || !ImageBufferPreparedGetWaitBeforePlayback() || g_bVideoRendering) {
        {
            std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
            s_ImageBufferPrewarmGpu.playRequested = false;
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

        // If the visible/lookahead cache was already complete before the option
        // was enabled, no CollectChunk call would exist to start full-song prep.
        // Force one cache generation restart on the requested Play transition;
        // the next render then enters the normal collector and schedules prep.
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
        // render. Hold one more frame until RenderNotesImageBuffer publishes
        // whether a texture-cache stage is required for this song.
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
