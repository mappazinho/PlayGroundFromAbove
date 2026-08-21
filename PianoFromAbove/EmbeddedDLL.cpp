// EmbeddedDLL.cpp
// Extracts DLLs embedded as RCDATA resources to a temporary directory,
// loads them with LoadLibrary, and hooks the MSVC delay-load mechanism
// so that bass.dll / bassmidi.dll / SDL2.dll resolve to those copies.
// On process exit the temp files are cleaned up automatically.
//
// The copies are loaded under UNIQUE module names (bassdll.dll / bassmd2.dll)
// instead of their original names. The OmniMIDI virtual MIDI driver shares
// the process and loads its own bass.dll/bassmidi.dll from
// %WINDIR%\System32\OmniMIDI; it then removes any module named
// bass.dll/bassmidi.dll from the process, which used to unmap our copies
// while the patched delay-load import table still held pointers into them,
// causing ACCESS_VIOLATION jumps into freed memory. Unique names make our
// copies invisible to that cleanup. bassmidi.dll's import of "bass.dll" is
// rewritten to "bassdll" (same length, in place) so it binds to the renamed
// copy.

#include "EmbeddedDLL.h"
#include "resource.h"
#include <delayimp.h>
#include <string.h>
#include <stdio.h>
#include <shlobj.h>
#include <string>
#include <vector>

#pragma comment(lib, "delayimp.lib")

// --- state -------------------------------------------------------------------
static HMODULE  s_hBass     = NULL;
static HMODULE  s_hBassMidi = NULL;
static HMODULE  s_hSDL2     = NULL;
static bool     s_bInit     = false;

static std::wstring s_tempDir;
static std::wstring s_pathBass;
static std::wstring s_pathBassMidi;
static std::wstring s_pathSDL2;

// --- helpers -----------------------------------------------------------------
static bool ExtractResource(HINSTANCE hInst, int resId, const std::wstring& destPath)
{
    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(resId), (LPCWSTR)RT_RCDATA);
    if (!hRes) hRes = FindResourceW(NULL, MAKEINTRESOURCEW(resId), (LPCWSTR)RT_RCDATA);
    if (!hRes) return false;

    HGLOBAL hGlob = LoadResource(hInst, hRes);
    if (!hGlob) return false;

    DWORD   size = SizeofResource(hInst, hRes);
    const void* data = LockResource(hGlob);
    if (!data || size == 0) return false;

    HANDLE hFile = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL  ok = WriteFile(hFile, data, size, &written, NULL);
    CloseHandle(hFile);
    return ok && (written == size);
}

static DWORD RvaToOffset(const IMAGE_NT_HEADERS* nt, DWORD rva)
{
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if (rva >= sec->VirtualAddress &&
            rva < sec->VirtualAddress + max(sec->SizeOfRawData, sec->Misc.VirtualSize))
            return sec->PointerToRawData + (rva - sec->VirtualAddress);
    }
    return 0;
}

