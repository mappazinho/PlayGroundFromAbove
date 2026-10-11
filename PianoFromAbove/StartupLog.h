#pragma once

// Always-on startup trace. Independent of the "Enable logging" toggle: it records the boot sequence
// (embedded DLLs, config, windows, renderer, synth open, first song load) to startup_log.txt next to
// the exe (or %TEMP% when that folder is not writable), then stops once the first song has loaded
// and played for a few seconds. Each line is written straight through with WriteFile, so whatever
// was logged survives a crash. Thread safe; a no-op once the log has ended.

void StartupLog_Begin();
void StartupLog(const char* format, ...);
// For lines mirrored from the chatty diagnostic logs: each distinct message pattern (digits
// ignored) is kept only the first few times, so per-frame tags cannot flood the file.
void StartupLogMirror(const char* text);
void StartupLog_End(const char* reason);
bool StartupLog_IsActive();
const wchar_t* StartupLog_Path(); // Empty until Begin has created the file

// Called by the crash handler: appends a final line without blocking if another thread holds the
// log lock (the crashing thread may be the one holding it).
void StartupLog_CrashNote(const char* note);

// Game thread, once per frame: ends the log a few seconds after the first song state goes live.
void StartupLog_OnFrame(const char* stateName);
