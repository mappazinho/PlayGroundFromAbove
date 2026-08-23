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

static void DrawImageBufferPrewarmProgress(
    Renderer* renderer, float notesX, float notesCX, float keyboardY)
{
    if (!renderer || !ImageBufferPreparedGetWaitBeforePlayback())
        return;

    const ImageBufferPreparedProgress progress = ImageBufferPreparedGetFullProgress();
    if (!progress.initialized || progress.unsupported || progress.total == 0 || progress.done >= progress.total)
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
    const float fraction = (float)progress.done / (float)(std::max)((size_t)1, progress.total);
    auto* draw = renderer->GetDrawList();
    if (!draw)
        return;

    const float rounding = barH * 0.5f;
    draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 180), rounding);
    draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + (x1 - x0) * fraction, y1),
        IM_COL32(235, 235, 235, 230), rounding);

    char text[128];
    if (progress.failed > 0) {
        sprintf_s(text, "Preparing dense image buffers  %zu / %zu  (%zu fallback)",
            progress.done, progress.total, progress.failed);
    } else {
        sprintf_s(text, "Preparing dense image buffers  %zu / %zu",
            progress.done, progress.total);
    }
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float tx = x0 + ((x1 - x0) - textSize.x) * 0.5f;
    const float ty = y0 - textSize.y - 3.0f * scale;
    draw->AddText(ImVec2(tx + 1.0f, ty + 1.0f), IM_COL32(0, 0, 0, 220), text);
    draw->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, 255), text);
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
        DrawImageBufferPrewarmProgress(m_pRenderer, m_fNotesX, m_fNotesCX, notesY + notesCY); \
    if (!imageBufferPreparedHandled) \
        ImageBufferMPCollectDispatch(m_pRenderer, chunkNotes, ImageBufferExactCollector, (k)); \
}())
#include "GameStateLegacy.inc"
#undef CollectChunk

VOID ImageBufferPrewarmPlaybackRequested(BOOL bPlaying)
{
    if (!bPlaying) {
        ImageBufferPreparedCancelPlaybackGate();
        return;
    }

    const auto& viz = Config::GetConfig().GetVizSettings();
    if (!viz.bImageBufferNotes || !ImageBufferPreparedGetWaitBeforePlayback() || g_bVideoRendering) {
        ImageBufferPreparedCancelPlaybackGate();
        return;
    }

    const MainScreen* screen = dynamic_cast<const MainScreen*>(g_pGameState);
    const void* owner = nullptr;
    if (screen && !screen->IsFreePlay() && screen->IsValid() && !screen->IsDiscarded())
        owner = screen;
    ImageBufferPreparedArmPlaybackGate(owner);
}

BOOL ImageBufferPrewarmPlaybackHold()
{
    if (!ImageBufferPreparedGetWaitBeforePlayback()) {
        ImageBufferPreparedCancelPlaybackGate();
        return FALSE;
    }

    const auto& viz = Config::GetConfig().GetVizSettings();
    if (!viz.bImageBufferNotes || g_bVideoRendering) {
        ImageBufferPreparedCancelPlaybackGate();
        return FALSE;
    }

    const MainScreen* screen = dynamic_cast<const MainScreen*>(g_pGameState);
    if (!screen || screen->IsFreePlay() || !screen->IsValid() || screen->IsDiscarded())
        return FALSE;

    return ImageBufferPreparedShouldHoldPlayback(screen) ? TRUE : FALSE;
}
