/*************************************************************************************************
*
* File: PianoFromAbove.cpp
*
* Description: Main entry point for Piano From Above.
*              Creates windows and enters the GUI and game loops
*
* Copyright (c) 2010 Brian Pantano. All rights reserved.
*
*************************************************************************************************/
#include <Windows.h>
#include <CommCtrl.h>
#include <Shlobj.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <wincodec.h>
#pragma comment(lib, "dbghelp.lib")
#include <ctime>
#include <shlwapi.h>
#include <winhttp.h>
#include <regex>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <vector>

#include "MainProcs.h"
#include "Globals.h"
#include "resource.h"

#include "Config.h"
#include "GameState.h"
#include "Renderer.h"
#include "Misc.h"
#include "MIDIPreRenderPlayer.h"
#include "EmbeddedDLL.h"

// Yes, I know you shouldn't store build numbers as doubles
constexpr double BUILD_VERSION = 20240112;

INT WINAPI WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpszCmdLine, INT nCmdShow );
DWORD WINAPI GameThread( LPVOID lpParameter );

// Global variables
HINSTANCE g_hInstance = NULL;
HWND g_hWnd = NULL;
HWND g_hWndGfx = NULL;
bool g_bGfxDestroyed = false;
GameState* g_pGameState = nullptr;

// The UI now ships one small PNG rather than a legacy multi-image .ico.  WIC
// turns the embedded PNG into the window icon at startup, so the executable
// and taskbar use the same source asset as the ImGui-focused UI.
static HICON LoadApplicationIconFromPNG(HINSTANCE hInstance)
{
    HRSRC resource = FindResourceW(hInstance, MAKEINTRESOURCEW(IDR_APPICON_PNG), RT_RCDATA);
    if (!resource)
        return NULL;
    HGLOBAL resourceData = LoadResource(hInstance, resource);
    const DWORD resourceSize = SizeofResource(hInstance, resource);
    const BYTE* pngData = static_cast<const BYTE*>(LockResource(resourceData));
    if (!pngData || resourceSize == 0)
        return NULL;

    IStream* stream = SHCreateMemStream(pngData, resourceSize);
    if (!stream)
        return NULL;

    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    HICON icon = NULL;

    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream, NULL, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                        WICBitmapDitherTypeNone, NULL, 0.0,
                                        WICBitmapPaletteTypeCustom))) {
        UINT width = 0, height = 0;
        if (SUCCEEDED(converter->GetSize(&width, &height)) && width > 0 && height > 0) {
            std::vector<BYTE> pixels((size_t)width * height * 4);
            if (SUCCEEDED(converter->CopyPixels(NULL, width * 4, (UINT)pixels.size(), pixels.data()))) {
                BITMAPV5HEADER bitmap = {};
                bitmap.bV5Size = sizeof(bitmap);
                bitmap.bV5Width = (LONG)width;
                bitmap.bV5Height = -(LONG)height;
                bitmap.bV5Planes = 1;
                bitmap.bV5BitCount = 32;
                bitmap.bV5Compression = BI_BITFIELDS;
                bitmap.bV5RedMask = 0x00FF0000;
                bitmap.bV5GreenMask = 0x0000FF00;
                bitmap.bV5BlueMask = 0x000000FF;
                bitmap.bV5AlphaMask = 0xFF000000;
                void* iconPixels = nullptr;
                HBITMAP color = CreateDIBSection(NULL, reinterpret_cast<BITMAPINFO*>(&bitmap),
                                                  DIB_RGB_COLORS, &iconPixels, NULL, 0);
                HBITMAP mask = CreateBitmap((int)width, (int)height, 1, 1, NULL);
                if (color && mask && iconPixels) {
                    memcpy(iconPixels, pixels.data(), pixels.size());
                    ICONINFO iconInfo = {};
                    iconInfo.fIcon = TRUE;
                    iconInfo.hbmColor = color;
                    iconInfo.hbmMask = mask;
                    icon = CreateIconIndirect(&iconInfo);
                }
                if (color) DeleteObject(color);
                if (mask) DeleteObject(mask);
            }
        }
    }

    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    stream->Release();
    return icon;
}

void HeartbeatLog(const char* tag) {
    StartupLogMirror(tag);
    if (!g_bLoggingEnabled.load(std::memory_order_relaxed))
        return;
    static bool bInit = false;
    static __int64 nStart = 0;
    static int nLoopCount = 0;
    if (!bInit) {
        bInit = true;
        nStart = GetTickCount64();
    }
    if (strcmp(tag, "loop") == 0 && (nLoopCount++ & 15) != 0)
        return;
    __int64 ms = GetTickCount64() - nStart;
    wchar_t path[MAX_PATH] = {};
    GetTempPathW(_countof(path), path);
    wcscat_s(path, L"pfa_heartbeat.log");
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"a");
    if (f) {
        fprintf(f, "[%6lld.%03d] %s\n", (long long)(ms / 1000), (int)(ms % 1000), tag);
        fclose(f);
    }
}

static const wchar_t* ExceptionCodeString(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:         return L"ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return L"ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return L"DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return L"FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_OVERFLOW:             return L"FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:          return L"FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:            return L"FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return L"ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:            return L"IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return L"INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:             return L"INT_OVERFLOW";
    case EXCEPTION_PRIV_INSTRUCTION:         return L"PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:           return L"STACK_OVERFLOW";
    case EXCEPTION_BREAKPOINT:               return L"BREAKPOINT";
    default:                                return L"UNKNOWN";
    }
}

static const wchar_t kIssuesUrl[] = L"https://github.com/mappazinho/PlayGroundFromAbove/issues";

// ntdll keeps a ring buffer of the last 64 unloaded DLLs. A crash at an address that no loaded
// module covers is usually a call into (or a vtable read from) one of these.
struct PGFAUnloadEvent {
    PVOID BaseAddress;
    SIZE_T SizeOfImage;
    ULONG Sequence;
    ULONG TimeDateStamp;
    ULONG CheckSum;
    WCHAR ImageName[32];
    ULONG Version[2];
};
typedef VOID(NTAPI* RtlGetUnloadEventTraceEx_t)(PULONG* ElementSize, PULONG* ElementCount, PVOID* EventTrace);

static RtlGetUnloadEventTraceEx_t s_pGetUnloadTrace = (RtlGetUnloadEventTraceEx_t)
    GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetUnloadEventTraceEx");

static const PGFAUnloadEvent* UnloadEventAt(ULONG index, ULONG* pCount) {
    *pCount = 0;
    if (!s_pGetUnloadTrace)
        return nullptr;
    __try {
        PULONG pSize = nullptr, pNum = nullptr;
        PVOID* pTrace = nullptr;
        s_pGetUnloadTrace(&pSize, &pNum, (PVOID*)&pTrace);
        if (!pSize || !pNum || !pTrace || !*pTrace || *pSize < sizeof(PGFAUnloadEvent))
            return nullptr;
        *pCount = *pNum;
        if (index >= *pNum)
            return nullptr;
        const PGFAUnloadEvent* ev = (const PGFAUnloadEvent*)((const BYTE*)*pTrace + (SIZE_T)index * *pSize);
        return ev->BaseAddress ? ev : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *pCount = 0;
        return nullptr;
    }
}

// "Module+0xOff", "UNLOADED Module+0xOff", or "" when the address is in no image.
static void DescribeAddress(DWORD64 addr, wchar_t* out, size_t cch) {
    out[0] = L'\0';
    if (addr < 0x10000)
        return;
    HMODULE hMod = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &hMod)) {
        wchar_t path[MAX_PATH] = L"";
        GetModuleFileNameW(hMod, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        _snwprintf_s(out, cch, _TRUNCATE, L"%s+0x%llX", name ? name + 1 : path,
                     addr - (DWORD64)(DWORD_PTR)hMod);
        return;
    }
    ULONG count = 0;
    UnloadEventAt(0, &count);
    for (ULONG i = 0; i < count; i++) {
        const PGFAUnloadEvent* ev = UnloadEventAt(i, &count);
        if (!ev)
            continue;
        const DWORD64 base = (DWORD64)(DWORD_PTR)ev->BaseAddress;
        if (addr >= base && addr < base + ev->SizeOfImage) {
            wchar_t name[33] = L"";
            wmemcpy(name, ev->ImageName, 32);
            _snwprintf_s(out, cch, _TRUNCATE, L"UNLOADED %s+0x%llX", name, addr - base);
            return;
        }
    }
}

