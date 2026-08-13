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
extern TSQueue< MSG > g_MsgQueue; // Producer/consumer to hold events for our game thread

void HeartbeatLog(const char* tag); // Temporary debug diagnostics (game thread only)

#define ERRORANDRETURN( hwnd, msg, retval ) { MessageBox( ( hwnd ), ( msg ), TEXT( "Error" ), MB_OK | MB_ICONERROR ); return ( retval ); }
