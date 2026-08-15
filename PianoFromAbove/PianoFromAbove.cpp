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
#include <ctime>
#include <shlwapi.h>
#include <winhttp.h>
#include <regex>
#include <clocale>
#include <cstdio>

#include "MainProcs.h"
#include "Globals.h"
#include "resource.h"

#include "Config.h"
#include "GameState.h"
#include "Renderer.h"
#include "Misc.h"
#include "MIDIPreRenderPlayer.h"

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

void HeartbeatLog(const char* tag) {
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
    default:                                 return L"UNKNOWN";
    }
}

static int WriteCrashLog(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    CONTEXT* ctx = ep->ContextRecord;

    wchar_t modulePath[MAX_PATH] = L"<unknown>";
    HMODULE hMod = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)ctx->Rip, &hMod)) {
        GetModuleFileNameW(hMod, modulePath, MAX_PATH);
        wchar_t* slash = wcsrchr(modulePath, L'\\');
        if (slash) wmemmove(modulePath, slash + 1, wcslen(slash + 1) + 1);
    }
    const unsigned long long llModBase = hMod ? (unsigned long long)(DWORD_PTR)hMod : 0;
    const unsigned long long llFaultOffset = ctx->Rip - (DWORD64)hMod;

    typedef USHORT(WINAPI* RtlCaptureStackBackTrace_t)(ULONG, ULONG, PVOID*, PULONG);
    static RtlCaptureStackBackTrace_t pCaptureStackBackTrace = (RtlCaptureStackBackTrace_t)
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlCaptureStackBackTrace");
    PVOID pFrames[64] = {};
    USHORT nFrames = pCaptureStackBackTrace ? pCaptureStackBackTrace(0, 64, pFrames, NULL) : 0;
    wchar_t stackBuf[8192] = L"";
    size_t stackLen = 0;
    for (USHORT i = 0; i < nFrames && stackLen < _countof(stackBuf) - 128; i++) {
        HMODULE hFrameMod = NULL;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)pFrames[i], &hFrameMod)) {
            wchar_t frameMod[MAX_PATH] = L"";
            GetModuleFileNameW(hFrameMod, frameMod, MAX_PATH);
            wchar_t* fs = wcsrchr(frameMod, L'\\');
            if (fs) wmemmove(frameMod, fs + 1, wcslen(fs + 1) + 1);
            stackLen += _snwprintf_s(stackBuf + stackLen, _countof(stackBuf) - stackLen, _TRUNCATE,
                L"  0x%016llX  %s+0x%llX\n",
                (unsigned long long)(DWORD_PTR)pFrames[i], frameMod,
                (unsigned long long)(DWORD_PTR)pFrames[i] - (unsigned long long)(DWORD_PTR)hFrameMod);
        } else {
            stackLen += _snwprintf_s(stackBuf + stackLen, _countof(stackBuf) - stackLen, _TRUNCATE,
                L"  0x%016llX  <unknown module>\n", (unsigned long long)(DWORD_PTR)pFrames[i]);
        }
    }

    wchar_t msg[12288];
    _snwprintf_s(msg, _countof(msg), _TRUNCATE,
        L"PlayGroundFromAbove has crashed.\n\n"
        L"Exception:  0x%08X  (%s)\n"
        L"Address:    0x%016llX\n"
        L"Module:     %s\n"
        L"Module Base: 0x%016llX\n"
        L"Fault Offset: 0x%08llX\n\n"
        L"Registers:\n"
        L"  RAX=%016llX  RBX=%016llX\n"
        L"  RCX=%016llX  RDX=%016llX\n"
        L"  RSI=%016llX  RDI=%016llX\n"
        L"  RSP=%016llX  RBP=%016llX\n"
        L"  RIP=%016llX  EFLAGS=%08X\n\n"
        L"Call stack:\n"
        L"%s"
        L"\nAbort = Quit, Ignore = Try to continue.",
        er->ExceptionCode,
        ExceptionCodeString(er->ExceptionCode),
        (unsigned long long)ctx->Rip,
        modulePath,
        llModBase,
        llFaultOffset,
        (unsigned long long)ctx->Rax,
        (unsigned long long)ctx->Rbx,
        (unsigned long long)ctx->Rcx,
        (unsigned long long)ctx->Rdx,
        (unsigned long long)ctx->Rsi,
        (unsigned long long)ctx->Rdi,
        (unsigned long long)ctx->Rsp,
        (unsigned long long)ctx->Rbp,
        (unsigned long long)ctx->Rip,
        ctx->EFlags,
        stackBuf);

    wchar_t logPath[MAX_PATH];
    if (GetModuleFileNameW(NULL, logPath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(logPath, L'\\');
        if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - logPath), L"crash_log.txt");
        FILE* f = NULL;
        if (_wfopen_s(&f, logPath, L"wb, ccs=UTF-16LE") == 0 && f) {
            fwprintf(f, L"%s\n", msg);
            fclose(f);
        }
    }

    int result = MessageBoxW(NULL, msg, L"PlayGroundFromAbove - Crash",
        MB_ICONERROR | MB_ABORTRETRYIGNORE | MB_TOPMOST | MB_SETFOREGROUND);
    return (result == IDIGNORE);
}