// Walks the faulting thread from its exception context with the x64 unwind tables. StackWalk64
// without a function-table callback cannot unwind x64 frames and printed raw stack data instead.
static int UnwindFromContext(const CONTEXT* start, DWORD64* frames, int maxFrames,
                             ULONG_PTR stackLow = 0, ULONG_PTR stackHigh = 0) {
    CONTEXT c = *start;
    if (!stackHigh) // default: the calling thread's own stack
        GetCurrentThreadStackLimits(&stackLow, &stackHigh);
    int n = 0;
    __try {
        while (n < maxFrames && c.Rip) {
            frames[n++] = c.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &imageBase, NULL);
            if (fn) {
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, fn, &c, &handlerData, &establisher, NULL);
            } else {
                // Leaf function, or code with no unwind info (e.g. an unloaded module): the return
                // address is on top of the stack.
                if (c.Rsp < stackLow || c.Rsp + 8 > stackHigh)
                    break;
                c.Rip = *(DWORD64*)c.Rsp;
                c.Rsp += 8;
            }
            if (c.Rsp < stackLow || c.Rsp >= stackHigh)
                break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// crash_log.txt next to the exe, or in %TEMP% when the exe folder is not writable.
static bool OpenCrashLogFile(FILE** pf, wchar_t* path, size_t cch) {
    *pf = nullptr;
    if (GetModuleFileNameW(NULL, path, (DWORD)cch)) {
        if (wchar_t* slash = wcsrchr(path, L'\\')) {
            wcscpy_s(slash + 1, cch - (slash + 1 - path), L"crash_log.txt");
            if (_wfopen_s(pf, path, L"wb, ccs=UTF-16LE") == 0 && *pf)
                return true;
        }
    }
    if (GetTempPathW((DWORD)cch, path)) {
        wcscat_s(path, cch, L"PGFA_crash_log.txt");
        if (_wfopen_s(pf, path, L"wb, ccs=UTF-16LE") == 0 && *pf)
            return true;
    }
    path[0] = L'\0';
    return false;
}

#define CRASH_APPEND(buf, len, ...) \
    do { if ((len) < _countof(buf)) { int _n = _snwprintf_s((buf) + (len), _countof(buf) - (len), _TRUNCATE, __VA_ARGS__); \
         (len) = _n < 0 ? _countof(buf) - 1 : (len) + (size_t)_n; } } while (0)

// What "Try to continue" can do on the crashing thread.
enum class CrashContinue {
    None,        // UI thread, or no safe way out: Quit only
    GameThread,  // Unwind to GameThread's __except and restart it on the start screen
    OtherThread, // End only the crashing thread
};

static HRESULT CALLBACK CrashDialogCallback(HWND hwnd, UINT msg, WPARAM, LPARAM lParam, LONG_PTR) {
    if (msg == TDN_CREATED) {
        // Task dialogs have no topmost flag; without this a fullscreen window can hide it.
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(hwnd);
    } else if (msg == TDN_HYPERLINK_CLICKED) {
        ShellExecuteW(hwnd, L"open", (LPCWSTR)lParam, NULL, NULL, SW_SHOWNORMAL);
    }
    return S_OK;
}

// Shows the crash summary with a "Quit" button and, when bCanContinue, a "Try to continue" button.
// brief holds the summary with the issues link to be inserted at headLen. Returns true to continue.
static bool ShowCrashDialog(const wchar_t* brief, size_t headLen, bool bCanContinue, wchar_t* content, size_t cch) {
    typedef HRESULT(WINAPI* TaskDialogIndirect_t)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    HMODULE hComctl = GetModuleHandleW(L"comctl32.dll");
    if (!hComctl)
        hComctl = LoadLibraryW(L"comctl32.dll");
    // Only comctl32 v6 (the exe's manifest asks for it) exports TaskDialogIndirect.
    auto pTaskDialogIndirect = hComctl ? (TaskDialogIndirect_t)GetProcAddress(hComctl, "TaskDialogIndirect") : nullptr;

    enum { kQuit = 100, kContinue = 101 };
    if (pTaskDialogIndirect) {
        _snwprintf_s(content, cch, _TRUNCATE, L"%.*s<a href=\"%s\">%s</a>%s",
                     (int)headLen, brief, kIssuesUrl, kIssuesUrl, brief + headLen);
        const TASKDIALOG_BUTTON buttons[] = { { kQuit, L"Quit" }, { kContinue, L"Try to continue" } };
        TASKDIALOGCONFIG cfg = {};
        cfg.cbSize = sizeof(cfg);
        cfg.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
        cfg.pszWindowTitle = L"PlayGroundFromAbove - Crash";
        cfg.pszMainIcon = TD_ERROR_ICON;
        cfg.pszMainInstruction = L"PlayGroundFromAbove has crashed.";
        cfg.pszContent = content;
        cfg.cButtons = bCanContinue ? 2 : 1;
        cfg.pButtons = buttons;
        cfg.nDefaultButton = kQuit;
        cfg.pfCallback = CrashDialogCallback;
        int button = 0;
        if (SUCCEEDED(pTaskDialogIndirect(&cfg, &button, NULL, NULL)))
            return bCanContinue && button == kContinue; // Closing the dialog (IDCANCEL) quits
    }

    _snwprintf_s(content, cch, _TRUNCATE, L"PlayGroundFromAbove has crashed.\n\n%.*s%s%s%s",
                 (int)headLen, brief, kIssuesUrl, brief + headLen,
                 bCanContinue ? L"\n\nQuit PlayGroundFromAbove? (No = Try to continue)" : L"");
    const int result = MessageBoxW(NULL, content, L"PlayGroundFromAbove - Crash",
        MB_ICONERROR | (bCanContinue ? MB_YESNO : MB_OK) | MB_TOPMOST | MB_SETFOREGROUND);
    return bCanContinue && result == IDNO;
}

// The full report (registers, both stacks, unloaded and loaded modules) goes to crash_log.txt; the
// dialog only shows the summary, the top frames, where the files are and where to report.
// Returns true when the user chose "Try to continue" (only offered when mode allows it).
static bool WriteCrashLog(EXCEPTION_POINTERS* ep, CrashContinue mode) {
    // One report at a time; a second thread crashing meanwhile waits for the first to finish.
    static volatile LONG s_bBusy = 0;
    if (InterlockedExchange(&s_bBusy, 1)) {
        Sleep(INFINITE);
        return false;
    }
    // Static so a stack overflow crash still has room to format the report.
    static wchar_t full[65536];
    static wchar_t brief[4096];
    size_t fl = 0, bl = 0;
    full[0] = brief[0] = L'\0';

    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    CONTEXT* ctx = ep->ContextRecord;
    wchar_t where[160], tmp[160];
    DescribeAddress(ctx->Rip, where, _countof(where));
    if (!where[0])
        wcscpy_s(where, L"<unknown module>");
    const bool bGameThread = GetCurrentThreadId() == g_dwGameThreadId;

    CRASH_APPEND(full, fl, L"PlayGroundFromAbove crash report\n\n");
    CRASH_APPEND(full, fl, L"Exception:  0x%08X (%s)\nAddress:    0x%016llX  %s\nThread:     %lu (%s)\n",
                 er->ExceptionCode, ExceptionCodeString(er->ExceptionCode), (unsigned long long)ctx->Rip,
                 where, GetCurrentThreadId(), bGameThread ? L"game thread" : L"not the game thread");
    CRASH_APPEND(brief, bl, L"%s (0x%08X) in %s\nThread %lu (%s)\n",
                 ExceptionCodeString(er->ExceptionCode), er->ExceptionCode, where, GetCurrentThreadId(),
                 bGameThread ? L"game thread" : L"not the game thread");
    if ((er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        er->NumberParameters >= 2) {
        const ULONG_PTR op = er->ExceptionInformation[0];
        const wchar_t* opName = op == 0 ? L"read of" : op == 1 ? L"write to" : op == 8 ? L"execute at" : L"access to";
        DescribeAddress(er->ExceptionInformation[1], tmp, _countof(tmp));
        CRASH_APPEND(full, fl, L"Fault:      %s 0x%016llX%s%s\n", opName,
                     (unsigned long long)er->ExceptionInformation[1], tmp[0] ? L"  " : L"", tmp);
        CRASH_APPEND(brief, bl, L"Bad %s 0x%llX%s%s\n", opName,
                     (unsigned long long)er->ExceptionInformation[1], tmp[0] ? L"  " : L"", tmp);
    }

    CRASH_APPEND(full, fl, L"\nRegisters:\n");
    const struct { const wchar_t* name; DWORD64 value; } regs[] = {
        { L"RAX", ctx->Rax }, { L"RBX", ctx->Rbx }, { L"RCX", ctx->Rcx }, { L"RDX", ctx->Rdx },
        { L"RSI", ctx->Rsi }, { L"RDI", ctx->Rdi }, { L"RBP", ctx->Rbp }, { L"RSP", ctx->Rsp },
        { L"R8 ", ctx->R8 },  { L"R9 ", ctx->R9 },  { L"R10", ctx->R10 }, { L"R11", ctx->R11 },
        { L"R12", ctx->R12 }, { L"R13", ctx->R13 }, { L"R14", ctx->R14 }, { L"R15", ctx->R15 },
        { L"RIP", ctx->Rip },
    };
    for (const auto& r : regs) {
        DescribeAddress(r.value, tmp, _countof(tmp));
        CRASH_APPEND(full, fl, L"  %s=%016llX  %s\n", r.name, (unsigned long long)r.value, tmp);
    }
    CRASH_APPEND(full, fl, L"  EFLAGS=%08X\n", ctx->EFlags);

    DWORD64 frames[48];
    const int nFrames = UnwindFromContext(ctx, frames, _countof(frames));
    CRASH_APPEND(full, fl, L"\nCrashing thread stack:\n");
    CRASH_APPEND(brief, bl, L"\nCrashing thread stack:\n");
    for (int i = 0; i < nFrames; i++) {
        DescribeAddress(frames[i], tmp, _countof(tmp));
        CRASH_APPEND(full, fl, L"  0x%016llX  %s\n", (unsigned long long)frames[i], tmp[0] ? tmp : L"<unknown module>");
        if (i < 8)
            CRASH_APPEND(brief, bl, L"  %s\n", tmp[0] ? tmp : L"<unknown module>");
    }
    if (nFrames > 8)
        CRASH_APPEND(brief, bl, L"  ... %d more in the log\n", nFrames - 8);

    {
        PVOID handlerFrames[64] = {};
        const USHORT n = RtlCaptureStackBackTrace(0, 64, handlerFrames, NULL);
        CRASH_APPEND(full, fl, L"\nHandler stack (includes the exception dispatch):\n");
        for (USHORT i = 0; i < n; i++) {
            DescribeAddress((DWORD64)(DWORD_PTR)handlerFrames[i], tmp, _countof(tmp));
            CRASH_APPEND(full, fl, L"  0x%016llX  %s\n", (unsigned long long)(DWORD_PTR)handlerFrames[i],
                         tmp[0] ? tmp : L"<unknown module>");
        }
    }

    {
        ULONG count = 0;
        UnloadEventAt(0, &count);
        CRASH_APPEND(full, fl, L"\nRecently unloaded modules:\n");
        bool any = false;
        for (ULONG i = 0; i < count; i++) {
            const PGFAUnloadEvent* ev = UnloadEventAt(i, &count);
            if (!ev)
                continue;
            wchar_t name[33] = L"";
            wmemcpy(name, ev->ImageName, 32);
            CRASH_APPEND(full, fl, L"  0x%016llX-0x%016llX  %s (seq %lu)\n",
                         (unsigned long long)(DWORD_PTR)ev->BaseAddress,
                         (unsigned long long)(DWORD_PTR)ev->BaseAddress + ev->SizeOfImage, name, ev->Sequence);
            any = true;
        }
        if (!any)
            CRASH_APPEND(full, fl, L"  (none)\n");
    }

    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery((LPCVOID)ctx->Rip, &mbi, sizeof(mbi)))
            CRASH_APPEND(full, fl, L"\nRIP region: Base=0x%016llX Size=0x%llX State=0x%X Protect=0x%X Type=0x%X\n",
                         (unsigned long long)(DWORD_PTR)mbi.BaseAddress, (unsigned long long)mbi.RegionSize,
                         (unsigned)mbi.State, (unsigned)mbi.Protect, (unsigned)mbi.Type);
        static HMODULE hMods[512];
        DWORD cbNeeded = 0;
        CRASH_APPEND(full, fl, L"\nLoaded modules:\n");
        if (EnumProcessModules(GetCurrentProcess(), hMods, sizeof(hMods), &cbNeeded)) {
            DWORD n = cbNeeded / sizeof(HMODULE);
            if (n > _countof(hMods)) n = _countof(hMods);
            for (DWORD i = 0; i < n; i++) {
                wchar_t mpath[MAX_PATH] = L"";
                GetModuleFileNameW(hMods[i], mpath, MAX_PATH);
                MODULEINFO mi = {};
                GetModuleInformation(GetCurrentProcess(), hMods[i], &mi, sizeof(mi));
                CRASH_APPEND(full, fl, L"  0x%016llX-0x%016llX  %ls\n", (unsigned long long)(DWORD_PTR)hMods[i],
                             (unsigned long long)(DWORD_PTR)hMods[i] + mi.SizeOfImage, mpath);
            }
        }
    }

    const bool bLogging = g_bLoggingEnabled.load(std::memory_order_relaxed);
    CRASH_APPEND(full, fl, L"\nLogging enabled: %s\nStartup log: %s\n", bLogging ? L"yes" : L"no",
                 StartupLog_Path()[0] ? StartupLog_Path() : L"(not created)");

    wchar_t logPath[MAX_PATH] = L"";
    FILE* f = nullptr;
    if (OpenCrashLogFile(&f, logPath, _countof(logPath))) {
        fwprintf(f, L"%s\n", full);
        fclose(f);
    }
    {
        char note[256];
        _snprintf_s(note, _countof(note), _TRUNCATE, "CRASH: %ls 0x%08X in %ls (full report in crash_log.txt)",
                    ExceptionCodeString(er->ExceptionCode), er->ExceptionCode, where);
        StartupLog_CrashNote(note);
    }

    CRASH_APPEND(brief, bl, L"\nFull report: %s\n", logPath[0] ? logPath : L"(could not be written)");
    if (StartupLog_Path()[0])
        CRASH_APPEND(brief, bl, L"Startup log: %s\n", StartupLog_Path());
    // The report link is spliced in between briefHead and briefTail: a hyperlink in the task
    // dialog, plain text in the MessageBox fallback.
    CRASH_APPEND(brief, bl, L"\nPlease report this at:\n");
    const size_t briefHeadLen = bl;
    CRASH_APPEND(brief, bl, L"\nand attach crash_log.txt%s.\n", StartupLog_Path()[0] ? L" and startup_log.txt" : L"");
    if (!bLogging)
        CRASH_APPEND(brief, bl, L"\nLogging is turned off. If you can, turn on Preferences > Viz > \"Enable logging\", "
                                L"make it crash again, and also attach pfa_heartbeat.log and pfa_prerender.log "
                                L"from your %%TEMP%% folder.\n");
    if (mode == CrashContinue::GameThread)
        CRASH_APPEND(brief, bl, L"\n\"Try to continue\" closes the current song and goes back to the start screen. "
                                L"PGFA may be unstable afterwards.\n");
    else if (mode == CrashContinue::OtherThread)
        CRASH_APPEND(brief, bl, L"\n\"Try to continue\" stops only the part that crashed. Audio, loading or other "
                                L"features may stop working until PGFA is restarted.\n");
    CRASH_APPEND(brief, bl, L"\nCtrl+C copies this message.");

    static wchar_t content[4096 + 256];
    content[0] = L'\0';
    const bool bContinue = ShowCrashDialog(brief, briefHeadLen, mode != CrashContinue::None, content, _countof(content));
    StartupLog_CrashNote(bContinue ? "CRASH: user chose Try to continue" : "CRASH: user chose Quit");
    InterlockedExchange(&s_bBusy, 0);
    return bContinue;
}

