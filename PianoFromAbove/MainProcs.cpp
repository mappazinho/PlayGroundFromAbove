/*************************************************************************************************
*
* File: MainProcs.cpp
*
* Description: Implements the main GUI functions. Mostly just window procs.
*
* Copyright (c) 2010 Brian Pantano. All rights reserved.
*
*************************************************************************************************/
#include <TChar.h>
#include <shlobj.h>
#include <Dbt.h>
#include <psapi.h>

#include <set>
#include <thread>
#include <fstream>

#include "MainProcs.h"
#include "ConfigProcs.h"
#include "Globals.h"
#include "resource.h"

#include "GameState.h"
#include "Config.h"
#include "MIDIPreRenderPlayer.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

VOID SizeWindows(int iMainWidth, int iMainHeight);

bool g_bShowLoading = false;
bool g_bInSizeMove = false;
bool g_bResetPending = false;
bool g_bSysResize = false;
bool g_bSkipGPUWait = false;
bool g_bDisableBlur = false;
bool g_bDisableGates = false; // Keep the size-move/reset gates ON: presenting through window animations stalls the flip queue on NVIDIA (2s+ fence waits -> TDR)
bool g_bInRecovery = false;
bool g_bD3D12Available = true;
bool g_bBootedFallback = false;
bool g_bForceWARP = false;
const wchar_t* g_pwszRenderMode = L"DirectX 12";

// ---- Render progress window -------------------------------------------------
// A plain Win32 popup that shows the render progress, output size, and render
// speed while a video render runs. It is a separate top-level window, so it is
// never captured into the video (the ImGui UI is hidden during renders). It is
// created and destroyed on the main thread (window messages), while the game
// thread feeds it via UpdateRenderProgressWindow (cross-thread SendMessage is
// marshaled to the main thread automatically).
#define WM_RENDERPROG_CREATE (WM_APP + 0x201)
#define WM_RENDERPROG_DESTROY (WM_APP + 0x202)

static HWND s_hRenderProgWnd = NULL;
static HWND s_hRenderProgBar = NULL;
static wchar_t s_wRenderProgText[512] = L"Rendering...";
static wchar_t s_wRenderProgLast[512] = L"";

