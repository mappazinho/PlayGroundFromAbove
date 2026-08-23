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
    const int imageBufferPrepRows = (int)std::ceil(std::fabs(notesCY)); \
    const bool imageBufferPreparedHandled = !imageBufferAnyHidden && ImageBufferPreparedTryCollect( \
        this, m_pRenderer, m_vEvents, m_MIDI, chunkNotes, (k), \
        kFirst, kLast, kMax, T, E, bTickMode, fCorrupt, \
        m_vTrackSettings.size(), imageBufferPrepRows); \
    if (!imageBufferPreparedHandled) \
        ImageBufferMPCollectDispatch(m_pRenderer, chunkNotes, ImageBufferExactCollector, (k)); \
}())
#include "GameStateLegacy.inc"
#undef CollectChunk