DWORD g_dwGameThreadId = 0;

// Last-resort backstop: some fatal traps (breakpoints from corrupted indirect
// calls, illegal instructions raised inside static CRT helpers) were slipping
// past the unhandled-exception filter because other components re-register or
// clear it. A second-chance VEH registered LAST runs before any of that and
// guarantees we capture registers + stack to crash_log.txt.
// Set once the game thread is gone and the config is saved. From then on an access violation can
// only come from a foreign thread running in a DLL that is being unloaded; there is nothing left
// to protect, so exit quietly instead of writing a crash log for a clean close.
static volatile LONG g_bTeardown = 0;

static DWORD g_dwMainThreadId = 0;
static volatile LONG g_bGameCrashReported = 0;

// A crash on any thread but the game thread. The UI thread cannot be continued (its message loop
// is the app); any other thread is ended on "Try to continue" and the rest of the app keeps going.
[[noreturn]] static void HandleForeignThreadCrash(EXCEPTION_POINTERS* ep) {
    const bool bCanContinue = GetCurrentThreadId() != g_dwMainThreadId;
    if (!WriteCrashLog(ep, bCanContinue ? CrashContinue::OtherThread : CrashContinue::None))
        TerminateProcess(GetCurrentProcess(), 1);
    ExitThread(0xDEAD);
}