static LRESULT CALLBACK RenderProgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CLOSE:
        // Closing the progress window cancels the render (same semantics as
        // closing the standalone renderer window).
        if (g_bVideoRendering)
            HandOffMsg(WM_COMMAND, ID_FILE_STOPRENDER, 0);
        return 0;
    case WM_DESTROY:
        s_hRenderProgWnd = NULL;
        s_hRenderProgBar = NULL;
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(230, 230, 230));
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT hOld = (HFONT)SelectObject(hdc, hFont);
        RECT rcText = { 12, 8, rc.right - 12, 62 };
        DrawTextW(hdc, s_wRenderProgText, -1, &rcText, DT_LEFT | DT_TOP | DT_WORDBREAK);
        SelectObject(hdc, hOld);
        EndPaint(hWnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

VOID RequestCreateRenderProgressWindow()
{
    PostMessageW(g_hWnd, WM_RENDERPROG_CREATE, 0, 0);
}

VOID RequestDestroyRenderProgressWindow()
{
    PostMessageW(g_hWnd, WM_RENDERPROG_DESTROY, 0, 0);
}

VOID UpdateRenderProgressWindow(INT iPermille, const wchar_t* wText)
{
    if (wText)
    {
        wcsncpy_s(s_wRenderProgText, wText, _TRUNCATE);
        if (wcscmp(s_wRenderProgText, s_wRenderProgLast) != 0)
        {
            wcscpy_s(s_wRenderProgLast, s_wRenderProgText);
            if (s_hRenderProgWnd)
                InvalidateRect(s_hRenderProgWnd, NULL, TRUE);
        }
    }
    if (s_hRenderProgBar)
        SendMessageW(s_hRenderProgBar, PBM_SETPOS, iPermille, 0);
}

static void CreateRenderProgressWindow()
{
    if (s_hRenderProgWnd)
        return;
    static bool s_bClassRegistered = false;
    if (!s_bClassRegistered)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = RenderProgProc;
        wc.hInstance = g_hInstance;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = L"PGFA_RenderProgress";
        RegisterClassExW(&wc);
        s_bClassRegistered = true;
    }
    s_hRenderProgWnd = CreateWindowExW(WS_EX_NOACTIVATE, L"PGFA_RenderProgress", L"Render to Video",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 340, 120, NULL, NULL, g_hInstance, NULL);
    if (!s_hRenderProgWnd)
        return;
    s_hRenderProgBar = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE, 12, 66, 300, 14, s_hRenderProgWnd, NULL, g_hInstance, NULL);
    SendMessageW(s_hRenderProgBar, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
    // Top-right of the work area, clear of the centered renderer window.
    RECT rcWork = {};
    SystemParametersInfo(SPI_GETWORKAREA, 0, &rcWork, 0);
    SetWindowPos(s_hRenderProgWnd, HWND_TOP, rcWork.right - 350, rcWork.top + 12, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
}

static void DestroyRenderProgressWindow()
{
    if (s_hRenderProgWnd)
        DestroyWindow(s_hRenderProgWnd);
    s_hRenderProgWnd = NULL;
    s_hRenderProgBar = NULL;
}

static void TraceMsg(const char* name, WPARAM wParam, LPARAM lParam) {
    static bool bInit = false;
    static __int64 nStart = 0;
    if (!bInit) {
        bInit = true;
        nStart = GetTickCount64();
    }
    __int64 ms = GetTickCount64() - nStart;
    wchar_t path[MAX_PATH] = {};
    GetTempPathW(_countof(path), path);
    wcscat_s(path, L"pfa_msgs.log");
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"a");
    if (f) {
        fprintf(f, "[%6lld.%03d] %s wp=0x%llx lp=%lldx%lld\n",
            (long long)(ms / 1000), (int)(ms % 1000), name,
            (unsigned long long)wParam, (long long)(short)LOWORD(lParam), (long long)(short)HIWORD(lParam));
        fclose(f);
    }
}

