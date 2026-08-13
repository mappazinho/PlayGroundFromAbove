#pragma once

#include "sdl2/SDL.h"
#include "MIDIAudio.h"

extern MIDIAudio* PRE_MIDIAudio;

// Instantaneous game-loop FPS, published each frame by the game thread and read by the SDL audio callback. Used for the "repeat on player lag" checkbox.
#include <atomic>
extern std::atomic<float> g_fGameFPS;

// Initializes the SDL audio device and the ring buffer that the SDL callback pulls from. Must be called AFTER PRE_MIDIAudio has been created.
extern int PRE_InitAudio();

// Clears the ring buffer and resets the read pointer.
extern void PRE_Reset();

// SDL audio callback.
extern void PRE_FillAudio(void* udata, Uint8* stream, int len);

// Temporary diagnostic: thread-safe append to %TEMP%\pfa_prerender.log
extern void PRE_DbgLog(const char* format, ...);