static LONG WINAPI FinalCrashBackstop(EXCEPTION_POINTERS* ep) {
    switch (ep->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case STATUS_BREAKPOINT:            // 0x80000003: int3 traps from corrupted calls
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH; // C++ exceptions (0xE06D7363) etc. pass through
    }
    if (g_bTeardown)
        TerminateProcess(GetCurrentProcess(), 0);
    if (GetCurrentThreadId() == g_dwGameThreadId) {
        if (!WriteCrashLog(ep, CrashContinue::GameThread))
            TerminateProcess(GetCurrentProcess(), 1);
        // Let the exception reach GameThreadBody's __except, which restarts the game loop. The
        // report is already written, so that handler must not show a second dialog.
        InterlockedExchange(&g_bGameCrashReported, 1);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    HandleForeignThreadCrash(ep);
}

static LONG WINAPI VectoredCrashHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW &&
        ep->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION &&
        ep->ExceptionRecord->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        ep->ExceptionRecord->ExceptionCode != EXCEPTION_IN_PAGE_ERROR &&
        ep->ExceptionRecord->ExceptionCode != EXCEPTION_DATATYPE_MISALIGNMENT)
        return EXCEPTION_CONTINUE_SEARCH;

    if (g_bTeardown)
        TerminateProcess(GetCurrentProcess(), 0);

    if (GetCurrentThreadId() == g_dwGameThreadId)
        return EXCEPTION_CONTINUE_SEARCH;

    HandleForeignThreadCrash(ep);
}

static void __cdecl TerminateHandler() {
    wchar_t msg[1024];
    _snwprintf_s(msg, _countof(msg), _TRUNCATE,
        L"PlayGroundFromAbove has encountered a fatal error.\n\n"
        L"std::terminate() was called (unhandled C++ exception, pure virtual call, etc.).\n"
        L"The application will now exit.\n\n"
        L"Please report this at:\n%s\nand attach crash_log.txt%s.",
        kIssuesUrl, StartupLog_Path()[0] ? L" and startup_log.txt" : L"");
    StartupLog_CrashNote("CRASH: std::terminate() called");
    FILE* f = NULL;
    wchar_t logPath[MAX_PATH];
    if (OpenCrashLogFile(&f, logPath, _countof(logPath))) {
        fwprintf(f, L"%s\n", msg);
        fclose(f);
    }
    MessageBoxW(NULL, msg, L"Oh deer..", MB_ICONERROR | MB_OK | MB_TOPMOST | MB_SETFOREGROUND);
    TerminateProcess(GetCurrentProcess(), 1);
}

TSQueue< MSG > g_MsgQueue; // Producer/consumer to hold events for our game thread

DWORD WINAPI UpdateCheckProc(LPVOID) {
    HINTERNET session = WinHttpOpen(L"pfavizkhang", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return 0;

    HINTERNET connect = WinHttpConnect(session, L"api.github.com",
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return 0;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", L"/repos/khang06/PianoFromAbove/releases",
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return 0;
    }

    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, NULL) &&
        WinHttpReceiveResponse(request, NULL))
    {
        std::string total;
        DWORD expected_len = 0;
        DWORD recv_len = 0;
        do {
            if (!WinHttpQueryDataAvailable(request, &expected_len))
                break;

            auto old_len = total.size();
            total.resize(old_len + expected_len);
            if (!WinHttpReadData(request, total.data() + old_len, expected_len, &recv_len))
                break;
        } while (expected_len != 0 && recv_len != 0);

        std::regex regex("\"tag_name\":\\s*\"([0-9.]+)\",");
        std::smatch matches;
        
        if (std::regex_search(total, matches, regex)) {
            auto old_locale = std::setlocale(LC_NUMERIC, nullptr);
            std::setlocale(LC_NUMERIC, "C");
            for (size_t i = 1; i < matches.size(); i++) {
                double parsed = 0.0;
                try {
                    parsed = std::stod(matches[i].str());
                } catch (...) {
                    continue;
                }
                if (parsed > BUILD_VERSION) {
                    wchar_t title[1024];
                    GetWindowText(g_hWnd, title, 1024);
                    wchar_t newTitle[1100];
                    _snwprintf_s(newTitle, 1100, L"Update available! — %s", title);
                    SetWindowText(g_hWnd, newTitle);
                    break;
                }
            }
            std::setlocale(LC_NUMERIC, old_locale);
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    return 0;
}

// ---- Shutdown watchdog ------------------------------------------------------------------------
// Closing PGFA can hang (a worker thread that never joins, a device that never closes, a static
// destructor, ...) and leave a windowless process behind. Shutdown steps record where they are; once
// closing begins a watchdog gives the process kShutdownGraceMs to exit, then reports the stuck step and
// every thread's stack, and offers to terminate the process.
static const DWORD kShutdownGraceMs = 10000;
// Like CRASH_APPEND, for a buffer reached through a pointer.
#define CRASH_APPEND_W(buf, cch, len, ...) \
    do { if ((len) < (cch)) { int _n = _snwprintf_s((buf) + (len), (cch) - (len), _TRUNCATE, __VA_ARGS__); \
         (len) = _n < 0 ? (cch) - 1 : (len) + (size_t)_n; } } while (0)
static const char* volatile g_pShutdownStage = "";
static volatile LONG g_bShutdownWatchdog = 0;

void ShutdownStage(const char* stage) {
    g_pShutdownStage = stage;
    HeartbeatLog(stage);
}

struct HangThread {
    DWORD tid = 0;
    DWORD64 frames[24] = {};
    int count = 0;
};

// Suspends each thread just long enough to capture its raw return addresses (no allocation or
// formatting while a thread that may hold the heap lock is stopped).
static int CaptureAllThreads(HangThread* out, int maxThreads) {
    typedef LONG (NTAPI *NtQueryInformationThreadFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    struct ThreadBasicInfo { LONG status; PVOID teb; PVOID pid; PVOID tid; ULONG_PTR affinity; LONG prio; LONG basePrio; };
    static NtQueryInformationThreadFn NtQueryInformationThread =
        (NtQueryInformationThreadFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");

    int n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || n >= maxThreads)
                continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h)
                continue;
            HangThread& t = out[n];
            t.tid = te.th32ThreadID;
            if (SuspendThread(h) != (DWORD)-1) {
                CONTEXT ctx = {};
                ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                if (GetThreadContext(h, &ctx)) {
                    ULONG_PTR high = 0;
                    ThreadBasicInfo tbi = {};
                    if (NtQueryInformationThread && NtQueryInformationThread(h, 0, &tbi, sizeof(tbi), NULL) >= 0 && tbi.teb)
                        high = (ULONG_PTR)((NT_TIB*)tbi.teb)->StackBase;
                    if (high)
                        t.count = UnwindFromContext(&ctx, t.frames, _countof(t.frames), 0, high);
                }
                ResumeThread(h);
            }
            CloseHandle(h);
            n++;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return n;
}

static void FormatHangThread(const HangThread& t, int maxFrames, wchar_t* buf, size_t cch, size_t& len) {
    const wchar_t* role = t.tid == g_dwMainThreadId ? L" (UI thread)" : t.tid == g_dwGameThreadId ? L" (game thread)" : L"";
    CRASH_APPEND_W(buf, cch, len, L"  thread %lu%s:\r\n", t.tid, role);
    for (int i = 0; i < t.count && i < maxFrames; i++) {
        wchar_t d[300];
        DescribeAddress(t.frames[i], d, _countof(d));
        if (d[0])
            CRASH_APPEND_W(buf, cch, len, L"    %s\r\n", d);
    }
}

static DWORD WINAPI ShutdownWatchdogProc(LPVOID) {
    DWORD waitedMs = 0;
    for (;;) {
        Sleep(kShutdownGraceMs);
        waitedMs += kShutdownGraceMs;

        HangThread threads[64];
        const int n = CaptureAllThreads(threads, _countof(threads));
        const char* stage = g_pShutdownStage;

        static wchar_t full[24 * 1024];
        size_t len = 0;
        CRASH_APPEND_W(full, _countof(full), len, L"PGFA did not exit within %lu seconds of closing.\r\nLast shutdown step reached: %S\r\n\r\nThreads:\r\n",
                       waitedMs / 1000, stage && stage[0] ? stage : "(closing started, no step recorded)");
        for (int i = 0; i < n; i++)
            FormatHangThread(threads[i], 24, full, _countof(full), len);

        // The log goes next to the exe (or %TEMP%), like the crash log.
        wchar_t logPath[MAX_PATH] = L"";
        if (GetModuleFileNameW(NULL, logPath, MAX_PATH)) {
            if (wchar_t* slash = wcsrchr(logPath, L'\\'))
                wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - logPath), L"shutdown_hang_log.txt");
        }
        FILE* f = nullptr;
        if (_wfopen_s(&f, logPath, L"wb, ccs=UTF-16LE") != 0 || !f) {
            GetTempPathW(MAX_PATH, logPath);
            wcscat_s(logPath, L"PGFA_shutdown_hang_log.txt");
            _wfopen_s(&f, logPath, L"wb, ccs=UTF-16LE");
        }
        if (f) {
            fwrite(full, sizeof(wchar_t), len, f);
            fclose(f);
        }

        // The dialog shows the stuck step and the UI/game threads; the rest is in the log.
        static wchar_t msg[4096];
        size_t mlen = 0;
        CRASH_APPEND_W(msg, _countof(msg), mlen, L"PGFA did not exit within %lu seconds of closing.\r\n\r\nLast shutdown step reached:\r\n  %S\r\n\r\n",
                       waitedMs / 1000, stage && stage[0] ? stage : "(closing started, no step recorded)");
        for (int i = 0; i < n; i++)
            if (threads[i].tid == g_dwMainThreadId || threads[i].tid == g_dwGameThreadId)
                FormatHangThread(threads[i], 6, msg, _countof(msg), mlen);
        CRASH_APPEND_W(msg, _countof(msg), mlen, L"\r\nAll thread stacks were saved to:\r\n%s\r\n\r\nTerminate PGFA now?\r\nYes = terminate    No = keep waiting", logPath);

        const int answer = MessageBoxW(NULL, msg, L"PGFA is not closing",
                                       MB_YESNO | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND | MB_DEFBUTTON2);
        if (answer == IDYES)
            TerminateProcess(GetCurrentProcess(), 1);
        // Keep waiting: report again (with fresh stacks) if it is still alive.
    }
}