LRESULT WINAPI WndProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    switch (msg) {
        case WM_SIZE:         TraceMsg("WM_SIZE", wParam, lParam); break;
        case WM_ENTERSIZEMOVE: TraceMsg("WM_ENTERSIZEMOVE", wParam, lParam); break;
        case WM_EXITSIZEMOVE: TraceMsg("WM_EXITSIZEMOVE", wParam, lParam); break;
        case WM_WINDOWPOSCHANGED: TraceMsg("WM_WINDOWPOSCHANGED", wParam, lParam); break;
        case WM_SHOWWINDOW:   TraceMsg("WM_SHOWWINDOW", wParam, lParam); break;
        case WM_SYSCOMMAND:   TraceMsg("WM_SYSCOMMAND", wParam, lParam); break;
    }
    static PlaybackSettings &cPlayback = Config::GetConfig().GetPlaybackSettings();
    static ViewSettings &cView = Config::GetConfig().GetViewSettings();
    static const ControlsSettings &cControls = Config::GetConfig().GetControlsSettings();

    switch( msg )
    {
        case WM_RENDERPROG_CREATE:
            CreateRenderProgressWindow();
            return 0;
        case WM_RENDERPROG_DESTROY:
            DestroyRenderProgressWindow();
            return 0;
        case WM_COMMAND:
        {
            int iId = LOWORD( wParam );
            switch ( iId )
            {
                case IDOK:
                {
                    return 0;
                }
                case ID_FILE_PRACTICESONG: case ID_FILE_PRACTICESONGCUSTOM:
                {
                    CheckActivity( TRUE );
                    OPENFILENAME ofn = {};
                    TCHAR sFilename[1024] = { 0 };
                    ofn.lStructSize = sizeof( OPENFILENAME );
                    ofn.hwndOwner = hWnd;
                    ofn.lpstrFilter = TEXT( "MIDI Files\0*.mid;*.xz\0" );
                    ofn.lpstrFile = sFilename;
                    ofn.nMaxFile = sizeof( sFilename ) / sizeof( TCHAR );
                    ofn.lpstrTitle = TEXT( "Please select a song to play" );
                    ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    if ( GetOpenFileName( &ofn ) )
                        PlayFile( sFilename, iId == ID_FILE_PRACTICESONGCUSTOM );
                    return 0;
                }
                case ID_FILE_FREEPLAY:
                {
                    auto& cPlayback = Config::GetConfig().GetPlaybackSettings();
                    auto& cView = Config::GetConfig().GetViewSettings();
                    FreePlayScreen* pGameState = new FreePlayScreen(g_hWndGfx, NULL);
                    if (!cPlayback.GetPlayable()) cPlayback.SetPlayable(true, true);
                    cPlayback.SetPlayMode(GameState::Practice, true);
                    cPlayback.SetPaused(false, true);
                    cPlayback.SetPosition(0);
                    cView.SetZoomMove(false, true);
                    SetMainTitle(L"PlayGroundFromAbove - Free Play");
                    HandOffMsg(WM_COMMAND, ID_CHANGESTATE, (LPARAM)pGameState);
                    return 0;
                }
                case ID_FILE_CLOSEFILE:
                {
                    if ( !cPlayback.GetPlayMode() ) break;
                    cPlayback.SetPlayMode( GameState::Intro, true );
                    cPlayback.SetPlayable( false, true );
                    cPlayback.SetPosition( 0 );
                    SetMainTitle(L"PlayGroundFromAbove " __DATE__);
                    HandOffMsg( WM_COMMAND, ID_CHANGESTATE, ( LPARAM )new IntroScreen( NULL, NULL ) );
                    return 0;
                }
                case ID_FILE_RENDERVIDEO:
                {
                    // Toggles: starts a video render, or stops one in progress.
                    // Runs on the game thread (it owns the render state).
                    HandOffMsg( WM_COMMAND, ID_FILE_RENDERVIDEO, 0 );
                    return 0;
                }
                case ID_PRACTICE_DEFAULT:
                case ID_PRACTICE_CUSTOM:
                case ID_PLAY_PLAY:
                    if ( cPlayback.GetPlayMode() && iId == ID_PLAY_PLAY )
                        cPlayback.SetPaused( false, true );
                    return 0;
                case ID_PLAY_PAUSE:
                    cPlayback.SetPaused( true, true );
                    return 0;
                case ID_PLAY_PLAYPAUSE:
                    if ( cPlayback.GetPlayMode() ) cPlayback.TogglePaused( true );
                    return 0;
                case ID_PLAY_STOP:
                    if ( cPlayback.GetPlayMode() ) HandOffMsg( msg, wParam, lParam );
                    return 0;
                case ID_PLAY_SKIPFWD: case ID_PLAY_SKIPBACK:
                    if ( cPlayback.GetPlayMode() ) HandOffMsg( msg, wParam, lParam );
                    return 0;
                case ID_PLAY_INCREASERATE:
                    cPlayback.SetSpeed( cPlayback.GetSpeed() * ( 1.0 + cControls.dSpeedUpPct / 100.0 ), true );
                    return 0;
                case ID_PLAY_DECREASERATE:
                    cPlayback.SetSpeed( cPlayback.GetSpeed() / ( 1.0 + cControls.dSpeedUpPct / 100.0 ), true );
                    return 0;
                case ID_PLAY_RESETRATE:
                    cPlayback.SetSpeed( 1.0, true );
                    return 0;
                case ID_PLAY_NFASTER:
                    cPlayback.SetNSpeed( cPlayback.GetNSpeed() / ( 1.0 + cControls.dSpeedUpPct / 100.0 ), true );
                    return 0;
                case ID_PLAY_NSLOWER:
                    cPlayback.SetNSpeed( cPlayback.GetNSpeed() * ( 1.0 + cControls.dSpeedUpPct / 100.0 ), true );
                    return 0;
                case ID_PLAY_NRESET:
                    cPlayback.SetNSpeed( 1.0, true );
                    return 0;
                case ID_PLAY_VOLUMEUP:
                    cPlayback.SetVolume( min( cPlayback.GetVolume() + 0.1, 1.0 ), true );
                    return 0;
                case ID_PLAY_VOLUMEDOWN:
                    cPlayback.SetVolume( max( cPlayback.GetVolume() - 0.1, 0.0 ), true );
                    return 0;
                case ID_PLAY_MUTE:
                    cPlayback.ToggleMute( true );
                    return 0;
                case ID_VIEW_CONTROLS:
                    cView.ToggleControls( true );
                    return 0;
                case ID_VIEW_KEYBOARD:
                    cView.ToggleKeyboard( true );
                    return 0;
                case ID_VIEW_ALWAYSONTOP:
                    cView.ToggleOnTop( true );
                    return 0;
                case ID_VIEW_FULLSCREEN:
                    cView.ToggleFullScreen( true );
                    return 0;
                case ID_VIEW_MOVEANDZOOM:
                    HandOffMsg( msg, wParam, lParam );
                    return 0;
                case ID_VIEW_RESETMOVEANDZOOM:
                    HandOffMsg( msg, wParam, lParam );
                    return 0;
                case ID_VIEW_SETWINDOWSIZE:
                    return 0;
                case ID_VIEW_NOFULLSCREEN:
                    if ( cView.GetZoomMove() ) HandOffMsg( msg, ID_VIEW_CANCELMOVEANDZOOM, lParam );
                    else if ( cView.GetFullScreen() ) cView.SetFullScreen( false, true );
                    return 0;
                case ID_OPTIONS_PREFERENCES:
                    CheckActivity( TRUE );
                    return 0;
                case ID_HELP_ABOUT:
                    return 0;
                case ID_GAMEERROR:
                    MessageBoxW( hWnd, GameState::Errors[lParam].c_str(), L"Error", MB_OK | MB_ICONEXCLAMATION );
                    return 0;
                case ID_UPDATE:
                    ShellExecute(NULL, L"open", L"https://github.com/khang06/PianoFromAbove/releases", NULL, NULL, SW_SHOWNORMAL);
                    return 0;
            }
            break;
        }
        case WM_ACTIVATE:
            if ( LOWORD( wParam ) != WA_INACTIVE )
                SetFocus( g_hWndGfx );
            return  0;
        case WM_SYSCOMMAND:
        {
            if ( wParam == SC_SCREENSAVE || wParam == SC_MONITORPOWER )
            {
                if ( cPlayback.GetPlayMode() && !cPlayback.GetPaused() )
                    return 0;
            }
            // Maximize/restore (titlebar buttons) animate BEFORE WM_SIZE arrives.
            // Freeze rendering and drain the GPU queue while the window is still
            // full-size, so no flip gets presented into the animating window:
            // such flips stall on NVIDIA and the reset's fence wait TDRs.
            int iCmd = wParam & 0xFFF0;
            if (iCmd == SC_MAXIMIZE || iCmd == SC_RESTORE)
            {
                g_bInSizeMove = true;
                g_bSysResize = true;
                HANDLE hDrain = CreateEvent(NULL, TRUE, FALSE, NULL);
                if (hDrain)
                {
                    MSG m = {};
                    m.message = WM_COMMAND;
                    m.wParam = ID_PRELOAD_DRAIN;
                    m.lParam = (LPARAM)hDrain;
                    g_MsgQueue.Push(m);
                    WaitForSingleObject(hDrain, 1500);
                    CloseHandle(hDrain);
                }
            }
            break;
        }
        case WM_GETMINMAXINFO:
        {
            LPMINMAXINFO lpmmi = ( LPMINMAXINFO )lParam;
            lpmmi->ptMinTrackSize.x = MINWIDTH;
            lpmmi->ptMinTrackSize.y = MINHEIGHT;
            lpmmi->ptMaxTrackSize.x = 65535;
            lpmmi->ptMaxTrackSize.y = 65535;
            return 0;
        }
        case WM_SIZE:
            if ( wParam == SIZE_MINIMIZED ) return 0;
            SizeWindows( LOWORD( lParam ), HIWORD( lParam ) );

            if ( wParam != SIZE_MAXIMIZED && !cView.GetFullScreen() )
            {
                RECT rcMain;
                GetWindowRect( hWnd, &rcMain );
                cView.SetMainSize( rcMain.right - rcMain.left, rcMain.bottom - rcMain.top );
            }
            if ( !g_bInSizeMove || g_bSysResize )
            {
                g_bResetPending = true;
                HandOffMsg( WM_COMMAND, ID_VIEW_RESETDEVICE, 0 );
            }
            return 0;
        case WM_MOVE:
        {
            RECT rcMain;
            GetWindowRect( hWnd, &rcMain );
            cView.SetMainPos( rcMain.left, rcMain.top );
            return 0;
        }
        case WM_ENTERSIZEMOVE:
            g_bInSizeMove = true;
            return 0;
        case WM_EXITSIZEMOVE:
            g_bResetPending = true;
            HandOffMsg( WM_COMMAND, ID_VIEW_RESETDEVICE, 0 );
            g_bInSizeMove = false;
            return 0;
        case WM_DEVICECHANGE:
            Sleep( 200 );
            Config::GetConfig().LoadMIDIDevices();
            HandOffMsg( WM_DEVICECHANGE, 0, 0 );
            break;
        case WM_DESTROY:
            PostQuitMessage( 0 );
            return 0;
        case WM_DROPFILES:
            if (!wParam)
                return 0;
            auto drop = (HDROP)wParam;
            if (DragQueryFile(drop, 0xFFFFFFFF, NULL, 0) != 1)
                return 0;
            std::vector<wchar_t> filename;
            filename.resize(DragQueryFile(drop, 0, NULL, 0) + 1);
            DragQueryFile(drop, 0, filename.data(), filename.size());
            PlayFile(filename.data(), true);
            return 0;
    }

    return DefWindowProc( hWnd, msg, wParam, lParam );
}



