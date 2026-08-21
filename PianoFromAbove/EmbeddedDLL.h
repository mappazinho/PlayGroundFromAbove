#pragma once

#include <windows.h>

// Extracts embedded DLLs from EXE resources to a temp directory,
// loads them via LoadLibrary, and registers cleanup on exit.
// Returns true on success.
bool InitEmbeddedDLLs(HINSTANCE hInstance);

// Cleans up temp DLL files (called automatically via atexit).
void FreeEmbeddedDLLs();