// Begins the countdown; safe to call repeatedly and from any thread.
void ShutdownWatchdogStart() {
    if (InterlockedExchange(&g_bShutdownWatchdog, 1))
        return;
    g_pShutdownStage = "close requested";
    HANDLE h = CreateThread(NULL, 0, ShutdownWatchdogProc, NULL, 0, NULL);
    if (h)
        CloseHandle(h);
}

INT WINAPI WinMain( HINSTANCE hInstance, HINSTANCE, LPSTR lpszCmdLine, INT nCmdShow )
{
    g_dwMainThreadId = GetCurrentThreadId();
    {
        // -bench <script>: deterministic headless benchmark (see Bench.h). Decided before anything reads the config.
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            for (int i = 1; i + 1 < argc; i++) {
                if (wcscmp(argv[i], L"-bench") == 0) {
                    if (BenchParseScript(argv[i + 1], g_Bench)) {
                        g_Bench.active = true;
                        g_bBenchMode = true;
                    } else {
                        MessageBoxW(NULL, (std::wstring(L"Could not read the bench script (needs midi= and out=):\n") + argv[i + 1]).c_str(), L"-bench", MB_ICONERROR);
                        return 2;
                    }
                    break;
                }
            }
            LocalFree(argv);
        }
    }
    StartupLog_Begin();
    StartupLog("cmdline: %s", lpszCmdLine ? lpszCmdLine : "");
    if (!InitEmbeddedDLLs(hInstance)) {
        StartupLog_End("embedded DLL init failed");
        return 1;
    }
    StartupLog("embedded DLLs loaded");

    AddVectoredExceptionHandler(1, VectoredCrashHandler);
    AddVectoredExceptionHandler(0, FinalCrashBackstop); // registered last: runs when nothing else handled
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
        // The game thread never gets here (GameThreadBody's __except catches everything).
        HandleForeignThreadCrash(ep);
    });
    std::set_terminate(TerminateHandler);

    g_hInstance = hInstance;
    srand( ( unsigned )time( NULL ) );

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof( INITCOMMONCONTROLSEX );
    icex.dwICC  = ICC_WIN95_CLASSES | ICC_COOL_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex); 

    HRESULT hr = CoInitialize( NULL );
    if ( FAILED( hr ) ) return 1;

    // Register the window class
    WNDCLASSEX wc;
    wc.cbSize = sizeof( WNDCLASSEX );
    wc.style = 0;
    wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0L;
    wc.cbWndExtra = 0L;
    wc.hInstance = hInstance;
    HICON applicationIcon = LoadApplicationIconFromPNG(hInstance);
    wc.hIcon = applicationIcon ? applicationIcon : LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor( NULL, IDC_ARROW );
    // Window is only a container... never seen, thus null brush
    wc.hbrBackground = NULL; //( HBRUSH )GetStockObject( NULL_BRUSH );
    wc.lpszMenuName = NULL;
    wc.lpszClassName = CLASSNAME;
    wc.hIconSm = wc.hIcon;
    if ( !RegisterClassEx( &wc ) )
        return 1;

    wc.style = CS_OWNDC;
    wc.lpfnWndProc = GfxProc;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = GFXCLASSNAME;
    if ( !RegisterClassEx( &wc ) )
        return 1;

    // In addition to getting settings, triggers loading of saved config
    Config &config = Config::GetConfig();
    ViewSettings &cView = config.GetViewSettings();
    PlaybackSettings &cPlayback = config.GetPlaybackSettings();
    if (g_Bench.active) {
        // Defaults only, no audio, nothing that depends on the clock or on the user's settings.
        SHCreateDirectoryExW(NULL, g_Bench.outDir.c_str(), NULL);
        srand(1);
        config.GetVideoSettings().eRenderer = g_Bench.renderer ? VideoSettings::DirectX12 : VideoSettings::DirectX11;
        config.GetVideoSettings().bLimitFPS = false;
        config.GetVideoSettings().bShowFPS = false;
        config.GetAudioSettings().bPreRenderAudio = false;
        config.GetAudioSettings().iOutDevice = -1;
        config.GetVizSettings().bKDMAPI = false;
        config.GetVizSettings().bDisableUI = !g_Bench.ui;
        config.GetVizSettings().bTickBased = g_Bench.tick;
        config.GetVizSettings().bDualPianoRoll = g_Bench.dual;
        config.GetVizSettings().bHorizontalPianoRoll = g_Bench.horizontal && !g_Bench.dual;
        config.GetVizSettings().bEnableLogging = false;
        config.GetVisualSettings().eKeysShown = VisualSettings::All;
        cView.SetKeyboard(g_Bench.keys, false);
        cView.SetControls(false, false);
        if (g_Bench.sliding >= 0)
            SetEnvironmentVariableA("PGFA_SLIDING", g_Bench.sliding >= 2 ? "2" : (g_Bench.sliding ? "1" : "0"));
    }
    {
        const AudioSettings& cAudio = config.GetAudioSettings();
        const wchar_t* outName = (cAudio.iOutDevice >= 0 && cAudio.iOutDevice < (int)cAudio.vMIDIOutDevices.size())
            ? cAudio.vMIDIOutDevices[cAudio.iOutDevice].c_str() : L"(none)";
        StartupLog("config: renderer=%s kdmapi=%d midiOut=%d '%ls' prerender=%d logging=%d",
                   config.GetVideoSettings().eRenderer == VideoSettings::DirectX12 ? "DX12" : "DX11",
                   (int)config.GetVizSettings().bKDMAPI, cAudio.iOutDevice, outName,
                   (int)cAudio.bPreRenderAudio, (int)g_bLoggingEnabled.load(std::memory_order_relaxed));
    }

    g_iKDMAPISynth = config.GetAudioSettings().iKDMAPISynth;

    // Bug the user if WinMM isn't patched
    if (!config.GetVizSettings().bKDMAPI || !PGFAGetKDMAPIModule()) {
        wchar_t winmm_path[1024] = {};
        wchar_t win_dir[MAX_PATH] = {};
        if (GetModuleFileName(GetModuleHandle(L"winmm.dll"), winmm_path, _countof(winmm_path)) &&
            GetWindowsDirectory(win_dir, MAX_PATH) &&
            FindNLSString(0, LINGUISTIC_IGNORECASE, winmm_path, -1, win_dir, -1, NULL) == 0) {
            MessageBox(NULL, L"You don't appear to be using a patched winmm.dll.\nPlease patch it for best results.", L"", MB_ICONWARNING);
        }
    }

    int winLeft = cView.GetMainLeft(), winTop = cView.GetMainTop(), winW = cView.GetMainWidth(), winH = cView.GetMainHeight();
    if (g_Bench.active) {
        RECT rcWanted = { 0, 0, g_Bench.width, g_Bench.height };
        AdjustWindowRectEx(&rcWanted, WS_OVERLAPPEDWINDOW, FALSE, 0);
        winLeft = 40; winTop = 40; // fully on screen so the compositor keeps presenting
        winW = rcWanted.right - rcWanted.left;
        winH = rcWanted.bottom - rcWanted.top;
    }
    g_hWnd = CreateWindowEx( 0, CLASSNAME, L"PlayGroundFromAbove " __DATE__, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, winLeft, winTop,
                             winW, winH, NULL, NULL, wc.hInstance, NULL );
    if ( !g_hWnd ) return 1;

    // Accept drag and drop
    DragAcceptFiles(g_hWnd, true);

    // Creation order (z-order) matters big time for full screen

    g_hWndGfx = CreateWindowEx( 0, GFXCLASSNAME, NULL, WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS,
                                0, 0, 800, 600, g_hWnd, NULL, wc.hInstance, NULL );
    if ( !g_hWndGfx ) return 1;

    HACCEL hAccel = LoadAccelerators( hInstance, MAKEINTRESOURCE( IDA_MAINMENU ) );
    if ( !hAccel ) return 1;

    StartupLog("windows created");

    // Get the game going
    HANDLE hThread = CreateThread( NULL, 8 * 1024 * 1024, GameThread, new SplashScreen( NULL, NULL ), 0, NULL );
    if ( !hThread ) return 1;
    StartupLog("game thread started");

    // Set up GUI and show
    SetPlayMode( GameState::Splash );
    SetOnTop( cView.GetOnTop() );
    ShowControls( cView.GetControls() );
    ShowWindow( g_hWndGfx, SW_SHOW );
    ShowWindow( g_hWnd, g_Bench.active ? SW_SHOWNOACTIVATE : nCmdShow );
    UpdateWindow( g_hWnd );
    SetFocus( g_hWndGfx );
    cPlayback.SetPaused( false, false );

    if ( g_Bench.active )
    {
        PlayFile( g_Bench.midi, false );
    }
    else if ( lpszCmdLine && lpszCmdLine[0] )
    {
        // Tokenize the command line (quotes respected)
        std::vector<std::wstring> tokens;
        {
            // lpszCmdLine is the ANSI command line: characters outside the code page (e.g. a fullwidth
            // question mark in a file name) are already replaced by '?'. Read the wide one instead.
            int argc = 0;
            if ( LPWSTR* argv = CommandLineToArgvW( GetCommandLineW(), &argc ) )
            {
                for ( int i = 1; i < argc; i++ )
                    if ( argv[i][0] ) tokens.push_back( argv[i] );
                LocalFree( argv );
            }
        }

        if ( tokens.size() >= 4 && tokens[0] == L"-repro" )
        {
            // -repro "<midiA>" "<audio>" "<midiB>" [delaySecs]
            g_bReproCustomAudio = true;
            g_sReproCustomAudioPath = tokens[2];
            double delaySecs = ( tokens.size() >= 5 ) ? wcstod( tokens[4].c_str(), NULL ) : 2.0;
            PRE_DbgLog( "REPRO: open A='%ls' audio='%ls' B='%ls' delay=%g",
                        tokens[1].c_str(), tokens[2].c_str(), tokens[3].c_str(), delaySecs );
            PlayFile( tokens[1], true );
            // After the delay, open song B plainly (WM_REPRO_OPEN handler).
            struct ReproDelayArgs { double secs; std::wstring* path; };
            ReproDelayArgs* pArgs = new ReproDelayArgs{ delaySecs, new std::wstring( tokens[3] ) };
            CreateThread( NULL, 0, []( LPVOID p ) -> DWORD {
                ReproDelayArgs* a = (ReproDelayArgs*)p;
                Sleep( (DWORD)( a->secs * 1000.0 ) );
                PostMessage( g_hWnd, WM_REPRO_OPEN, 0, (LPARAM)a->path );
                delete a;
                return 0;
            }, pArgs, 0, NULL );
        }
        else if ( !tokens.empty() )
        {
            PlayFile( tokens[0], false );
        }
    }

    CreateThread(NULL, 0, UpdateCheckProc, NULL, 0, NULL);

    MSG msg = {};
    while( GetMessage( &msg, NULL, 0, 0 ) )
    {
        if( !TranslateAccelerator( g_hWnd, hAccel, &msg ) )
        {
            TranslateMessage( &msg );
            DispatchMessage( &msg );
        }
    }

    StartupLog_End("app closed before a MIDI was loaded");
    ShutdownWatchdogStart();
    ShutdownStage("UI: message loop ended, waiting for the game thread to finish");
    g_MsgQueue.ForcePush( msg );
    WaitForSingleObject( hThread, INFINITE );

    ShutdownStage("UI: saving config");
    config.SaveConfigValues();

    ShutdownStage("UI: unregistering window class / CoUninitialize");
    UnregisterClass( CLASSNAME, wc.hInstance );
    CoUninitialize();
    InterlockedExchange(&g_bTeardown, 1);
    ShutdownStage("UI: freeing embedded DLLs (BASS/SDL)");
    FreeEmbeddedDLLs();
    ShutdownStage("UI: WinMain returning (CRT exit, static destructors, DLL unload)");
    return 0;
}