VOID SizeWindows( int iMainWidth, int iMainHeight )
{
    int iLibWidth = 0;
    UINT swpFlags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER;

    if ( !iMainWidth || !iMainHeight )
    {
        RECT rcMain;
        GetClientRect( g_hWnd, &rcMain );
        iMainWidth = rcMain.right;
        iMainHeight = rcMain.bottom;
    }

    HDWP hdwp = BeginDeferWindowPos( 1 );
    if ( hdwp ) hdwp = DeferWindowPos( hdwp, g_hWndGfx, NULL, iLibWidth, 0, iMainWidth, iMainHeight, swpFlags );
    if ( hdwp ) EndDeferWindowPos( hdwp );
}

LRESULT WINAPI GfxProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    static const ViewSettings &cView = Config::GetConfig().GetViewSettings();
    static const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
    static bool bShowBar;
    static int iBarHeight;

    static bool bTrackL = false, bTrackR = false;

    // ImGui context and Win32 backend asynchronously on the game thread;
    if (ImGui::GetCurrentContext() != nullptr &&
        ImGui::GetIO().BackendPlatformUserData != nullptr &&
        ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    if ( ( msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST ) || ( msg >= WM_KEYFIRST && msg <= WM_KEYLAST ) ||
         msg == WM_CAPTURECHANGED || msg == WM_MOUSELEAVE )
        HandOffMsg( msg, wParam, lParam );

    switch (msg)
    {
        case WM_CREATE:
            ShowKeyboard( cView.GetKeyboard() );
            return 0;
        case WM_CLOSE:
            // Closing the standalone renderer window (it is only detached from
            // the player window while a render runs) cancels the render and
            // brings the player window back.
            if ( g_bVideoRendering )
            {
                HandOffMsg( WM_COMMAND, ID_FILE_STOPRENDER, 0 );
                return 0;
            }
            break;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
            SetFocus( hWnd );
            if ( !bTrackR && !bTrackL ) SetCapture( hWnd );
            if ( msg == WM_LBUTTONDOWN ) bTrackL = true;
            else bTrackR = true;
            return 0;
        case WM_LBUTTONUP: case WM_RBUTTONUP:
            if ( msg == WM_LBUTTONUP ) bTrackL = false;
            else bTrackR = false;
            if ( !bTrackR && !bTrackL ) ReleaseCapture();
            if ( cView.GetZoomMove()  ) return 0;
            break;
        case WM_CAPTURECHANGED:
            bTrackR = bTrackL = false;
            return 0;
        case WM_DESTROY:
            g_bGfxDestroyed = true;
            return 0;
    }

    return DefWindowProc( hWnd, msg, wParam, lParam );
}






