#pragma once

#include <windows.h>
#include <stdint.h>

typedef void* HMEMORYMODULE;

typedef HMEMORYMODULE (*CustomLoadLibraryFunc)(const char* filename, void* userdata);
typedef FARPROC (*CustomGetProcAddressFunc)(HMEMORYMODULE module, const char* name, void* userdata);
typedef void (*CustomFreeLibraryFunc)(HMEMORYMODULE module, void* userdata);

HMEMORYMODULE MemoryLoadLibrary(const void* data, size_t size);

HMEMORYMODULE MemoryLoadLibraryEx(
    const void* data,
    size_t size,
    CustomLoadLibraryFunc loadLibrary,
    CustomGetProcAddressFunc getProcAddress,
    CustomFreeLibraryFunc freeLibrary,
    void* userdata
);

FARPROC MemoryGetProcAddress(HMEMORYMODULE module, const char* name);
FARPROC MemoryGetProcAddressOrdinal(HMEMORYMODULE module, WORD ordinal);

void MemoryFreeLibrary(HMEMORYMODULE module);