// Creates a renderer and runs the full boot init ladder: up to three direct
// attempts, then a D3D12 -> D3D11 fallback, then hardware D3D11 -> WARP.
// Returns nullptr when nothing can be initialized (the caller decides whether
// that is fatal). Used both at boot and when a lost device cannot recover.
static Renderer* InitRendererLadder(std::tuple<HRESULT, const char*>* pLastFail = nullptr, bool bShowFallbackMsgs = true)
{
    Renderer* pRenderer = Renderer::CreateInstance();
    std::tuple<HRESULT, const char*> init_res = { E_FAIL, "Init not attempted" };
    auto TryInitOnce = [&]() {
        init_res = pRenderer->Init(g_hWndGfx, Config::GetConfig().GetVideoSettings().bLimitFPS);
        if (FAILED(std::get<0>(init_res))) {
            char ebuf[160];
            sprintf_s(ebuf, "init_failed:%s hr=0x%08X", std::get<1>(init_res), (unsigned)std::get<0>(init_res));
            HeartbeatLog(ebuf);
        }
    };
    for (int attempt = 0; attempt < 3; attempt++) {
        TryInitOnce();
        if (SUCCEEDED(std::get<0>(init_res)))
            break;
        Sleep(500);
    }
    // A user-requested D3D12 backend that cannot initialize falls back to D3D11
    // for the rest of the session (the D3D12 option is disabled from then on).
    if (FAILED(std::get<0>(init_res)) && !g_bBootedFallback &&
        Config::GetConfig().GetVideoSettings().eRenderer == VideoSettings::DirectX12) {
        g_bBootedFallback = true;
        delete pRenderer;
        pRenderer = Renderer::CreateInstance();
        init_res = { E_FAIL, "Init not attempted" };
        for (int attempt = 0; attempt < 3; attempt++) {
            TryInitOnce();
            if (SUCCEEDED(std::get<0>(init_res)))
                break;
            Sleep(500);
        }
        if (SUCCEEDED(std::get<0>(init_res)) && bShowFallbackMsgs)
            MessageBox(g_hWnd, TEXT("DirectX 12 could not be initialized; this session is running on DirectX 11."), TEXT("PlayGroundFromAbove"), MB_OK | MB_ICONINFORMATION);
    }
    // Last resort: hardware D3D11 failed to create a swap chain (e.g. WDDM 1.x,
    // RDP, or a virtual GPU). Retry once on the WARP software rasterizer so the
    // app can still boot.
    if (FAILED(std::get<0>(init_res)) && !g_bForceWARP) {
        g_bForceWARP = true;
        delete pRenderer;
        pRenderer = Renderer::CreateInstance();
        init_res = { E_FAIL, "Init not attempted" };
        for (int attempt = 0; attempt < 3; attempt++) {
            TryInitOnce();
            if (SUCCEEDED(std::get<0>(init_res)))
                break;
            Sleep(500);
        }
        if (SUCCEEDED(std::get<0>(init_res)) && bShowFallbackMsgs)
            MessageBox(g_hWnd, TEXT("Hardware accelerated rendering is unavailable on this system; this session is running on the software (WARP) renderer."), TEXT("PlayGroundFromAbove"), MB_OK | MB_ICONINFORMATION);
    }
    if (FAILED(std::get<0>(init_res))) {
        if (pLastFail)
            *pLastFail = init_res;
        delete pRenderer;
        return nullptr;
    }
    return pRenderer;
}

