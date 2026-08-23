#include <algorithm>
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

// Keep the multipass CPU staging behavior, but replace the legacy collector's
// global-longest-note backscan with the block overlap index. The original local
// CollectChunk lambda remains in GameStateLegacy.inc for source compatibility;
// function-like macro expansion only intercepts its call sites.
#define CollectChunk(k) ([&]() { \
    auto ImageBufferIndexedCollector = [&](long long imageBufferChunk) { \
        ImageBufferOverlapCollect(this, m_vEvents, m_MIDI, chunkNotes, imageBufferChunk, T, E, bTickMode, \
            [&](MIDIChannelEvent imageBufferNote, long long imageBufferChunkStart) { \
                return BuildChunkNoteData(imageBufferNote, imageBufferChunkStart); \
            }); \
    }; \
    ImageBufferMPCollectDispatch(m_pRenderer, chunkNotes, ImageBufferIndexedCollector, (k)); \
}())
#include "GameStateLegacy.inc"
#undef CollectChunk