VOID HandOffMsg( UINT msg, WPARAM wParam, LPARAM lParam )
{
    MSG msgGameThread = { g_hWndGfx, msg, wParam, lParam, 0, {0, 0} };
    g_MsgQueue.ForcePush( msgGameThread );
}

VOID ShowControls( BOOL )
{
    g_bResetPending = true;
    HandOffMsg( WM_COMMAND, ID_VIEW_RESETDEVICE, 0 );
}

VOID ShowKeyboard( BOOL ) {}

VOID SetOnTop( BOOL bOnTop )
{
    static const ViewSettings &cView = Config::GetConfig().GetViewSettings();
    if ( !cView.GetFullScreen() )
        SetWindowPos( g_hWnd, bOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE );
}

VOID SetFullScreen( BOOL bFullScreen )
{
    static const ViewSettings &cView = Config::GetConfig().GetViewSettings();
    static RECT rcOld = {};

    if ( bFullScreen )
    {
        RECT rcDesktop;
        GetWindowRect( g_hWnd, &rcOld );
        GetWindowRect( GetDesktopWindow(), &rcDesktop );

        SetWindowLongPtr( g_hWnd, GWL_STYLE, GetWindowLongPtr( g_hWnd, GWL_STYLE ) & ~WS_CAPTION & ~WS_THICKFRAME );
        SetWindowPos( g_hWnd, HWND_TOPMOST, rcDesktop.left, rcDesktop.top,
                      rcDesktop.right - rcDesktop.left, rcDesktop.bottom - rcDesktop.top,
                      SWP_NOACTIVATE | SWP_FRAMECHANGED );
        g_bResetPending = true;
        HandOffMsg( WM_COMMAND, ID_VIEW_RESETDEVICE, 0 );
    }
    else
    {
        SetWindowLongPtr( g_hWnd, GWL_STYLE, GetWindowLongPtr( g_hWnd, GWL_STYLE ) | WS_CAPTION | WS_THICKFRAME );
        SetWindowPos( g_hWnd, cView.GetOnTop() ? HWND_TOPMOST : HWND_NOTOPMOST, rcOld.left, rcOld.top,
                      rcOld.right - rcOld.left, rcOld.bottom - rcOld.top,
                      SWP_NOACTIVATE | SWP_FRAMECHANGED );
        g_bResetPending = true;
        HandOffMsg( WM_COMMAND, ID_VIEW_RESETDEVICE, 0 );
    }
}