wchar_t g_wszRecoverDetail[192] = L"";

// Runs on the game thread. When the active D3D backend loses its device and
// cannot rebuild on the same API (e.g. D3D12CreateDevice keeps failing with
// E_FAIL after an iGPU TDR), fall back through the boot init ladder - DirectX
// 11 hardware, then the WARP software rasterizer - instead of retrying the
// dead backend and popping a scary error for something we can recover from.
// The Error box is only posted if every backend fails.
static void RecoverRenderer(Renderer*& pRenderer, GameState* pGameState) {
    static bool s_bErrorPosted = false;
    // Always log diagnostics for the whole recovery attempt (the toggle may be
    // off and this is exactly when we need the trace).
    const bool wasLogging = g_bLoggingEnabled.load(std::memory_order_relaxed);
    g_bLoggingEnabled.store(true, std::memory_order_relaxed);

    HRESULT res = pRenderer->RecoverDevice(g_hWndGfx, Config::GetConfig().GetVideoSettings().bLimitFPS);
    g_wszRecoverDetail[0] = L'\0';
    if (SUCCEEDED(res)) {
        s_bErrorPosted = false;
        HeartbeatLog("recover:ok");
        g_bLoggingEnabled.store(wasLogging, std::memory_order_relaxed);
        return;
    }

    // The failed renderer still owns a swap chain bound to the window; free it first
    // or every replacement backend fails CreateSwapChainForHwnd with INVALID_CALL.
    pRenderer->ReleaseForReplacement();
    Sleep(300); // let the driver settle after the TDR before rebuilding
    std::tuple<HRESULT, const char*> ladderFail = {};
    Renderer* pNew = InitRendererLadder(&ladderFail, /*bShowFallbackMsgs*/ false);
    if (pNew)
    {
        // A different backend came up. Swap it in. A video render's capture
        // size is bound to the old swapchain, so cancel it: the user can press
        // Start Render again on the stable backend.
        if (g_bVideoRendering)
            StopVideoRender();
        if (pGameState)
            pGameState->SetRenderer(pNew);
        delete pRenderer;
        pRenderer = pNew;
        g_pwszRenderMode = pRenderer->GetModeName();
        SetMainTitle(L"PlayGroundFromAbove");
        g_bLoggingEnabled.store(true, std::memory_order_relaxed);
        HeartbeatLog("recover:backend-switched");
        g_bLoggingEnabled.store(wasLogging, std::memory_order_relaxed);
        if (!wasLogging)
            MessageBoxW(g_hWnd, L"DirectX 12 stopped responding while starting the render. The app has switched to DirectX 11 to keep running - press Start Render again.",
                        L"PlayGroundFromAbove", MB_OK | MB_ICONWARNING);
        return;
    }

    // Nothing can initialize. Surface the TRUE final failure (last ladder
    // attempt, e.g. WARP), not the initial D3D12 stage.
    wchar_t stage[128] = L"?";
    const char* sStage = std::get<1>(ladderFail);
    if (!sStage || !*sStage)
        sStage = pRenderer->LastRecoverStage();
    MultiByteToWideChar(CP_UTF8, 0, sStage, -1, stage, 128);
    swprintf_s(g_wszRecoverDetail, L"\n\nFailing step: %s (hr=0x%08X)", stage, (unsigned)std::get<0>(ladderFail));
    char ebuf[192];
    sprintf_s(ebuf, "recover:all-backends-failed %s hr=0x%08X", sStage, (unsigned)std::get<0>(ladderFail));
    HeartbeatLog(ebuf);
    if (!s_bErrorPosted) {
        s_bErrorPosted = true;
        PostMessage(g_hWnd, WM_COMMAND, ID_GAMEERROR, (LPARAM)GameState::DirectXError);
    }
    g_bLoggingEnabled.store(wasLogging, std::memory_order_relaxed);
}

static const DWORD kGameThreadCrashed = 0xDEAD;
static Renderer* s_pGameRenderer = nullptr; // Current renderer, for abandoning it after a crash

