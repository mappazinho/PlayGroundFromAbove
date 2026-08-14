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

// True when the SDL audio callback hasn't fired for >2s (device stalled: the whole
// process keeps running but no sound is produced). Poll from the game thread only.
extern bool PRE_AudioStalled();

// Closes and re-opens the SDL audio device (safe under the WASAPI backend, whose
// audio thread parks on a shutdown event even when the device itself has stalled).
extern void PRE_RestartAudio();

// Temporary diagnostic: thread-safe append to %TEMP%\pfa_prerender.log
extern void PRE_DbgLog(const char* format, ...);