static DWORD WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    WriteCrashLog(ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

static DWORD g_dwGameThreadId = 0;

static LONG WINAPI VectoredCrashHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW &&
        ep->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION &&
        ep->ExceptionRecord->ExceptionCode != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        ep->ExceptionRecord->ExceptionCode != EXCEPTION_IN_PAGE_ERROR &&
        ep->ExceptionRecord->ExceptionCode != EXCEPTION_DATATYPE_MISALIGNMENT)
        return EXCEPTION_CONTINUE_SEARCH;

    if (GetCurrentThreadId() == g_dwGameThreadId)
        return EXCEPTION_CONTINUE_SEARCH;

    wchar_t logPath[MAX_PATH];
    if (GetModuleFileNameW(NULL, logPath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(logPath, L'\\');
        if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - logPath), L"crash_log.txt");
        FILE* f = NULL;
        if (_wfopen_s(&f, logPath, L"wb, ccs=UTF-16LE") == 0 && f) {
            fwprintf(f, L"Crash on non-game thread: 0x%08X at 0x%016llX\n",
                ep->ExceptionRecord->ExceptionCode,
                (unsigned long long)ep->ContextRecord->Rip);
            fclose(f);
        }
    }
    WriteCrashLog(ep);
    TerminateProcess(GetCurrentProcess(), 1);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void __cdecl TerminateHandler() {
    wchar_t msg[512];
    _snwprintf_s(msg, _countof(msg), _TRUNCATE,
        L"PlayGroundFromAbove has encountered a fatal error.\n\n"
        L"std::terminate() was called (unhandled C++ exception, pure virtual call, etc.).\n"
        L"The application will now exit.");
    FILE* f = NULL;
    wchar_t logPath[MAX_PATH];
    if (GetModuleFileNameW(NULL, logPath, MAX_PATH)) {
        wchar_t* slash = wcsrchr(logPath, L'\\');
        if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - logPath), L"crash_log.txt");
        _wfopen_s(&f, logPath, L"wb, ccs=UTF-16LE");
        if (f) { fwprintf(f, L"%s\n", msg); fclose(f); }
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

