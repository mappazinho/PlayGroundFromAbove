/*************************************************************************************************
*
* File: Globals.h
*
* Description: Global variables. Mostly window handlers.
*
* Copyright (c) 2010 Brian Pantano. All rights reserved.
*
*************************************************************************************************/
#pragma once

#include <Windows.h>
#include "Misc.h"

extern HINSTANCE g_hInstance;
extern HWND g_hWnd;
extern HWND g_hWndGfx;
extern bool g_bGfxDestroyed;
extern bool g_bShowLoading;
extern bool g_bInSizeMove;
extern bool g_bResetPending;
extern bool g_bSysResize; // SC_MAXIMIZE/SC_RESTORE in flight (titlebar button transitions)
// Discard the current song (events + SoA pools) BEFORE parsing the next one: the
// old state stays fully resident through the whole load otherwise, and the combined
// peak TDRs the driver on 16GB machines. Runs on the game thread (it owns the state).
class GameState;
extern GameState* g_pGameState; // Current game state; game thread owns it
#define ID_PRELOAD_DISCARD (WM_APP + 0x101)
#define ID_PRELOAD_DRAIN (WM_APP + 0x102) // Drain the GPU queue before a window transition animates
extern bool g_bSkipGPUWait; // Experiment: skip WaitForGPU fence waits entirely (default true)
extern bool g_bDisableBlur; // Experiment: skip the per-frame blur compute passes
extern bool g_bDisableGates; // Experiment: render/present through window transitions like the base
extern bool g_bInRecovery;   // Set while the renderer is being rebuilt after a TDR
extern bool g_bD3D12Available; // Probed at startup: can this machine create a D3D12 device at all?
extern bool g_bBootedFallback; // Set when the requested renderer failed to init and we fell back to D3D11
extern bool g_bForceWARP;     // Retry D3D11 init on the WARP software rasterizer (hardware swapchain failed)
extern const wchar_t* g_pwszRenderMode; // Active backend name ("DirectX 12"/"DirectX 11"), set after init
extern TSQueue< MSG > g_MsgQueue; // Producer/consumer to hold events for our game thread

// Video render: captures playback frames into an FFmpeg pipe, records the
// prerendered audio to WAV, and muxes both into an mp4. RequestVideoRender is
// safe to call from any thread (queues the request to the game thread state).
extern bool g_bVideoRendering;
bool RequestVideoRender();
void StopVideoRender();
bool VideoRenderSongLoaded(); // True when the current state is a loaded song screen

void HeartbeatLog(const char* tag); // Temporary debug diagnostics (game thread only)

// Sets the main window title with the active renderer indicator appended.
inline void SetMainTitle(const wchar_t* title) {
    wchar_t buf[1280];
    _snwprintf_s(buf, 1280, L"%s (Mode: %s)", title, g_pwszRenderMode);
    SetWindowText(g_hWnd, buf);
}

#define ERRORANDRETURN( hwnd, msg, retval ) { MessageBox( ( hwnd ), ( msg ), TEXT( "Error" ), MB_OK | MB_ICONERROR ); return ( retval ); }