// Rewrites an import-table DLL name string in place (e.g. "bass.dll" ->
// "bassdll"). Only same-length/shrinking rewrites are supported.
static bool PatchImportName(const std::wstring& path, const char* from, const char* to)
{
    if (strlen(to) > strlen(from)) return false;

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD size = GetFileSize(hFile, NULL);
    bool patched = false;
    if (size > sizeof(IMAGE_DOS_HEADER))
    {
        std::vector<char> img(size);
        DWORD read = 0;
        if (ReadFile(hFile, img.data(), size, &read, NULL) && read == size)
        {
            IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)img.data();
            if (dos->e_magic == IMAGE_DOS_SIGNATURE &&
                dos->e_lfanew + sizeof(IMAGE_NT_HEADERS) <= read)
            {
                IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(img.data() + dos->e_lfanew);
                if (nt->Signature == IMAGE_NT_SIGNATURE)
                {
                    DWORD impRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
                    DWORD impSize = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
                    DWORD impOff = impRva ? RvaToOffset(nt, impRva) : 0;
                    if (impOff && impSize && impOff + impSize <= read)
                    {
                        for (DWORD i = 0; i < impSize / sizeof(IMAGE_IMPORT_DESCRIPTOR); ++i)
                        {
                            IMAGE_IMPORT_DESCRIPTOR* d =
                                (IMAGE_IMPORT_DESCRIPTOR*)(img.data() + impOff) + i;
                            if (!d->Name) break;
                            DWORD nameOff = RvaToOffset(nt, d->Name);
                            if (!nameOff || nameOff >= read) continue;
                            char* name = img.data() + nameOff;
                            if (_stricmp(name, from) == 0)
                            {
                                memcpy(name, to, strlen(to) + 1);
                                patched = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
        if (patched)
        {
            SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
            DWORD written = 0;
            WriteFile(hFile, img.data(), size, &written, NULL);
        }
    }
    CloseHandle(hFile);
    return patched;
}

static bool MatchDll(const char* a, const char* b)
{
    return a && b && _stricmp(a, b) == 0;
}

static HMODULE GetEmbeddedHMODULE(const char* name)
{
    if (MatchDll(name, "bass.dll"))     return s_hBass;
    if (MatchDll(name, "bassmidi.dll")) return s_hBassMidi;
    if (MatchDll(name, "SDL2.dll"))     return s_hSDL2;
    return NULL;
}

// --- public API --------------------------------------------------------------

bool InitEmbeddedDLLs(HINSTANCE hInstance)
{
    if (s_bInit) return true;
    if (!hInstance) hInstance = GetModuleHandleW(NULL);

    // Build temp directory path
    wchar_t tmpBase[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpBase);

    // Use a subfolder keyed on the exe path hash so multiple instances don't collide
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    DWORD hash = 0;
    for (const wchar_t* p = exePath; *p; ++p) hash = hash * 31 + *p;

    wchar_t subDir[64];
    _snwprintf_s(subDir, _TRUNCATE, L"PGFA_%08X", hash);

    s_tempDir = std::wstring(tmpBase) + subDir;
    CreateDirectoryW(s_tempDir.c_str(), NULL);

    // Unique module names: OmniMIDI's driver unloads any bass.dll/bassmidi.dll
    // it finds in the process, so our copies must not carry those names.
    s_pathBass     = s_tempDir + L"\\bassdll.dll";
    s_pathBassMidi = s_tempDir + L"\\bassmd2.dll";
    s_pathSDL2     = s_tempDir + L"\\SDL2.dll";

    // Extract all three from resources (files are still written to disk so
    // cleanup is uniform; only the copies we actually load are mapped).
    if (!ExtractResource(hInstance, IDR_DLL_BASS,     s_pathBass) ||
        !ExtractResource(hInstance, IDR_DLL_BASSMIDI, s_pathBassMidi) ||
        !ExtractResource(hInstance, IDR_DLL_SDL2,     s_pathSDL2))
    {
        MessageBoxW(NULL, L"Failed to extract embedded runtime DLLs.",
                    L"PlayGroundFromAbove - Error", MB_OK | MB_ICONERROR);
        return false;
    }

    // Rewrite bassmidi.dll's import of "bass.dll" to the renamed "bassdll"
    // so it binds to our renamed copy. Without this the import would fail
    // to resolve (no module named "bass.dll" exists anymore).
    if (!PatchImportName(s_pathBassMidi, "bass.dll", "bassdll"))
    {
        MessageBoxW(NULL, L"Failed to patch embedded bassmidi.dll imports.",
                    L"PlayGroundFromAbove - Error", MB_OK | MB_ICONERROR);
        return false;
    }

    // Load in dependency order: bass -> bassmidi -> SDL2
    {
        s_hBass = LoadLibraryW(s_pathBass.c_str());
        if (!s_hBass) {
            MessageBoxW(NULL, L"Failed to load embedded bass.dll.",
                        L"PlayGroundFromAbove - Error", MB_OK | MB_ICONERROR);
            return false;
        }

        s_hBassMidi = LoadLibraryW(s_pathBassMidi.c_str());
        if (!s_hBassMidi) {
            MessageBoxW(NULL, L"Failed to load embedded bassmidi.dll.",
                        L"PlayGroundFromAbove - Error", MB_OK | MB_ICONERROR);
            return false;
        }

        // Hold a permanent reference on each embedded runtime DLL. Reloading
        // the same files bumps the refcount without changing the module base;
        // the extra references are intentionally never released. (With the
        // unique module names above nothing can find these copies by name
        // anymore; this is just extra insurance.)
        LoadLibraryW(s_pathBass.c_str());
        LoadLibraryW(s_pathBassMidi.c_str());
    }

    s_hSDL2 = LoadLibraryW(s_pathSDL2.c_str());
    if (!s_hSDL2) {
        MessageBoxW(NULL, L"Failed to load embedded SDL2.dll.",
                    L"PlayGroundFromAbove - Error", MB_OK | MB_ICONERROR);
        return false;
    }

    s_bInit = true;
    return true;
}

void FreeEmbeddedDLLs()
{
    if (s_hSDL2)     { FreeLibrary(s_hSDL2);     s_hSDL2     = NULL; }
    if (s_hBassMidi)  { FreeLibrary(s_hBassMidi);  s_hBassMidi  = NULL; }
    if (s_hBass)      { FreeLibrary(s_hBass);      s_hBass      = NULL; }

    // Clean up temp files
    if (!s_pathSDL2.empty())     DeleteFileW(s_pathSDL2.c_str());
    if (!s_pathBassMidi.empty()) DeleteFileW(s_pathBassMidi.c_str());
    if (!s_pathBass.empty())     DeleteFileW(s_pathBass.c_str());
    if (!s_tempDir.empty())      RemoveDirectoryW(s_tempDir.c_str());

    s_bInit = false;
}

// --- MSVC delay-load hook ----------------------------------------------------
static FARPROC WINAPI EmbeddedDllDelayHook(unsigned dliNotify, PDelayLoadInfo pdli)
{
    switch (dliNotify) {
    case dliNotePreLoadLibrary:
    case dliFailLoadLib: {
        if (!s_bInit) InitEmbeddedDLLs(NULL);
        HMODULE h = GetEmbeddedHMODULE(pdli->szDll);
        if (h) return (FARPROC)h;
        break;
    }
    case dliNotePreGetProcAddress:
    case dliFailGetProc: {
        if (!s_bInit) InitEmbeddedDLLs(NULL);
        HMODULE h = GetEmbeddedHMODULE(pdli->szDll);
        if (h) {
            if (pdli->dlp.fImportByName)
                return GetProcAddress(h, pdli->dlp.szProcName);
            else
                return GetProcAddress(h, MAKEINTRESOURCEA(pdli->dlp.dwOrdinal));
        }
        break;
    }
    default:
        break;
    }
    return NULL;
}

ExternC const PfnDliHook __pfnDliNotifyHook2  = EmbeddedDllDelayHook;
ExternC const PfnDliHook __pfnDliFailureHook2  = EmbeddedDllDelayHook;