// Runtime DLLs and config (SDL2/BASS) live in %APPDATA%\PlayGroundFromAbove rather
// than next to the exe. The DLLs are delay-loaded, so on first launch we migrate
// any copies found beside the exe into that folder and load them from there.
static bool EnsureAppDataRuntime()
{
    wchar_t szAppData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, szAppData)))
        return false;

    std::wstring dir = std::wstring(szAppData) + L"\\" L"PlayGroundFromAbove";
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES)
        if (!CreateDirectoryW(dir.c_str(), NULL))
            return false;

    static const wchar_t* const s_runtimeDLLs[] = { L"SDL2.dll", L"bass.dll", L"bassmidi.dll" };

    wchar_t szExe[MAX_PATH] = {};
    GetModuleFileNameW(NULL, szExe, MAX_PATH);
    std::wstring exeDir(szExe);
    size_t lastSlash = exeDir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos)
        exeDir.resize(lastSlash + 1);
    else
        exeDir.clear();

    for (const wchar_t* dll : s_runtimeDLLs) {
        std::wstring dst = dir + L"\\" + dll;
        if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::wstring src = exeDir + dll;
            if (GetFileAttributesW(src.c_str()) != INVALID_FILE_ATTRIBUTES)
                CopyFileW(src.c_str(), dst.c_str(), FALSE);
        }
        if (!LoadLibraryW(dst.c_str())) {
            wchar_t msg[1024];
            _snwprintf_s(msg, _TRUNCATE, L"Failed to load %ls from:\n%ls\n\nError %u",
                         dll, dst.c_str(), (unsigned)GetLastError());
            MessageBoxW(NULL, msg, L"PlayGroundFromAbove - Missing Runtime", MB_OK | MB_ICONERROR);
            return false;
        }
    }
    return true;
}

