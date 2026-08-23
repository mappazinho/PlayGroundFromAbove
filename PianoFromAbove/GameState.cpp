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

// The lambda itself remains unchanged. Only its calls are routed through the
// multipass CPU staging cache so a dense chunk is scanned/converted once, not
// once per 200k-note GPU slice. The macro's self-reference intentionally names
// the original local lambda (recursive macro expansion is suppressed).
#define CollectChunk(k) ImageBufferMPCollectDispatch(m_pRenderer, chunkNotes, CollectChunk, (k))
#include "GameStateLegacy.inc"
#undef CollectChunk