VOID SetZoomMove( BOOL )
{
}

VOID SetMute( BOOL ) {}
VOID SetSpeed( DOUBLE ) {}
VOID SetNSpeed( DOUBLE ) {}
VOID SetVolume( DOUBLE ) {}
VOID SetPosition( INT ) {}
VOID SetPlayable( BOOL ) {}
VOID SetPlayMode( INT ePlayMode )
{
    SetZoomMove( FALSE );
}
VOID SetPlayPauseStop( BOOL, BOOL, BOOL ) {}

BOOL PlayFile( const wstring &sFile, bool bCustomSettings )
{
    Config &config = Config::GetConfig();
    const VisualSettings &cVisual = config.GetVisualSettings();
    PlaybackSettings &cPlayback = config.GetPlaybackSettings();
    ViewSettings &cView = config.GetViewSettings();

    const GameState::State ePlayMode = GameState::Practice;

    g_LoadingProgress.stage = MIDILoadingProgress::Stage::CopyToMem;
    g_LoadingProgress.progress = 0;
    g_LoadingProgress.max = 0;
    g_bShowLoading = true;
    // Free the previous song BEFORE parsing the new one: the old state's song
    // data (merged events + SoA pools) otherwise stays resident through the
    // whole load, and the combined peak TDRs the driver on memory-tight
    // machines. The state is owned by the game thread, so hand *it* the
    // discard job and wait for the event it signals.
    if (g_pGameState != NULL)
    {
        HANDLE hDiscardDone = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (hDiscardDone)
        {
            MSG m = {};
            m.message = WM_COMMAND;
            m.wParam = ID_PRELOAD_DISCARD;
            m.lParam = (LPARAM)hDiscardDone;
            g_MsgQueue.Push(m);
            WaitForSingleObject(hDiscardDone, 30000);
            CloseHandle(hDiscardDone);
            PRE_DbgLog("PlayFile: old state discarded (bCustomSettings=%d)", (int)bCustomSettings);
        }
    }
    MainScreen* pGameState = new MainScreen(sFile, ePlayMode, NULL, NULL);
    if (!pGameState->IsValid())
    {
        g_bShowLoading = false;
        MessageBox(g_hWnd, (L"Was not able to load " + sFile).c_str(), TEXT("Error"), MB_OK | MB_ICONEXCLAMATION);
        delete pGameState;
        return FALSE;
    }

    if ( bCustomSettings )
    {
        if (!GetCustomSettings(pGameState))
        {
            PRE_DbgLog("PlayFile: custom dialog cancelled");
            g_bShowLoading = false;
            delete pGameState;
            return FALSE;
        }
        PRE_DbgLog("PlayFile: custom dialog ok useCustom=%d audioPath='%ls'", (int)pGameState->m_bUseCustomAudio, pGameState->m_sCustomAudioPath.c_str());
    }
    else
    {
        pGameState->SetChannelSettings(
            vector< bool >(),
            vector< bool >(),
            vector< unsigned >( cVisual.colors, cVisual.colors + sizeof( cVisual.colors ) / sizeof( cVisual.colors[0] ) ) );
    }

    if ( !cPlayback.GetPlayable() ) cPlayback.SetPlayable( true, true );
    if ( cPlayback.GetPlayMode() != ePlayMode ) cPlayback.SetPlayMode( ePlayMode, true );
    cPlayback.SetPaused( ePlayMode != GameState::Practice, true );
    cPlayback.SetPosition( 0 );
    cView.SetZoomMove( false, true );
    SetMainTitle( sFile.c_str() + ( sFile.find_last_of( L'\\' ) + 1 ) );

    HandOffMsg( WM_COMMAND, ID_CHANGESTATE, ( LPARAM )pGameState );
    return TRUE;
}