INT WINAPI WinMain( HINSTANCE hInstance, HINSTANCE, LPSTR lpszCmdLine, INT nCmdShow )
{
    if (!EnsureAppDataRuntime())
        return 1;

    AddVectoredExceptionHandler(1, VectoredCrashHandler);
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
        WriteCrashLog(ep);
        return EXCEPTION_EXECUTE_HANDLER;
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
    wc.hIcon = LoadIcon( hInstance, MAKEINTRESOURCE( IDI_PFAICON ) );
    wc.hCursor = LoadCursor( NULL, IDC_ARROW );
    // Window is only a container... never seen, thus null brush
    wc.hbrBackground = NULL; //( HBRUSH )GetStockObject( NULL_BRUSH );
    wc.lpszMenuName = NULL;
    wc.lpszClassName = CLASSNAME;
    wc.hIconSm = NULL;
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

    // Bug the user if WinMM isn't patched
    if (!config.GetVizSettings().bKDMAPI || !LoadLibrary(L"OmniMIDI")) {
        wchar_t winmm_path[1024] = {};
        wchar_t win_dir[MAX_PATH] = {};
        if (GetModuleFileName(GetModuleHandle(L"winmm.dll"), winmm_path, _countof(winmm_path)) &&
            GetWindowsDirectory(win_dir, MAX_PATH) &&
            FindNLSString(0, LINGUISTIC_IGNORECASE, winmm_path, -1, win_dir, -1, NULL) == 0) {
            MessageBox(NULL, L"You don't appear to be using a patched winmm.dll.\nPlease patch it for best results.", L"", MB_ICONWARNING);
        }
    }

    g_hWnd = CreateWindowEx( 0, CLASSNAME, L"PlayGroundFromAbove " __DATE__, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, cView.GetMainLeft(), cView.GetMainTop(),
                             cView.GetMainWidth(), cView.GetMainHeight(), NULL, NULL, wc.hInstance, NULL );
    if ( !g_hWnd ) return 1;

    // Accept drag and drop
    DragAcceptFiles(g_hWnd, true);

    // Creation order (z-order) matters big time for full screen

    g_hWndGfx = CreateWindowEx( 0, GFXCLASSNAME, NULL, WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS,
                                0, 0, 800, 600, g_hWnd, NULL, wc.hInstance, NULL );
    if ( !g_hWndGfx ) return 1;

    HACCEL hAccel = LoadAccelerators( hInstance, MAKEINTRESOURCE( IDA_MAINMENU ) );
    if ( !hAccel ) return 1;

    // Get the game going
    HANDLE hThread = CreateThread( NULL, 8 * 1024 * 1024, GameThread, new SplashScreen( NULL, NULL ), 0, NULL );
    if ( !hThread ) return 1;

    // Set up GUI and show
    SetPlayMode( GameState::Splash );
    SetOnTop( cView.GetOnTop() );
    ShowControls( cView.GetControls() );
    ShowWindow( g_hWndGfx, SW_SHOW );
    ShowWindow( g_hWnd, nCmdShow );
    UpdateWindow( g_hWnd );
    SetFocus( g_hWndGfx );
    cPlayback.SetPaused( false, false );

    if ( lpszCmdLine && lpszCmdLine[0] )
    {
        int iLen = MultiByteToWideChar( CP_ACP, 0, lpszCmdLine, -1, NULL, 0 );
        if ( iLen > 1 )
        {
            std::wstring wsPath( (size_t)iLen, L'\0' );
            MultiByteToWideChar( CP_ACP, 0, lpszCmdLine, -1, &wsPath[0], iLen );
            wsPath.resize( wsPath.size() - 1 ); // drop the trailing NUL
            while ( !wsPath.empty() && ( wsPath.front() == L'"' || wsPath.front() == L' ' ) )
                wsPath.erase( wsPath.begin() );
            while ( !wsPath.empty() && ( wsPath.back() == L'"' || wsPath.back() == L' ' ) )
                wsPath.pop_back();
            PlayFile( wsPath, false );
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

    g_MsgQueue.ForcePush( msg );
    WaitForSingleObject( hThread, INFINITE );

    config.SaveConfigValues();

    UnregisterClass( CLASSNAME, wc.hInstance );
    CoUninitialize();
    return 0;
}

static void RecoverRenderer(Renderer* pRenderer) {
    static bool s_bErrorPosted = false;
    HRESULT res = pRenderer->RecoverDevice(g_hWndGfx, Config::GetConfig().GetVideoSettings().bLimitFPS);
    if (FAILED(res)) {
        if (!s_bErrorPosted) {
            s_bErrorPosted = true;
            PostMessage(g_hWnd, WM_COMMAND, ID_GAMEERROR, (LPARAM)GameState::DirectXError);
        }
        Sleep(1000);
    } else {
        s_bErrorPosted = false;
    }
}

DWORD WINAPI GameThread( LPVOID lpParameter )
{    if ( !g_hWndGfx ) { delete reinterpret_cast<GameState*>(lpParameter); return 0; }
    g_dwGameThreadId = GetCurrentThreadId();

    __try {

    Renderer *pRenderer = Renderer::CreateInstance();
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
        if (SUCCEEDED(std::get<0>(init_res)))
            MessageBox(g_hWnd, TEXT("DirectX 12 could not be initialized; this session is running on DirectX 11."), TEXT("PlayGroundFromAbove"), MB_OK | MB_ICONINFORMATION);
    }
    // Last resort: hardware D3D11 failed to create a swap chain (e.g. WDDM 1.x,
    // RDP, or a virtual GPU). Retry once on the WARP software rasterizer so the
    // app can still boot.
    if (FAILED(std::get<0>(init_res)) && !g_bForceWARP && !g_bInRecovery) {
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
        if (SUCCEEDED(std::get<0>(init_res)))
            MessageBox(g_hWnd, TEXT("Hardware accelerated rendering is unavailable on this system; this session is running on the software (WARP) renderer."), TEXT("PlayGroundFromAbove"), MB_OK | MB_ICONINFORMATION);
    }
    if( FAILED(std::get<0>(init_res)) )
    {
        wchar_t msg[1024] = {};
        _snwprintf_s(msg, 1024, L"Fatal error initializing the graphics device.\n%S failed with code 0x%x.", std::get<1>(init_res), std::get<0>(init_res));
        MessageBox( g_hWnd, msg, TEXT( "Error" ), MB_OK | MB_ICONEXCLAMATION );
        delete pRenderer;
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
            RecoverRenderer(pRenderer);
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
            auto tRender = std::chrono::steady_clock::now();
            pGameState->Render();
            auto tEnd = std::chrono::steady_clock::now();
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
            RecoverRenderer(pRenderer);
        }
    }

    pRenderer->WaitForGPU();

    delete pGameState;
    g_pGameState = nullptr;
    delete pRenderer;

    } __except([&]() -> int {
        WriteCrashLog(GetExceptionInformation());
        return EXCEPTION_EXECUTE_HANDLER;
    }()) {}
    return 0;
}