// "Try to continue" after a game thread crash: the crashed state and renderer are leaked rather
// than deleted (their destructors could crash again). The state's audio is silenced and the
// renderer's swap chain released so a fresh renderer can bind to the same window.
static void AbandonCrashedGame()
{
    __try {
        if (g_pGameState)
            g_pGameState->AbandonAfterCrash();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try {
        if (s_pGameRenderer)
            s_pGameRenderer->ReleaseForReplacement();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_pGameState = nullptr;
    s_pGameRenderer = nullptr;
    // A crash inside the cleanup above may have been reported and continued too.
    InterlockedExchange(&g_bGameCrashReported, 0);
}

static DWORD GameThreadBody( LPVOID lpParameter );

DWORD WINAPI GameThread( LPVOID lpParameter )
{    if ( !g_hWndGfx ) { delete reinterpret_cast<GameState*>(lpParameter); return 0; }
    g_dwGameThreadId = GetCurrentThreadId();

    for (;;)
    {
        const DWORD result = GameThreadBody( lpParameter );
        if ( result != kGameThreadCrashed )
            return result;
        _resetstkoflw(); // Re-arm the guard page if the crash was a stack overflow
        AbandonCrashedGame();
        HeartbeatLog( "crash: game thread restarting on the start screen" );
        lpParameter = new SplashScreen( NULL, NULL );
        // Put the UI's play mode and toolbar back in their no-song state as well.
        PostMessage( g_hWnd, WM_COMMAND, ID_CRASH_RECOVERED, 0 );
    }
}

static DWORD GameThreadBody( LPVOID lpParameter )
{
    __try {

    std::tuple<HRESULT, const char*> boot_fail = { S_OK, "" };
    StartupLog("renderer init");
    Renderer *pRenderer = InitRendererLadder(&boot_fail);
    s_pGameRenderer = pRenderer;
    if ( !pRenderer )
    {
        StartupLog("renderer init failed: %s hr=0x%08X", std::get<1>(boot_fail), (unsigned)std::get<0>(boot_fail));
        wchar_t msg[1024] = {};
        _snwprintf_s(msg, 1024, L"Fatal error initializing the graphics device.\n%S failed with code 0x%x.", std::get<1>(boot_fail), std::get<0>(boot_fail));
        MessageBox( g_hWnd, msg, TEXT( "Error" ), MB_OK | MB_ICONEXCLAMATION );
        delete reinterpret_cast<GameState*>(lpParameter);
        PostMessage( g_hWnd, WM_QUIT, 1, 0 );
        return 1;
    }

    GameState *pGameState = reinterpret_cast< GameState* >( lpParameter );
    pGameState->SetHWnd( g_hWndGfx );
    pGameState->SetRenderer( pRenderer );
    pGameState->Init();
    g_pGameState = pGameState;
    GameState::GameError ge;

    wchar_t buf[1024] = {};
    g_pwszRenderMode = pRenderer->GetModeName();
    StartupLog("renderer ready: %ls on '%ls'%s", g_pwszRenderMode, pRenderer->GetAdapterName().c_str(),
               g_bForceWARP ? " (WARP fallback)" : g_bBootedFallback ? " (DX12 -> DX11 fallback)" : "");
#ifdef __AVX2__
    _snwprintf_s(buf, 1024, L"PlayGroundFromAbove %S (AVX2 build, Device: %s, Mode: %s)", __DATE__, pRenderer->GetAdapterName().c_str(), g_pwszRenderMode);
#else
    _snwprintf_s(buf, 1024, L"PlayGroundFromAbove %S (SSE4.2 build, Device: %s, Mode: %s)", __DATE__, pRenderer->GetAdapterName().c_str(), g_pwszRenderMode);
#endif
    SetWindowTextW(g_hWnd, buf);

    MSG msg = {};
    auto tLastFrame = std::chrono::steady_clock::now();
    while( msg.message != WM_QUIT )
    {
        {
            auto tNow = std::chrono::steady_clock::now();
            double dtS = std::chrono::duration<double>(tNow - tLastFrame).count();
            tLastFrame = tNow;
            g_fGameFPS = (float)(dtS > 0.0 ? 1.0 / dtS : 1000.0f);
        }
        // WaitForGPU runs; otherwise the fence signal queues up behind hundreds
        const bool bResetHeld = g_bResetPending;
        static auto s_tLoopLog = std::chrono::steady_clock::now();
        auto tNowLoop = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(tNowLoop - s_tLoopLog).count() >= 1000)
        {
            s_tLoopLog = tNowLoop;
            HeartbeatLog("loop");
        }
        while ( g_MsgQueue.Pop( msg ) )
        {
            if ( msg.message == WM_COMMAND && LOWORD( msg.wParam ) == ID_PRELOAD_DISCARD )
            {
                // A new song is about to load on the UI thread: free the current
                // song's data here (this thread owns the state) before the parse
                // starts, so the combined memory peak stays off the TDR cliff.
                HANDLE hDiscardDone = (HANDLE)msg.lParam;
                if (pGameState)
                {
                    HeartbeatLog("discard:begin");
                    pGameState->Discard();
                    HeartbeatLog("discard:done");
                }
                if (hDiscardDone)
                    SetEvent(hDiscardDone);
                continue;
            }
            if ( msg.message == WM_COMMAND && LOWORD( msg.wParam ) == ID_PRELOAD_DRAIN )
            {
                // Window transition about to animate: finish all queued GPU work
                // (including the in-flight frame's present) while the window is
                // still at its current size, then render nothing until the reset.
                HeartbeatLog("drain:start");
                pRenderer->WaitForGPU();
                HeartbeatLog("drain:done");
                if (msg.lParam)
                    SetEvent((HANDLE)msg.lParam);
                continue;
            }
            if ( msg.message == WM_KEYDOWN && msg.wParam == VK_F12 )
            {
                // Test hotkey for the crash reporter. F12 crashes the game thread (SEH path);
                // Shift+F12 crashes a separate thread (vectored handler path, like a crash inside a
                // synth or audio driver thread).
                StartupLog( "F12 test crash requested" );
                if ( GetKeyState( VK_SHIFT ) < 0 )
                {
                    HANDLE hCrash = CreateThread( NULL, 0, []( LPVOID ) -> DWORD {
                        *(volatile int*)nullptr = 0xF12;
                        return 0;
                    }, NULL, 0, NULL );
                    if ( hCrash ) CloseHandle( hCrash );
                    continue;
                }
                *(volatile int*)nullptr = 0xF12;
            }
            if ( msg.message == WM_COMMAND && LOWORD( msg.wParam ) == ID_VIEW_RESETDEVICE )
            {
                g_bResetPending = false;
                pGameState->MsgProc( msg.hwnd, msg.message, msg.wParam, msg.lParam );
                // The swapchain was recreated at the new size on an idle queue;
                // the transition is over, let frames flow again.
                g_bInSizeMove = false;
                g_bSysResize = false;
                continue;
            }
            pGameState->MsgProc( msg.hwnd, msg.message, msg.wParam, msg.lParam );
        }

        if (pRenderer->DeviceLost()) {
            HeartbeatLog("recover:drain");
            RecoverRenderer(pRenderer, pGameState);
            continue;
        }

        if (!g_bDisableGates && (g_bInSizeMove || bResetHeld)) {
            HeartbeatLog("skip:insizemove");
            Sleep(10);
            continue;
        }

        if ( ( ge = GameState::ChangeState( pGameState->NextState(), &pGameState ) ) != GameState::Success )
            PostMessage( g_hWnd, WM_COMMAND, ID_GAMEERROR, ge );
        g_pGameState = pGameState;
        s_pGameRenderer = pRenderer; // RecoverRenderer may have swapped it
        StartupLog_OnFrame( pGameState->DebugName() );
        {
            static auto s_tStateLog = std::chrono::steady_clock::now();
            auto tNowState = std::chrono::steady_clock::now();
            if ( std::chrono::duration_cast<std::chrono::milliseconds>(tNowState - s_tStateLog).count() >= 1000 )
            {
                s_tStateLog = tNowState;
                char buf[96];
                sprintf_s(buf, "state:%s(%p) next=%s", pGameState->DebugName(), (void*)pGameState,
                          pGameState->NextState() ? pGameState->NextState()->DebugName() : "none");
                HeartbeatLog(buf);
            }
            auto tLogic = std::chrono::steady_clock::now();
            pGameState->Logic();
            if (!g_bDisableGates && (g_bResetPending || g_bInSizeMove))
                continue;
            auto tRender = std::chrono::steady_clock::now();
            pGameState->Render();
            auto tEnd = std::chrono::steady_clock::now();
#ifdef PGFA_IMGBUF_TRACE
            {
                auto& ibt = PGFAIbTraceGet();
                ibt.ms[IB_LOGIC] += std::chrono::duration<double, std::milli>(tRender - tLogic).count(); ibt.calls[IB_LOGIC]++;
                ibt.ms[IB_RENDER] += std::chrono::duration<double, std::milli>(tEnd - tRender).count(); ibt.calls[IB_RENDER]++;
                PGFAIbTraceFlush(std::chrono::duration<double, std::milli>(tEnd - tLogic).count());
            }
#endif
            static int s_frameLog = 0;
            static double s_logicMs = 0.0, s_renderMs = 0.0;
            s_logicMs += std::chrono::duration<double, std::milli>(tRender - tLogic).count();
            s_renderMs += std::chrono::duration<double, std::milli>(tEnd - tRender).count();
            if ((s_frameLog++ & 127) == 127) {
                char buf[96];
                sprintf_s(buf, "frame:logic=%.2f render=%.2f", s_logicMs / 128.0, s_renderMs / 128.0);
                HeartbeatLog(buf);
                s_logicMs = s_renderMs = 0.0;
            }
        }

        if (pRenderer->DeviceLost()) {
            HeartbeatLog("recover:postrender");
            RecoverRenderer(pRenderer, pGameState);
        }
    }

    ShutdownStage("game: waiting for the GPU to go idle");
    pRenderer->WaitForGPU();

    ShutdownStage("game: destroying the game state (audio threads, song data)");
    delete pGameState;
    g_pGameState = nullptr;
    ShutdownStage("game: destroying the renderer");
    delete pRenderer;
    s_pGameRenderer = nullptr;
    ShutdownStage("game: thread finished, waiting for the UI thread");

    } __except([&]() -> int {
        // Exceptions the vectored backstop already reported (the user chose to continue) skip the
        // dialog; anything else (e.g. a C++ exception) is reported here.
        if (!InterlockedExchange(&g_bGameCrashReported, 0) &&
            !WriteCrashLog(GetExceptionInformation(), CrashContinue::GameThread))
            TerminateProcess(GetCurrentProcess(), 1);
        return EXCEPTION_EXECUTE_HANDLER;
    }()) {
        return kGameThreadCrashed;
    }
    return 0;
}