VOID CheckActivity( BOOL bIsActive, POINT *ptNew, BOOL bToggleEnable )
{
    static const ViewSettings &cView = Config::GetConfig().GetViewSettings();
    static const VisualSettings &cVisual = Config::GetConfig().GetVisualSettings();
    static bool bEnabled = true;
    static bool bWasActive = true;
    static bool bMouseHidden = false;
    static POINT ptOld;

    if ( !bEnabled && !bToggleEnable ) return;
    if ( bToggleEnable ) bEnabled = !bEnabled;

    bool bSamePt;
    if ( ptNew )
    {
        bSamePt = ( ptNew->x == ptOld.x && ptNew->y == ptOld.y );
        ptOld = *ptNew;
    }
    else
    {
        POINT pt;
        GetCursorPos( &pt );
        bSamePt = ( pt.x == ptOld.x && pt.y == ptOld.y );
        ptOld = pt;
    }

    if ( ( bIsActive && !ptNew ) || !bSamePt || !cView.GetFullScreen() )
    {
        bWasActive = true;
        if ( bMouseHidden ) bMouseHidden = ( ShowCursor( TRUE ) < 0 );
    }
    else if ( !bIsActive && GetFocus() == g_hWndGfx  && cVisual.bAlwaysShowControls )
    {
        if ( bWasActive )
            bWasActive = false;
        else if ( !bMouseHidden )
            bMouseHidden = ( ShowCursor( FALSE ) < 0 );
    }
}