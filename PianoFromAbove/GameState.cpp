/*************************************************************************************************
*
* File: GameState.cpp
*
* Description: Implements the game states and objects rendered into the graphics window
*              Contains the core game logic (IntroScreen, SplashScreen, MainScreen objects)
*
* Copyright (c) 2010 Brian Pantano. All rights reserved.
*
*************************************************************************************************/
#include <algorithm>
#include <tchar.h>
#include <ppl.h>
#include <dwmapi.h>
#include <fstream>
#include <pdh.h>
#include <thread>
#include <atomic>

#include "Globals.h"
#include "GameState.h"
#include "Config.h"
#include "resource.h"
#include "ConfigProcs.h"        
#include "MainProcs.h"          
#include "MIDIPreRenderPlayer.h"
#include <d3d9types.h>

// Uploads the (possibly changed) track settings colors into the renderer's
// GPU-side color buffer. Must run every frame in every screen that renders
// notes, since the renderer only re-uploads the marked tracks.
static void SyncTrackColors(Renderer* pRenderer, const vector<TrackSettings>& vTrackSettings)
{
    auto* track_colors = pRenderer->GetTrackColors();
    for (size_t i = 0; i < min(vTrackSettings.size(), MaxTrackColors); i++) {
        bool bChanged = false;
        for (size_t j = 0; j < 16; j++) {
            auto& src = vTrackSettings[i].aChannels[j];
            auto& dst = track_colors[i * 16 + j];
            TrackColor c = { src.iPrimaryRGB, src.iDarkRGB, src.bHidden ? 0xFFFFFFFF : src.iVeryDarkRGB }; // Hack to signal hidden track without checking on CPU
            if (dst.primary != c.primary || dst.dark != c.dark || dst.darker != c.darker) {
                dst = c;
                bChanged = true;
            }
        }
        if (bChanged)
            pRenderer->MarkTrackColorsDirty(i);
    }
}

// ---- video render (FFmpeg capture) -----------------------------------------
bool g_bVideoRendering = false;

// Resolution the raw stream was started with (the capture aborts cleanly if
// the window is resized mid-render, which would corrupt the stream).
static int s_FFCapW = 0, s_FFCapH = 0;

// Runs an ffmpeg command to completion (used for the final mux pass).
static void RunFFmpegSync(wchar_t* sCmd)
{
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(NULL, sCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

bool RequestVideoRender()
{
    if (!g_pGameState)
        return false;
    MainScreen* ms = dynamic_cast<MainScreen*>(g_pGameState);
    if (!ms)
        return false;
    ms->StartVideoRender();
    return true;
}

void StopVideoRender()
{
    if (!g_pGameState)
        return;
    MainScreen* ms = dynamic_cast<MainScreen*>(g_pGameState);
    if (ms)
        ms->FinishVideoRender();
}

bool VideoRenderSongLoaded()
{
    if (!g_pGameState)
        return false;
    MainScreen* ms = dynamic_cast<MainScreen*>(g_pGameState);
    if (!ms)
        return false;
    return !ms->IsFreePlay() && ms->IsValid();
}

void MainScreen::StartVideoRender()
{
    if (m_bRenderVideo)
        return;
    if (IsFreePlay() || !m_MIDI.IsValid())
    {
        MessageBox(g_hWnd, TEXT("Load a song first - the video render captures song playback."), TEXT("Render Video"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    Config& config = Config::GetConfig();
    VizSettings& viz = config.GetVizSettings();

    if (m_bUseCustomAudio && !m_sCustomAudioPath.empty())
    {
        MessageBox(g_hWnd, TEXT("Custom audio is not supported by the video render.\nDisable custom audio in the Playback menu first."), TEXT("Render Video"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (viz.bDumpFrames)
    {
        MessageBox(g_hWnd, TEXT("Turn off Dump Frames in the Viz tab first."), TEXT("Render Video"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!config.GetAudioSettings().bPreRenderAudio)
    {
        MessageBox(g_hWnd, TEXT("Enable Pre-rendered Audio in Settings -> Audio first.\nThe video render records the prerendered audio track."), TEXT("Render Video"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (viz.sFFmpegDir.empty())
    {
        MessageBox(g_hWnd, TEXT("Set the FFmpeg folder (the one containing ffmpeg.exe) first."), TEXT("Render Video"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring sFFmpeg = viz.sFFmpegDir + L"\\ffmpeg.exe";
    if (GetFileAttributesW(sFFmpeg.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        MessageBox(g_hWnd, TEXT("ffmpeg.exe was not found in the FFmpeg folder."), TEXT("Render Video"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Capture starts immediately at the renderer's current resolution; ffmpeg
    // scales the stream to the requested output resolution on the fly.
    BeginVideoRender();
}

void MainScreen::BeginVideoRender()
{
    Config& config = Config::GetConfig();
    VizSettings& viz = config.GetVizSettings();

    // Output file names: the user's path (with the chosen container extension,
    // like Comet's replace_extension) or a default next to the executable.
    std::wstring sName = m_MIDI.GetInfo().sFilename;
    size_t slash = sName.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        sName = sName.substr(slash + 1);
    size_t dot = sName.find_last_of(L'.');
    if (dot != std::wstring::npos)
        sName = sName.substr(0, dot);
    wchar_t cwd[MAX_PATH] = {};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    m_sFFWav = std::wstring(cwd) + L"\\Render_" + sName + L"_audio.wav";
    m_sFFVideoRaw = std::wstring(cwd) + L"\\Render_" + sName + L"_video.mp4";
    const std::wstring sExt = viz.iRenderFormat == 1 ? L"mov" : viz.iRenderFormat == 2 ? L"avi" : L"mp4";
    m_sFFVideoOut = viz.sRenderOutputPath;
    if (m_sFFVideoOut.empty())
        m_sFFVideoOut = std::wstring(cwd) + L"\\Render_" + sName + L"." + sExt;
    else
    {
        size_t dot2 = m_sFFVideoOut.find_last_of(L'.');
        size_t slash2 = m_sFFVideoOut.find_last_of(L"\\/");
        if (dot2 != std::wstring::npos && (slash2 == std::wstring::npos || dot2 > slash2))
            m_sFFVideoOut = m_sFFVideoOut.substr(0, dot2);
        m_sFFVideoOut += L"." + sExt;
    }

    // Capture at the renderer's current resolution; the output resolution
    // (and fps, which is also the capture rate) come from the render dialog.
    int w = (int)m_pRenderer->GetBufferWidth(), h = (int)m_pRenderer->GetBufferHeight();
    s_FFCapW = w;
    s_FFCapH = h;

    // Encoder settings: codec, x264/x265 preset, constant bitrate or CRF, and
    // the user's extra ffmpeg options (mirrors Comet's FFmpegCommandBuilder).
    static const char* sPresets[] = { "ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", "placebo" };
    std::string sEnc = std::string("-c:v ") + (viz.iRenderCodec == 1 ? "libx265" : "libx264") + " -preset " + sPresets[max(0, min(viz.iRenderPreset, 9))] + " ";
    char sNum[128];
    if (viz.iRenderBitrateMode == 0)
    {
        sprintf_s(sNum, "-b:v %dk -minrate %dk -maxrate %dk -bufsize %dk ",
            viz.iRenderBitrateKbps, viz.iRenderBitrateKbps, viz.iRenderBitrateKbps, viz.iRenderBitrateKbps * 2);
        sEnc += sNum;
    }
    else
    {
        sprintf_s(sNum, "-crf %d ", max(0, min(viz.iRenderCRF, 51)));
        sEnc += sNum;
    }
    if (viz.bRenderAdvanced && !viz.sRenderAdvancedOptions.empty())
        sEnc += std::string(Util::WstringToString(viz.sRenderAdvancedOptions)) + " ";
    if (w != viz.iRenderWidth || h != viz.iRenderHeight)
    {
        sprintf_s(sNum, "-vf scale=%d:%d ", viz.iRenderWidth, viz.iRenderHeight);
        sEnc += sNum;
    }
    sEnc += "-pix_fmt yuv420p";

    // Spawn ffmpeg: raw BGRA frames over stdin -> video-only intermediate.
    // The Framerate setting is the video's output fps: each captured frame is
    // one video frame at that rate, so the video length matches the song. The
    // render itself is uncapped (limit FPS off) and runs as fast as the
    // machine can; the window title shows the achieved render speed.
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hPipeRead = NULL, hPipeWrite = NULL;
    if (!CreatePipe(&hPipeRead, &hPipeWrite, &sa, 0))
    {
        MessageBox(g_hWnd, TEXT("Could not create the capture pipe."), TEXT("Render Video"), MB_OK | MB_ICONERROR);
        return;
    }
    SetHandleInformation(hPipeWrite, HANDLE_FLAG_INHERIT, 0);
    wchar_t sCmd[4096];
    swprintf_s(sCmd, L"\"%ls\\ffmpeg.exe\" -y -f rawvideo -pix_fmt bgra -s %dx%d -r %d -i pipe:0 -an %hs -r %d \"%ls\"",
        viz.sFFmpegDir.c_str(), w, h, viz.iRenderFPS, sEnc.c_str(), viz.iRenderFPS, m_sFFVideoRaw.c_str());
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hPipeRead;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(NULL, sCmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        CloseHandle(hPipeRead);
        CloseHandle(hPipeWrite);
        MessageBox(g_hWnd, TEXT("Could not start ffmpeg.exe."), TEXT("Render Video"), MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(hPipeRead);
    m_hFFPipeWrite = hPipeWrite;
    m_hFFProc = pi.hProcess;
    CloseHandle(pi.hThread);

    // Show only the renderer window while rendering: detach the gfx window
    // from the (hidden) player window and center it on the screen. Closing
    // the renderer window cancels the render and brings the player window back.
    SetParent(g_hWndGfx, NULL);
    SetWindowLongPtrA(g_hWndGfx, GWL_STYLE, (GetWindowLongPtrA(g_hWndGfx, GWL_STYLE) & ~WS_CHILD) | WS_POPUP);
    {
        RECT rcWork = {};
        SystemParametersInfo(SPI_GETWORKAREA, 0, &rcWork, 0);
        int iX = rcWork.left + max(0, ((rcWork.right - rcWork.left) - w) / 2);
        int iY = rcWork.top + max(0, ((rcWork.bottom - rcWork.top) - h) / 2);
        SetWindowPos(g_hWndGfx, HWND_TOP, iX, iY, w, h, SWP_SHOWWINDOW);
    }
    ShowWindow(g_hWnd, SW_HIDE);
    SetWindowText(g_hWndGfx, L"Render to Video");

    // Restart the prerender with WAV recording, rewind the song, and unpause.
    // The song clock advances one output frame per rendered frame, while the
    // real frame rate stays uncapped (limit FPS off) so the render runs as
    // fast as the machine can; the window title shows the achieved speed.
    if (PRE_MIDIAudio)
        PRE_MIDIAudio->StartWavRecording(m_sFFWav.c_str());
    m_bAudioStarted = false;
    JumpTo(GetMinTime());
    config.GetPlaybackSettings().SetPaused(false, true);
    m_Timer.Init(true);
    m_Timer.SetFrameRate(viz.iRenderFPS);

    m_bRenderVideo = true;
    g_bVideoRendering = true;
    PRE_DbgLog("RENDER: begin %dx%d fps=%d raw=%ls wav=%ls", w, h, viz.iRenderFPS, m_sFFVideoRaw.c_str(), m_sFFWav.c_str());

    // Strip the UI for the video: close the ImGui render dialog and any other
    // ImGui windows (the captured output must stay clean), and pop up the
    // plain Win32 progress window (progress, size, render speed).
    if (m_pRenderer)
        m_pRenderer->m_bShowRenderDialog = m_pRenderer->m_bShowPreferences = m_pRenderer->m_bShowAbout = false;
    RequestCreateRenderProgressWindow();
}

void MainScreen::FinishVideoRender()
{
    if (!m_bRenderVideo)
        return;
    m_bRenderVideo = false;
    g_bVideoRendering = false;
    s_FFCapW = s_FFCapH = 0;

    // Close the Win32 progress window and restore the ImGui UI.
    RequestDestroyRenderProgressWindow();

    // Restore the app's normal timer mode (the render forced the 60 fps
    // manual timer; leaving it manual would lock the FPS limiter on).
    m_Timer.Init(Config::GetConfig().m_bManualTimer);

    PRE_DbgLog("RENDER: finalizing");
    if (m_hFFPipeWrite)
    {
        CloseHandle(m_hFFPipeWrite);
        m_hFFPipeWrite = NULL;
    }
    if (m_hFFProc)
    {
        WaitForSingleObject(m_hFFProc, 60000);
        CloseHandle(m_hFFProc);
        m_hFFProc = NULL;
    }

    // Bring the player window back: reattach the renderer window as a child
    // and refit it to the main window's client area.
    ShowWindow(g_hWnd, SW_SHOW);
    SetParent(g_hWndGfx, g_hWnd);
    SetWindowLongPtrA(g_hWndGfx, GWL_STYLE, (GetWindowLongPtrA(g_hWndGfx, GWL_STYLE) & ~WS_POPUP) | WS_CHILD);
    {
        RECT rcClient;
        GetClientRect(g_hWnd, &rcClient);
        SetWindowPos(g_hWndGfx, NULL, 0, 0, rcClient.right, rcClient.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindow(g_hWndGfx, SW_SHOW);
    SetFocus(g_hWndGfx);

    if (PRE_MIDIAudio)
        PRE_MIDIAudio->Stop();
    if (PRE_MIDIAudio)
        PRE_MIDIAudio->StopWavRecording();

    // Mux the raw video with the recorded WAV (unless audio is disabled),
    // padding the audio with silence if it came up short, and remux into the
    // chosen container at the user's output path.
    Config& config = Config::GetConfig();
    VizSettings& viz = config.GetVizSettings();
    const wchar_t* sFmt = viz.iRenderFormat == 1 ? L"mov" : viz.iRenderFormat == 2 ? L"avi" : L"mp4";
    const wchar_t* sFast = viz.iRenderFormat == 2 ? L"" : L" -movflags +faststart";
    wchar_t sCmd[4096];
    if (viz.bRenderIncludeAudio)
        swprintf_s(sCmd, L"\"%ls\\ffmpeg.exe\" -y -i \"%ls\" -i \"%ls\" -filter:a apad -c:v copy -c:a aac -b:a 192k -shortest%s -f %ls \"%ls\"",
            viz.sFFmpegDir.c_str(), m_sFFVideoRaw.c_str(), m_sFFWav.c_str(), sFast, sFmt, m_sFFVideoOut.c_str());
    else
        swprintf_s(sCmd, L"\"%ls\\ffmpeg.exe\" -y -i \"%ls\" -c:v copy -f %ls \"%ls\"",
            viz.sFFmpegDir.c_str(), m_sFFVideoRaw.c_str(), sFmt, m_sFFVideoOut.c_str());
    RunFFmpegSync(sCmd);

    PRE_DbgLog("RENDER: done out=%ls", m_sFFVideoOut.c_str());
    DeleteFileW(m_sFFVideoRaw.c_str());
    DeleteFileW(m_sFFWav.c_str());

    const std::wstring& name = m_MIDI.GetInfo().sFilename;
    TCHAR sTitle[1024];
    _stprintf_s(sTitle, TEXT("%ws"), name.c_str() + (name.find_last_of(L'\\') + 1));
    SetMainTitle(sTitle);

    std::wstring sMsg = L"Render complete:\n" + m_sFFVideoOut;
    MessageBox(g_hWnd, sMsg.c_str(), TEXT("Render Video"), MB_OK | MB_ICONINFORMATION);
    PRE_DbgLog("RENDER: done %ls", m_sFFVideoOut.c_str());
}

// ---- system stats sampling (CPU / RAM / VRAM / GPU) -------------------------
// CPU/RAM/VRAM are sampled at 1 Hz on the game thread (cheap Win32 calls). GPU
// usage is sampled on a DEDICATED thread: the PDH "GPU Engine" counters are
// maintained by the display driver and are known to hang or fail the process -
// they must never run on the render thread.
static ULONGLONG s_ullLastKernel = 0, s_ullLastUser = 0, s_ullLastIdle = 0;
static bool s_bCpuBase = false;
static double s_dCpuPct = 0.0;
static MEMORYSTATUSEX s_memEx = {};
static DWORDLONG s_ullVramUsed = 0, s_ullVramTotal = 0;
static LONGLONG s_llLastSysSample = 0;

static std::atomic<float> s_fGpuPct(0.0f);
static std::atomic<bool> s_bGpuAvail(false);

/// Transition-mode zoom state (MainScreen::RenderGlobals). File scope so the
// stop button can reset the zoom back to the base view. The keyboard glides
// toward 128-key range when out-of-range notes appear and stays there
// permanently until the song is stopped/reset.
static float s_fTransitionPhase = 0.0f;
static std::chrono::steady_clock::time_point s_LastTransitionFrame = std::chrono::steady_clock::now();

static void GpuPdhThreadMain()
{
    PDH_HQUERY hQuery = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &hQuery) != ERROR_SUCCESS)
        return;
    PDH_HCOUNTER hCounter = nullptr;
    if (PdhAddCounterW(hQuery, L"\\GPU Engine(*)\\Utilization Percentage", 0, &hCounter) != ERROR_SUCCESS)
    {
        PdhCloseQuery(hQuery);
        return;
    }
    bool bPrimed = false;
    for (;;)
    {
        Sleep(1000);
        try
        {
            PDH_STATUS st = PdhCollectQueryData(hQuery);
            if (st == ERROR_SUCCESS && bPrimed)
            {
                DWORD nSize = 0, nCount = 0;
                PdhGetFormattedCounterArrayA(hCounter, PDH_FMT_DOUBLE, &nSize, &nCount, nullptr);
                if (nSize > 0 && nCount > 0 && nSize < 4 * 1024 * 1024)
                {
                    std::vector<BYTE> buf(nSize + sizeof(PDH_FMT_COUNTERVALUE_ITEM_A) * 16);
                    PdhGetFormattedCounterArrayA(hCounter, PDH_FMT_DOUBLE, &nSize, &nCount, (PPDH_FMT_COUNTERVALUE_ITEM_A)buf.data());
                    size_t capacity = (buf.size() - sizeof(PDH_FMT_COUNTERVALUE_ITEM_A) * 16) / sizeof(PDH_FMT_COUNTERVALUE_ITEM_A);
                    size_t items = min((size_t)nCount, capacity);
                    const PDH_FMT_COUNTERVALUE_ITEM_A* arr = (const PDH_FMT_COUNTERVALUE_ITEM_A*)buf.data();
                    double dSum = 0.0;
                    char pidTag[32] = {};
                    snprintf(pidTag, sizeof(pidTag) - 1, "pid_%lu", GetCurrentProcessId());
                    for (size_t i = 0; i < items; i++)
                    {
                        if (arr[i].szName && strstr(arr[i].szName, pidTag))
                            dSum += arr[i].FmtValue.doubleValue;
                    }
                    s_fGpuPct.store((float)min(dSum, 100.0));
                    s_bGpuAvail.store(true);
                }
            }
            else if (st == ERROR_SUCCESS)
                bPrimed = true;
        }
        catch (...)
        {
        }
    }
}

static void StartGpuPdhThread()
{
    static std::atomic<bool> s_bStarted(false);
    if (!s_bStarted.exchange(true))
        std::thread(GpuPdhThreadMain).detach();
}

static void UpdateSysStats(Renderer* pRenderer)
{
    LARGE_INTEGER pc = {};
    QueryPerformanceCounter(&pc);
    if (pc.QuadPart - s_llLastSysSample < 10000000)
        return; // once per second
    s_llLastSysSample = pc.QuadPart;

    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user))
    {
        ULONGLONG i = ((ULONGLONG)idle.dwHighDateTime << 32) | idle.dwLowDateTime;
        ULONGLONG k = ((ULONGLONG)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
        ULONGLONG u = ((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime;
        if (s_bCpuBase)
        {
            ULONGLONG dk = k - s_ullLastKernel, du = u - s_ullLastUser, di = i - s_ullLastIdle;
            ULONGLONG total = dk + du;
            if (total > 0)
                s_dCpuPct = 100.0 * (double)(total - di) / (double)total;
        }
        s_ullLastKernel = k; s_ullLastUser = u; s_ullLastIdle = i; s_bCpuBase = true;
    }

    s_memEx.dwLength = sizeof(s_memEx);
    GlobalMemoryStatusEx(&s_memEx);

    if (pRenderer)
        pRenderer->GetAdapterVideoMemory(s_ullVramUsed, s_ullVramTotal);

    StartGpuPdhThread();
}

const wstring GameState::Errors[] =
{
    L"Success.",
    L"Invalid pointer passed. It would be nice if you could submit feedback with a description of how this happened.",
    L"Out of memory. This is a problem",
    L"Error calling DirectX. It would be nice if you could submit feedback with a description of how this happened.",
};


static uint64_t CorruptSeed(MIDIChannelEvent pNote, MIDIChannelEvent pSister)
{
    uint64_t h = min((uint64_t)pNote, (uint64_t)pSister);
    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ull; h ^= h >> 27; h *= 0x94d049bb133111ebull; h ^= h >> 31;
    return h;
}

// Corrupted pitch for a note; identical for both events of a note on/off pair
static int CorruptPitch(float fCorrupt, uint64_t h, int iNote)
{
    if (fCorrupt <= 0.0f)
        return iNote;
    float r = (float)(h & 0xFFFFF) * (1.0f / 1048576.0f);
    iNote += (int)llroundf((r * 2.0f - 1.0f) * 60.0f * fCorrupt);
    return min(max(iNote, 0), 127);
}

static int CorruptVelocity(float fCorrupt, uint64_t h, int iVelocity)
{
    if (fCorrupt <= 0.0f)
        return iVelocity;
    float r = (float)((h >> 20) & 0xFFFFF) * (1.0f / 1048576.0f);
    iVelocity = (int)(iVelocity * (1.0 + (r * 2.0f - 1.0f) * 0.75 * fCorrupt) + 0.5);
    return min(max(iVelocity, 1), 127);
}

static int CorruptValue(float fCorrupt, uint64_t h, int iValue)
{
    if (fCorrupt <= 0.0f)
        return iValue;
    float r = (float)((h >> 40) & 0xFFFFF) * (1.0f / 1048576.0f);
    iValue += (int)llroundf((r * 2.0f - 1.0f) * 127.0f * fCorrupt);
    return min(max(iValue, 0), 127);
}

static int CorruptPitchBend(float fCorrupt, uint64_t h, int iBend)
{
    if (fCorrupt <= 0.0f)
        return iBend;
    float r = (float)(h & 0xFFFFF) * (1.0f / 1048576.0f);
    iBend += (int)llroundf((r * 2.0f - 1.0f) * 8192.0f * fCorrupt);
    return min(max(iBend, 0), 16383);
}

static void CorruptNote(float fCorrupt, uint64_t h,
    int& iNote, int& iTrack, int& iChannel,
    long long& llNoteStart, long long& llNoteLength,
    long long llTimeSpan, size_t iNumTracks)
{
    if (fCorrupt <= 0.0f)
        return;

    uint64_t h2 = h * 0x9e3779b97f4a7c15ull;
    const float fScale = 1.0f / 1048576.0f;
    float rTime = (float)((h >> 20) & 0xFFFFF) * fScale;
    float rLen = (float)((h >> 40) & 0xFFFFF) * fScale;
    float rCol = (float)(h2 & 0xFFFFF) * fScale;

    iNote = CorruptPitch(fCorrupt, h, iNote);

    llNoteStart += (long long)((double)(rTime * 2.0f - 1.0f) * (double)llTimeSpan * 0.10 * (double)fCorrupt);

    llNoteLength = (long long)((double)llNoteLength * (1.0 + (double)(rLen * 2.0f - 1.0f) * 0.5 * (double)fCorrupt));
// SplashScreen GameState object

    if (iNumTracks > 0)
    {
        iTrack += (int)llroundf((rCol * 2.0f - 1.0f) * (float)iNumTracks * 0.5f * fCorrupt);
        iTrack = min(max(iTrack, 0), (int)iNumTracks - 1);
    }
    iChannel += (int)llroundf((rCol * 2.0f - 1.0f) * 16.0f * fCorrupt);
    iChannel = min(max(iChannel, 0), 15);
}

static void CustomAudioStop();

GameState::GameError GameState::ChangeState( GameState *pNextState, GameState **pDestObj )
{
    if ( !pNextState )
        return Success;
    if (!pDestObj )
        return BadPointer;

    if (Config::GetConfig().GetAudioSettings().bPreRenderAudio && PRE_MIDIAudio)
    {
        PRE_DbgLog("ChangeState: Stop prerender");
        PRE_MIDIAudio->Stop();
    }
    CustomAudioStop();

    if ( *pDestObj )
    {
        if ( !pNextState->m_hWnd ) pNextState->m_hWnd = ( *pDestObj )->m_hWnd;
        if ( !pNextState->m_pRenderer ) pNextState->m_pRenderer = ( *pDestObj )->m_pRenderer;
        char buf[128];
        sprintf_s(buf, "ChangeState: %s -> %s", ( *pDestObj )->DebugName(), pNextState->DebugName());
        HeartbeatLog(buf);
        delete *pDestObj;
    }
    *pDestObj = pNextState;
    GameError iResult = pNextState->Init();
    if ( iResult )
    {
        *pDestObj = new IntroScreen( pNextState->m_hWnd, pNextState->m_pRenderer );
        delete pNextState;
        ( *pDestObj )->Init();
        return iResult;
    }


    return Success;
}


GameState::GameError IntroScreen::MsgProc( HWND, UINT msg, WPARAM wParam, LPARAM lParam )
{
    switch (msg)
    {
        case WM_COMMAND:
        {
            int iId = LOWORD( wParam );
            switch( iId )
            {
                case ID_CHANGESTATE:
                    m_pNextState = reinterpret_cast< GameState* >( lParam );
                    return Success;
                case ID_VIEW_RESETDEVICE:
                    m_pRenderer->ResetDevice();
                    return Success;
            }
        }
    }

    return Success;
}

GameState::GameError IntroScreen::Init()
{
    return Success;
}

GameState::GameError IntroScreen::Logic()
{
    m_pRenderer->ImGuiStartFrame();

    Sleep( 10 );
    return Success;
}

GameState::GameError IntroScreen::Render()
{
    if ( FAILED( m_pRenderer->ResetDeviceIfNeeded() ) ) return DirectXError;

    m_pRenderer->ClearAndBeginScene( D3DCOLOR_XRGB( 0, 0, 0 ) );
    m_pRenderer->DrawRect( 0.0f, 0.0f, static_cast< float >( m_pRenderer->GetBufferWidth() ),
                           static_cast< float >( m_pRenderer->GetBufferHeight() ), 0x00000000 );

    m_pRenderer->BeginText();
    m_pRenderer->EndText();

    m_pRenderer->EndScene();
    m_pRenderer->Present();
    return Success;
}


SplashScreen::SplashScreen( HWND hWnd, Renderer *pRenderer ) : GameState( hWnd, pRenderer )
{
    if (Config::GetConfig().GetAudioSettings().bPreRenderAudio && PRE_MIDIAudio)
    {
        PRE_DbgLog("SplashScreen ctor: Stop");
        PRE_MIDIAudio->Stop();
        m_bAudioStarted = false;
    }

    HRSRC hResInfo = FindResource( NULL, MAKEINTRESOURCE( IDR_SPLASHMIDI ), TEXT( "MIDI" ) );
    HGLOBAL hRes = LoadResource( NULL, hResInfo );
    int iSize = SizeofResource( NULL, hResInfo );
    unsigned char *pData = ( unsigned char * )LockResource( hRes );

    Config& config = Config::GetConfig();
    VizSettings viz = config.GetVizSettings();

    if (!viz.sSplashMIDI.empty()) {
        m_MIDI.~MIDI();
        new (&m_MIDI) MIDI(viz.sSplashMIDI);
        if (!m_MIDI.IsValid()) {
            MessageBox(hWnd, L"The custom splash MIDI failed to load. Please choose a different MIDI.", L"", MB_ICONWARNING);
            m_MIDI = MIDI();
            m_MIDI.ParseMIDI(pData, iSize);
        }
    } else {
        m_MIDI.ParseMIDI(pData, iSize);
    }
    m_MIDI.ConnectNotes(); // Order's important here
    m_MIDI.PostProcess(m_vEvents);

    m_vTrackSettings.resize( m_MIDI.GetInfo().iNumTracks );
    for (int i = 0; i < 128; i++)
        m_vState[i].reserve(128);

    //InitNotes( vEvents );
    InitState();
}

void SplashScreen::InitState()
{
    static Config &config = Config::GetConfig();
    static const PlaybackSettings &cPlayback = config.GetPlaybackSettings();
    static const VisualSettings &cVisual = config.GetVisualSettings();
    static const AudioSettings &cAudio = config.GetAudioSettings();
    static const VizSettings &cViz = config.GetVizSettings();

    m_iStartPos = 0;
    m_iEndPos = -1;
    m_llStartTime = m_MIDI.GetInfo().llFirstNote - 3000000;
    m_bPaused = cPlayback.GetPaused();
    m_bMute = cPlayback.GetMute();

    SetChannelSettings( vector< bool >(), vector< bool >(),
        vector< unsigned >( cVisual.colors, cVisual.colors + sizeof( cVisual.colors ) / sizeof( cVisual.colors[0] ) ) );

    if (cViz.bKDMAPI) {
        m_OutDevice.OpenKDMAPI();
    } else {
        if (cAudio.iOutDevice >= 0)
            m_OutDevice.Open(cAudio.iOutDevice);
    }
    m_OutDevice.SetVolume(1.0);

    m_Timer.Init(false);
}

GameState::GameError SplashScreen::Init()
{
    const size_t nTracks = min(m_vTrackSettings.size(), MaxTrackColors);
    auto* track_colors = m_pRenderer->GetTrackColors();
    for (size_t i = 0; i < nTracks; i++) {
        for (size_t j = 0; j < 16; j++) {
            auto& src = m_vTrackSettings[i].aChannels[j];
            track_colors[i * 16 + j] = TrackColor{ src.iPrimaryRGB, src.iDarkRGB, src.iVeryDarkRGB };
        }
        m_pRenderer->MarkTrackColorsDirty(i);
    }
    return Success;
}

void SplashScreen::ColorChannel( int iTrack, int iChannel, unsigned int iColor, bool bRandom )
{
    if ( bRandom )
        m_vTrackSettings[iTrack].aChannels[iChannel].SetColor();
    else
        m_vTrackSettings[iTrack].aChannels[iChannel].SetColor( iColor );
}

void SplashScreen::SetChannelSettings( const vector< bool > &, const vector< bool > &, const vector< unsigned > &vColor )
{
    const MIDI::MIDIInfo &mInfo = m_MIDI.GetInfo();
    const vector< MIDITrack* > &vTracks = m_MIDI.GetTracks();

    static Config& config = Config::GetConfig();
    static const VizSettings& cViz = config.GetVizSettings();

    size_t iPos = 0;
    for ( int i = 0; i < (int)mInfo.iNumTracks; i++ )
    {
        const MIDITrack::MIDITrackInfo &mTrackInfo = vTracks[i]->GetInfo();
        for ( int j = 0; j < 16; j++ )
            if ( mTrackInfo.aNoteCount[j] > 0 )
            {
                if (cViz.bColorLoop) {
                    ColorChannel(i, j, vColor[iPos % vColor.size()]);
                } else {
                    if (iPos < vColor.size())
                        ColorChannel(i, j, vColor[iPos]);
                    else
                        ColorChannel(i, j, 0, true);
                }
                iPos++;
            }
    }
}

GameState::GameError SplashScreen::MsgProc( HWND, UINT msg, WPARAM wParam, LPARAM lParam )
{
    static Config &config = Config::GetConfig();
    static PlaybackSettings &cPlayback = config.GetPlaybackSettings();
    static const AudioSettings& cAudio = config.GetAudioSettings();
    static const VizSettings& cViz = config.GetVizSettings();

    switch (msg)
    {
        case WM_COMMAND:
        {
            int iId = LOWORD( wParam );
            switch( iId )
            {
                case ID_CHANGESTATE:
                    HeartbeatLog("SplashScreen: ID_CHANGESTATE received");
                    m_pNextState = reinterpret_cast< GameState* >( lParam );
                    return Success;
                case ID_VIEW_RESETDEVICE:
                    m_pRenderer->ResetDevice();
                    return Success;
            }
        }
        case WM_DEVICECHANGE:
            if (!cViz.bKDMAPI) {
                if (cAudio.iOutDevice >= 0 && m_OutDevice.GetDevice() != cAudio.vMIDIOutDevices[cAudio.iOutDevice])
                    m_OutDevice.Open(cAudio.iOutDevice);
            }
            break;
        case WM_KEYDOWN:
        {
            switch( wParam )
            {
                case VK_SPACE:
                    cPlayback.TogglePaused( true );
                    return Success;
            }
        }
    }

    return Success;
}

// from the song clock every frame; the classic MIDI-device path is bypassed.
// BASS is used purely as a decoder (BASS_STREAM_DECODE, no output device - on this
// machine BASS's device resolution ends up on the silent "No sound" device). The
// decoded samples are fed to a dedicated SDL queue-mode output device instead, the
// same engine that already produces sound for the prerender.
static HSTREAM s_hCustomAudio = 0;      // decode-only stream
static SDL_AudioDeviceID s_hCustomSdl = 0;
static float s_fCustomAudioVol = -1.0f;
static double s_dFileRate = 0.0;        // frames/sec of the file
static unsigned s_dFileChans = 0;
static long long s_llFedUs = 0;         // total time fed into the SDL queue
static double s_dFeedFrac = 0.0;        // fractional frame remainder between feeds

static void CustomAudioStop()
{
    if (s_hCustomSdl)
    {
        SDL_CloseAudioDevice(s_hCustomSdl);
        s_hCustomSdl = 0;
    }
    if (s_hCustomAudio)
    {
        BASS_StreamFree(s_hCustomAudio);
        s_hCustomAudio = 0;
    }
    s_fCustomAudioVol = -1.0f;
    s_dFileRate = 0.0;
    s_dFileChans = 0;
    s_llFedUs = 0;
    s_dFeedFrac = 0.0;
}

// Opens a decode-only BASS stream plus a dedicated SDL queue-mode output device.
// Returns 0 on failure.
static HSTREAM CustomAudioOpen(const wstring &sFile)
{
    HSTREAM h = BASS_StreamCreateFile(FALSE, sFile.c_str(), 0, 0, BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT);
    if (h == 0)
    {
        PRE_DbgLog("CustomAudio: failed to open %ls (err %d)", sFile.c_str(), BASS_ErrorGetCode());
        return 0;
    }

    BASS_CHANNELINFO ci = {};
    BASS_ChannelGetInfo(h, &ci);
    s_dFileRate = ci.freq ? (double)ci.freq : 48000.0;
    s_dFileChans = ci.chans ? ci.chans : 2;
    PRE_DbgLog("CustomAudio: opened %ls rate=%.0f chans=%u len=%.2fs -> 0x%I64X err=%d", sFile.c_str(), s_dFileRate, (unsigned)ci.chans, (double)BASS_ChannelBytes2Seconds(h, BASS_ChannelGetLength(h, BASS_POS_BYTE)), (unsigned long long)h, (int)BASS_ErrorGetCode());

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
    {
        PRE_DbgLog("CustomAudio: SDL_InitSubSystem failed");
        BASS_StreamFree(h);
        return 0;
    }

    SDL_AudioSpec wanted = {};
    wanted.freq = (int)s_dFileRate;
    wanted.format = AUDIO_F32;
    wanted.channels = 2;
    wanted.samples = 2048;
    wanted.callback = NULL; // queue mode
    SDL_AudioSpec obtained = {};
    int nDevs = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < nDevs; i++)
        PRE_DbgLog("CustomAudio: SDL output dev[%d] = '%s'", i, SDL_GetAudioDeviceName(i, 0));
    s_hCustomSdl = SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, SDL_AUDIO_ALLOW_ANY_CHANGE);
    if (s_hCustomSdl == 0)
    {
        PRE_DbgLog("CustomAudio: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        BASS_StreamFree(h);
        return 0;
    }
    PRE_DbgLog("CustomAudio: SDL device opened id=%u freq=%d fmt=0x%X ch=%u", (unsigned)s_hCustomSdl, obtained.freq, (unsigned)obtained.format, (unsigned)obtained.channels);
    SDL_PauseAudioDevice(s_hCustomSdl, 1);
    s_llFedUs = 0;
    return h;
}

// Plays the user-provided audio file (wav/mp3/ogg) in place of the prerender. The file's
// t=0 maps to the first note of the song, i.e. the audio begins right after the 3-second
// lead-in stall. The song clock (visuals) is the master; the SDL queue is drained by the
// hardware, and we only re-seek (clear queue + rewind) when drift exceeds a small dead
// zone, so pause/seek/speed all just work.
static void CustomAudioUpdate(const wstring &sFile, long long llSongUs, long long llFirstNoteUs,
                              bool bPaused, bool bMute, double dVolume)
{
    static auto s_tLog = std::chrono::steady_clock::now();
    auto tNow = std::chrono::steady_clock::now();
    bool bLog = std::chrono::duration_cast<std::chrono::milliseconds>(tNow - s_tLog).count() >= 1000;
    if (bLog) s_tLog = tNow;

    long long llAudioUs = llSongUs - llFirstNoteUs;
    if (s_hCustomAudio == 0)
    {
        if (llAudioUs < 0 || sFile.empty())
        {
            if (bLog) PRE_DbgLog("CUSTOM waiting audioUs=%.2f empty=%d", (double)llAudioUs / 1e6, (int)sFile.empty());
            return;
        }
        s_hCustomAudio = CustomAudioOpen(sFile);
        if (s_hCustomAudio == 0)
            return;
        s_fCustomAudioVol = -1.0f;
    }

    QWORD qLength = BASS_ChannelGetLength(s_hCustomAudio, BASS_POS_BYTE);
    double dFileEndUs = qLength ? BASS_ChannelBytes2Seconds(s_hCustomAudio, qLength) * 1e6 : 0.0;

    // Still inside the lead-in stall: keep the output device silent
    if (llAudioUs < 0)
    {
        if (s_hCustomSdl) SDL_PauseAudioDevice(s_hCustomSdl, 1);
        if (bLog) PRE_DbgLog("CUSTOM stall hold audioUs=%.2f", (double)llAudioUs / 1e6);
        return;
    }

    long long llTargetUs = min(llAudioUs, (long long)dFileEndUs);
    if (llTargetUs >= (long long)dFileEndUs || bPaused)
    {
        if (s_hCustomSdl) SDL_PauseAudioDevice(s_hCustomSdl, 1);
        if (bLog) PRE_DbgLog("CUSTOM hold paused=%d target=%.2f end=%.2f", (int)bPaused, (double)llTargetUs / 1e6, dFileEndUs / 1e6);
        return;
    }

    float fVol = (float)(bMute ? 0.0 : dVolume);
    if (fVol != s_fCustomAudioVol)
    {
        s_fCustomAudioVol = fVol;
        if (bLog) PRE_DbgLog("CUSTOM vol=%.2f", fVol);
    }

    // Queued playhead = what we fed minus what the device has consumed (device-format bytes)
    SDL_AudioSpec obtained = {};
    SDL_GetAudioDeviceSpec(s_hCustomSdl, 0, &obtained);
    double dBpf = (double)(SDL_AUDIO_BITSIZE(obtained.format) / 8) * obtained.channels;
    double dQueuedUs = dBpf > 0 ? (double)SDL_GetQueuedAudioSize(s_hCustomSdl) / dBpf / obtained.freq * 1e6 : 0.0;
    double dPlayedUs = (double)s_llFedUs - dQueuedUs;

    // Hard resync when the queued output has drifted from the song clock. The dead
    // zone MUST stay well above the keep-ahead below (80ms): in steady state the
    // queued playhead legitimately sits up to keep-ahead ahead of the target, so a
    // tight threshold turns every refill cycle into a clear+seek -> clicks.
    if (fabs(dPlayedUs - llTargetUs) > 250000.0)
    {
        SDL_ClearQueuedAudio(s_hCustomSdl);
        BASS_ChannelSetPosition(s_hCustomAudio, BASS_ChannelSeconds2Bytes(s_hCustomAudio, llTargetUs / 1e6), BASS_POS_BYTE);
        s_llFedUs = llTargetUs;
        s_dFeedFrac = 0.0;
        if (bLog) PRE_DbgLog("CUSTOM resync target=%.2f played=%.2f", (double)llTargetUs / 1e6, dPlayedUs / 1e6);
    }

    // Keep the device fed ~80ms ahead of the target so it never starves; the queue is
    // drained by the hardware in real time and the song clock stays the master. Refill
    // only when the queue drops below ~40ms, and then in one large chunk: micro-feeding
    // every frame (the game loop runs far faster than the device pull period) makes the
    // WASAPI device see shortfalls at every pull boundary and click on each one.
    const long long kKeepAheadUs = 80000;
    const double kRefillBelowUs = 40000.0;
    if (dQueuedUs < kRefillBelowUs)
    {
        long long llWantUs = llTargetUs + kKeepAheadUs - s_llFedUs;
        if (llWantUs > 0)
        {
            double dFrames = min((double)llWantUs / 1e6 * s_dFileRate, 0.25 * s_dFileRate) + s_dFeedFrac;
            int nFrames = (int)dFrames;
            s_dFeedFrac = dFrames - nFrames;
            if (nFrames > 0)
            {
                static std::vector<float> s_pBuf;
                s_pBuf.resize((size_t)nFrames * s_dFileChans);
                DWORD dwGot = BASS_ChannelGetData(s_hCustomAudio, s_pBuf.data(), (DWORD)(s_pBuf.size() * sizeof(float)));
                size_t nGotFrames = (size_t)(dwGot / sizeof(float) / s_dFileChans);
                if (nGotFrames > 0)
                {
                    if (fVol != 1.0f)
                        for (size_t i = 0; i < nGotFrames * s_dFileChans; i++)
                            s_pBuf[i] *= fVol;
                    SDL_QueueAudio(s_hCustomSdl, s_pBuf.data(), (Uint32)(nGotFrames * s_dFileChans * sizeof(float)));
                }
                s_llFedUs += (long long)((double)nGotFrames / s_dFileRate * 1e6);
                if (bLog) PRE_DbgLog("CUSTOM run song=%.2f target=%.2f fed=%.2f queued=%.2f nGotF=%zu err=%d", (double)llSongUs / 1e6, (double)llTargetUs / 1e6, (double)s_llFedUs / 1e6, dQueuedUs / 1e6, nGotFrames, (int)BASS_ErrorGetCode());
            }
        }
    }

    // Never let the device idle silently mid-song: keep it running; the queue is
    // always refilled on the next frame. Pausing happens only in the states above.
    SDL_PauseAudioDevice(s_hCustomSdl, 0);
}

// from the song clock every frame; the classic MIDI-device path is bypassed.
// Prerender self-healing: when the BASS synth dies (stream handle lost, position
// frozen, silent-after-audio) the generator is rebuilt from the current position.
// After s_iPreRestartsLimit failed rebuilds the song falls back to the live MIDI
// device (omnimidi). Reset per song by MainScreen::InitState.
static const int s_iPreRestartsLimit = 4;
static int s_iPreRestarts = 0;
static bool s_bPreFailed = false;

static void UpdatePreRenderAudio(const std::vector<MIDIChannelEvent>& vEvents, const MIDI& midi, long long llStartTime, int iStartPos, bool& bAudioStarted)
{
    static Config &config = Config::GetConfig();
    static AudioSettings &cAudio = config.GetAudioSettings();
    static PlaybackSettings &cPlayback = config.GetPlaybackSettings();

    if (!cAudio.bPreRenderAudio)
    {
        if (PRE_MIDIAudio)
            SDL_PauseAudio(1);
        return;
    }

    if (s_bPreFailed)
    {
        if (PRE_MIDIAudio)
            SDL_PauseAudio(1);
        return;
    }

    if (!PRE_MIDIAudio)
    {
        PRE_DbgLog("UPA: init (create MIDIAudio/BASS/SDL)");
        WAVEFORMATEX wfFormat;
        memset(&wfFormat, 0, sizeof(wfFormat));
        wfFormat.nSamplesPerSec = 48000;
        wfFormat.nChannels = 2;
        wfFormat.nBlockAlign = (32 * 2) / 8;
        wfFormat.wBitsPerSample = 32;
        wfFormat.nAvgBytesPerSec = wfFormat.nSamplesPerSec * (32 * 2) / 8;
        wfFormat.wFormatTag = WAVE_FORMAT_PCM;
        BASSMIDI::InitBASS(wfFormat);

        BASSMIDI::LoadSoundfont(cAudio.sPreSoundfontPath.c_str());

        int initSecs = max(120, (cAudio.iPreBufferMs * 2 + 999) / 1000);
        PRE_MIDIAudio = new MIDIAudio(48000 * initSecs);
        PRE_MIDIAudio->m_bPaused = true;
        PRE_InitAudio();
    }

    bool bPaused = cPlayback.GetPaused();
    PRE_MIDIAudio->m_bPaused = bPaused;
    g_preVolume = cPlayback.GetMute() ? 0.0 : cPlayback.GetVolume();

    bool bRestart = false;
    if (PRE_MIDIAudio->m_iDefaultVoices != cAudio.iPreVoices)
    {
        PRE_MIDIAudio->m_iDefaultVoices = cAudio.iPreVoices;
        bRestart = true;
    }
    PRE_MIDIAudio->SetMaxAheadMs(cAudio.iPreBufferMs);
    PRE_MIDIAudio->m_dFPS = cAudio.dPreFPS;
    PRE_MIDIAudio->m_dAttack = (double)cAudio.iPreLMAttack / 1000.0;
    PRE_MIDIAudio->m_dRelease = (double)cAudio.iPreLMRelease / 1000.0;
    PRE_MIDIAudio->m_iVelThreshLow = cAudio.iPreVelThreshLow;
    PRE_MIDIAudio->m_iVelThreshUpp = cAudio.iPreVelThreshUpp;
    PRE_MIDIAudio->m_bDefaultNoFx = cAudio.bNoFX;
    PRE_MIDIAudio->m_bUnderrunRepeat = cAudio.bPreUnderrunRepeat;
    PRE_MIDIAudio->m_iRepeatFrames = cAudio.bPreRepeatCustom ? cAudio.iPreRepeatMs * 48 : 12000;
    PRE_MIDIAudio->m_bExtendVisualsOnSkip = cAudio.bPreStutterOnLag;
    PRE_MIDIAudio->SetReadSpeed(cPlayback.GetSpeed());

    if (g_bGenDead && PRE_MIDIAudio)
    {
        if (s_iPreRestarts >= s_iPreRestartsLimit)
        {
            s_bPreFailed = true;
            PRE_DbgLog("UPA: prerender dead %d times - falling back to live device", s_iPreRestarts);
            SDL_PauseAudio(1);
            PRE_MIDIAudio->Stop();
            bAudioStarted = false;
            g_bGenDead = false;
            return;
        }
        s_iPreRestarts++;
        PRE_DbgLog("UPA: generator dead - rebuild #%d (t=%.2f)", s_iPreRestarts, (double)llStartTime / 1e6);
        PRE_MIDIAudio->Stop();
        bAudioStarted = false;
        g_bGenDead = false;
    }

    if (!bPaused)
    {
        SDL_PauseAudio(0);

        // Watchdog: the SDL callback must be producing audio. If it has gone quiet
        // (device stall after a display/GPU hiccup on this machine), re-open the
        // device so the splash music resumes instead of dying silently.
        if (PRE_AudioStalled())
        {
            PRE_DbgLog("UPA: SDL audio callback stalled - restarting device");
            SDL_PauseAudio(1);
            PRE_RestartAudio();
            SDL_PauseAudio(0);
        }

        if (bRestart)
        {
            PRE_DbgLog("UPA: generator restart");
            PRE_MIDIAudio->Stop();
            bAudioStarted = false;
        }
        if (!bAudioStarted)
        {
            PRE_DbgLog("UPA: StartRender t=%.2f", (double)llStartTime / 1e6);
            PRE_MIDIAudio->m_pMIDI = &midi;
            PRE_MIDIAudio->StartRender(llStartTime, true, const_cast<std::vector<MIDIChannelEvent>*>(&vEvents), cPlayback.GetSpeed(), iStartPos);
            bAudioStarted = true;
        }
        else
        {
            PRE_MIDIAudio->SyncPlayer((double)llStartTime / 1e6, cPlayback.GetSpeed());
        }
    }
    else
    {
        SDL_PauseAudio(1);
    }
}

GameState::GameError SplashScreen::Logic()
{
    m_pRenderer->ImGuiStartFrame();

    static Config &config = Config::GetConfig();
    static PlaybackSettings &cPlayback = config.GetPlaybackSettings();
    const MIDI::MIDIInfo &mInfo = m_MIDI.GetInfo();

    bool bPaused = cPlayback.GetPaused();
    bool bMute = cPlayback.GetMute();
    bool bMuteChanged = ( bMute != m_bMute );
    bool bPausedChanged = ( bPaused != m_bPaused );
    
    m_bMute = bMute;
    m_bPaused = bPaused;
    m_dVolume = cPlayback.GetVolume();

    double dMaxCorrect = ( mInfo.iMaxVolume > 0 ? 127.0 / mInfo.iMaxVolume : 1.0 );
    double dVolumeCorrect = ( mInfo.iVolumeSum > 0 ? ( m_dVolume * 127.0 * mInfo.iNoteCount ) / mInfo.iVolumeSum : 1.0 );
    dVolumeCorrect = min( dVolumeCorrect, dMaxCorrect );

    long long llMaxTime = m_MIDI.GetInfo().llTotalMicroSecs + 500000;
    long long llElapsed = m_Timer.GetMicroSecs();
    m_Timer.Start();

    if ( ( bPausedChanged || bMuteChanged ) && ( m_bPaused || m_bMute ) )
        m_OutDevice.AllNotesOff();

    double dSpeed = cPlayback.GetSpeed();
    long long llElapsedStep = static_cast< long long >( min( (double)llElapsed * dSpeed, 100000.0 ) + 0.5 );
    long long llNextStartTime = m_llStartTime + llElapsedStep;
    // fixed slew would break speed changes; instead cap the catch-up on top of
    const long long kDeadZone = 20000;   // ignore drift smaller than 20ms
    const long long kMaxCatchUp = 10000; // extra catch-up at most 10ms per frame
    if ( !bPaused && m_llStartTime < llMaxTime )
    {
        if ( config.GetAudioSettings().bPreRenderAudio && config.GetAudioSettings().bPreStutterOnLag
             && m_bAudioStarted && PRE_MIDIAudio && PRE_MIDIAudio->IsAudioStarted() )
        {
            long long llAudioTime = (long long)( PRE_MIDIAudio->GetPlayerTime() * 1000000.0 );
            if ( llAudioTime >= 0 && llAudioTime <= llMaxTime )
            {
                long long llAhead = llAudioTime - llNextStartTime;
                if ( llAhead > kDeadZone )
                {
                    llNextStartTime += min( llAhead - kDeadZone, kMaxCatchUp );
                }
                else if ( llAhead < -kDeadZone )
                {
                    llNextStartTime = min( llNextStartTime, llAudioTime + kDeadZone );
                    static auto s_tHoldLog = std::chrono::steady_clock::now();
                    auto tNowHold = std::chrono::steady_clock::now();
                    if ( std::chrono::duration_cast<std::chrono::milliseconds>(tNowHold - s_tHoldLog).count() >= 500 )
                    {
                        s_tHoldLog = tNowHold;
                        PRE_DbgLog("CLK hold audio=%.3f vis=%.3f ahead=%.3f", (double)llAudioTime/1e6, (double)llNextStartTime/1e6, (double)llAhead/1e6);
                    }
                }
                m_llStartTime = llNextStartTime;
            }
            else
            {
                m_llStartTime = llNextStartTime;
            }
        }
        else
        {
            m_llStartTime = llNextStartTime;
        }
    }
    long long llEndTime = m_llStartTime + TimeSpan;

    RenderGlobals();

    int iEventCount = (int)m_vEvents.size();
    while ( m_iEndPos + 1 < iEventCount && m_MIDI.GetEventTime(m_vEvents[m_iEndPos + 1]) < llEndTime )
        m_iEndPos++;

    // The splash always plays through the live MIDI device below.
    {
        using clock_t = std::chrono::steady_clock;
        static auto s_tPushLog = clock_t::now();
        static long long s_iPushed = 0;
        static double s_dMaxFrameUs = 0;
        auto tPushStart = clock_t::now();
        size_t iPushed = 0;
        while ( m_iStartPos < iEventCount && m_MIDI.GetEventTime(m_vEvents[m_iStartPos]) <= m_llStartTime )
        {
            MIDIChannelEvent pEvent = m_vEvents[m_iStartPos];
            if ( m_MIDI.GetEventChannelEventType(pEvent) != MIDI::NoteOn )
                m_OutDevice.PlayEvent( m_MIDI.GetEventCode(pEvent), m_MIDI.GetEventParam1(pEvent), m_MIDI.GetEventParam2(pEvent) );
            else if ( !m_bMute && !m_vTrackSettings[m_MIDI.GetEventTrack(pEvent)].aChannels[m_MIDI.GetEventChannel(pEvent)].bMuted )
                m_OutDevice.PlayEvent( m_MIDI.GetEventCode(pEvent), m_MIDI.GetEventParam1(pEvent),
                                        static_cast< int >( m_MIDI.GetEventParam2(pEvent) * dVolumeCorrect + 0.5 ) );
            UpdateState( m_iStartPos );
            m_iStartPos++;
            iPushed++;
        }
        auto tPushEnd = clock_t::now();
        double dFrameUs = std::chrono::duration<double, std::micro>(tPushEnd - tPushStart).count();
        s_iPushed += (long long)iPushed;
        if ( dFrameUs > s_dMaxFrameUs ) s_dMaxFrameUs = dFrameUs;
        if ( std::chrono::duration_cast<std::chrono::milliseconds>(tPushEnd - s_tPushLog).count() >= 1000 )
        {
            PRE_DbgLog("LIVEPUSH live ev/s=%lld maxFrameMs=%.1f",
                s_iPushed, s_dMaxFrameUs / 1000.0);
            s_iPushed = 0;
            s_dMaxFrameUs = 0;
            s_tPushLog = tPushEnd;
        }
    }

    auto& root_consts = m_pRenderer->GetRootConstants();
    root_consts.deflate = clamp(round(m_fWhiteCX * 0.15f / 2.0f), 1.0f, 3.0f);
    root_consts.notes_y = m_fNotesY;
    root_consts.notes_cy = m_fNotesCY;
    root_consts.white_cx = m_fWhiteCX;
    root_consts.timespan = TimeSpan;
    root_consts.notes_x = m_fNotesX;
    root_consts.notes_cx = m_fNotesCX;

    auto& fixed_consts = m_pRenderer->GetFixedSizeConstants();
    memcpy(&fixed_consts.note_x, &notex_table, sizeof(float) * 128);
    memset(&fixed_consts.bends, 0, sizeof(float) * 16);

    SyncTrackColors(m_pRenderer, m_vTrackSettings);

    return Success;
}

int sse_bin_search(const std::vector<int>& data, int key) {
    auto it = std::lower_bound(data.begin(), data.end(), key);
    if (it != data.end() && *it == key)
        return static_cast<int>(it - data.begin());
    return -1;
}

void SplashScreen::UpdateState(int iPos)
{
    MIDIChannelEvent pEvent = m_vEvents[iPos];
    if (!m_MIDI.EventHasSister(pEvent)) return;

    MIDI::ChannelEventType eEventType = m_MIDI.GetEventChannelEventType(pEvent);
    int iNote = m_MIDI.GetEventParam1(pEvent);
    int iVelocity = m_MIDI.GetEventParam2(pEvent);

    unsigned iSisterIdx = m_MIDI.GetEventSisterIdx(pEvent);
    auto& note_state = m_vState[iNote];

    if (eEventType == MIDI::NoteOn && iVelocity > 0)
        note_state.push_back(iPos);
    else
    {
        if (iSisterIdx != UINT32_MAX) {
            auto pos = sse_bin_search(note_state, (int)iSisterIdx);
            if (pos != -1)
                note_state.erase(note_state.begin() + pos);
        }
        else {
            vector< int >::iterator it = note_state.begin();
            MIDIChannelEvent pSearch = m_vEvents[m_MIDI.GetEventSisterIdx(pEvent)];
            while (it != note_state.end())
            {
                if (m_vEvents[*it] == pSearch) {
                    it = note_state.erase(it);
                    break;
                }
                else {
                    ++it;
                }
            }
        }
    }
}

const float SplashScreen::SharpRatio = 0.65f;

GameState::GameError SplashScreen::Render()
{
    if ( FAILED( m_pRenderer->ResetDeviceIfNeeded() ) ) return DirectXError;

    m_pRenderer->ClearAndBeginScene( D3DCOLOR_XRGB( 0, 0, 0 ) );
    m_pRenderer->DrawRect( 0.0f, 0.0f, static_cast< float >( m_pRenderer->GetBufferWidth() ),
                           static_cast< float >( m_pRenderer->GetBufferHeight() ), 0x00000000 );
    RenderNotes();

    m_pRenderer->EndScene();
    m_pRenderer->Present();
    return Success;
}

void SplashScreen::RenderGlobals()
{
    const MIDI::MIDIInfo &mInfo = m_MIDI.GetInfo();
    m_iStartNote = mInfo.iMinNote;
    m_iEndNote = mInfo.iMaxNote;

    m_fNotesX = 0.0f;
    m_fNotesCX = static_cast< float >( m_pRenderer->GetBufferWidth() );
    m_fNotesY = 0.0f;
    m_fNotesCY = static_cast< float >( m_pRenderer->GetBufferHeight() );

    m_iAllWhiteKeys = MIDI::WhiteCount( m_iStartNote, m_iEndNote + 1 );
    float fBuffer = ( MIDI::IsSharp( m_iStartNote ) ? SharpRatio / 2.0f : 0.0f ) +
                    ( MIDI::IsSharp( m_iEndNote ) ? SharpRatio / 2.0f : 0.0f );
    m_fWhiteCX = m_fNotesCX / ( m_iAllWhiteKeys + fBuffer );

    long long llMicroSecsPP = static_cast< long long >( TimeSpan / m_fNotesCY + 0.5f );
    m_llRndStartTime = m_llStartTime - ( m_llStartTime < 0 ? llMicroSecsPP : 0 );
    m_llRndStartTime = ( m_llRndStartTime / llMicroSecsPP ) * llMicroSecsPP;

    GenNoteXTable();
}

void SplashScreen::RenderNotes()
{
    if ( m_iEndPos < 0 || m_iStartPos >= static_cast< int >( m_vEvents.size() ) )
        return;

    for (int i = m_iEndPos; i >= m_iStartPos; i--) {
        MIDIChannelEvent pEvent = m_vEvents[i];
        if (m_MIDI.GetEventChannelEventType(pEvent) == MIDI::NoteOn &&
            m_MIDI.GetEventParam2(pEvent) > 0 && m_MIDI.EventHasSister(pEvent) &&
            MIDI::IsSharp(m_MIDI.GetEventParam1(pEvent))) {
            RenderNote(pEvent);
        }
    }
    for (int i = 0; i < 128; i++) {
        if (MIDI::IsSharp(i)) {
            for (vector< int >::reverse_iterator it = (m_vState[i]).rbegin(); it != (m_vState[i]).rend(); it++) {
                RenderNote(m_vEvents[*it]);
            }
        }
    }

    for (int i = m_iEndPos; i >= m_iStartPos; i--) {
        MIDIChannelEvent pEvent = m_vEvents[i];
        if (m_MIDI.GetEventChannelEventType(pEvent) == MIDI::NoteOn &&
            m_MIDI.GetEventParam2(pEvent) > 0 && m_MIDI.EventHasSister(pEvent))
        {
            if (!MIDI::IsSharp(m_MIDI.GetEventParam1(pEvent))) {
                RenderNote(pEvent);
            }
        }
    }
    for (int i = 0; i < 128; i++) {
        if (!MIDI::IsSharp(i)) {
            for (vector< int >::reverse_iterator it = (m_vState[i]).rbegin(); it != (m_vState[i]).rend(); it++) {
                RenderNote(m_vEvents[*it]);
            }
        }
    }

    m_pRenderer->RenderBatch();
}

void SplashScreen::RenderNote(MIDIChannelEvent pNote)
{
    int iNote = m_MIDI.GetEventParam1(pNote);
    int iTrack = m_MIDI.GetEventTrack(pNote);
    int iChannel = m_MIDI.GetEventChannel(pNote);
    long long llNoteStart = m_MIDI.GetEventTime(pNote);
    long long llNoteEnd = llNoteStart + m_MIDI.GetEventLength(pNote);
    m_pRenderer->PushNoteData(
        NoteData {
            .key = (uint8_t)iNote,
            .channel = (uint8_t)iChannel,
            .track = (uint16_t)iTrack,
            .pos = static_cast<float>(llNoteStart - m_llRndStartTime),
            .length = static_cast<float>(llNoteEnd - llNoteStart),
        }
    );
}

void SplashScreen::GenNoteXTable() {
    int min_key = min(max(0, m_iStartNote), 127);
    int max_key = min(max(0, m_iEndNote), 127);
    for (int i = min_key; i <= max_key; i++) {
        int iWhiteKeys = MIDI::WhiteCount(m_iStartNote, i);
        float fStartX = (MIDI::IsSharp(m_iStartNote) - MIDI::IsSharp(i)) * SharpRatio / 2.0f;
        if (MIDI::IsSharp(i))
        {
            MIDI::Note eNote = MIDI::NoteVal(i);
            if (eNote == MIDI::CS || eNote == MIDI::FS) fStartX -= SharpRatio / 5.0f;
            else if (eNote == MIDI::AS || eNote == MIDI::DS) fStartX += SharpRatio / 5.0f;
        }
        notex_table[i] = m_fNotesX + m_fWhiteCX * (iWhiteKeys + fStartX);
    }
}

float SplashScreen::GetNoteX(int iNote) {
    return notex_table[iNote];
}


MainScreen::MainScreen( wstring sMIDIFile, State eGameMode, HWND hWnd, Renderer *pRenderer ) :
    GameState( hWnd, pRenderer ), m_MIDI( sMIDIFile ), m_eGameMode( eGameMode )
{
    if (Config::GetConfig().GetAudioSettings().bPreRenderAudio && PRE_MIDIAudio)
    {
        PRE_DbgLog("MainScreen ctor: Stop");
        PRE_MIDIAudio->Stop();
        m_bAudioStarted = false;
    }

    if ( !m_MIDI.IsValid() ) return;
    try
    {
        m_MIDI.ConnectNotes(); // Order's important here
        // the memory peak and throws bad_alloc; PostProcess's push_back doubling
        m_MIDI.PostProcess(m_vEvents, &m_vProgramChange, &m_vMetaEvents, &m_vTempo, &m_vSignature, &m_vMarkers);
        if (!m_vEvents.empty())
        {
            size_t aProbe[] = { 0, 1, 10, 100, 1000, 10000, 100000, 1000000, (size_t)m_vEvents.size() / 16,
                                (size_t)m_vEvents.size() / 8, (size_t)m_vEvents.size() / 4,
                                (size_t)m_vEvents.size() / 2, (size_t)(m_vEvents.size() * 3 / 4),
                                m_vEvents.size() - 1000000, m_vEvents.size() - 1000, m_vEvents.size() - 1 };
            char buf[512] = {};
            size_t iLen = 0;
            for (size_t p : aProbe)
            {
                if (p >= m_vEvents.size()) continue;
                iLen += (size_t)sprintf_s(buf + iLen, sizeof(buf) - iLen, "%zu:%.1fs ", p,
                    (double)m_MIDI.GetEventTime(m_vEvents[p]) / 1e6);
            }
            PRE_DbgLog("EVENTMAP %s", buf);
            size_t aTypes[8] = {}; // NoteOn, NoteOff, Controller, ProgramChange, PitchBend, NoteAftertouch, ChannelAftertouch, other
            for (size_t s = 0; s < 200000; s++)
            {
                size_t p = (size_t)((unsigned long long)s * m_vEvents.size() / 200000 + 7) % m_vEvents.size();
                switch (m_MIDI.GetEventChannelEventType(m_vEvents[p]))
                {
                case MIDI::NoteOn: aTypes[0]++; break;
                case MIDI::NoteOff: aTypes[1]++; break;
                case MIDI::Controller: aTypes[2]++; break;
                case MIDI::ProgramChange: aTypes[3]++; break;
                case MIDI::PitchBend: aTypes[4]++; break;
                case MIDI::NoteAftertouch: aTypes[5]++; break;
                case MIDI::ChannelAftertouch: aTypes[6]++; break;
                default: aTypes[7]++; break;
                }
            }
            PRE_DbgLog("EVENTTYPES on=%zu off=%zu cc=%zu prog=%zu bend=%zu atouch=%zu catouch=%zu other=%zu",
                aTypes[0], aTypes[1], aTypes[2], aTypes[3], aTypes[4], aTypes[5], aTypes[6], aTypes[7]);

            // Precompute maximum Notes-Per-Second for the song
            m_llMaxNPS = 1;
            vector<long long> noteTimes;
            noteTimes.reserve(m_vEvents.size());
            for (size_t i = 0; i < m_vEvents.size(); i++)
            {
                if (m_MIDI.GetEventChannelEventType(m_vEvents[i]) == MIDI::NoteOn &&
                    m_MIDI.GetEventParam2(m_vEvents[i]) > 0)
                {
                    noteTimes.push_back(m_MIDI.GetEventTime(m_vEvents[i]));
                }
            }
            if (!noteTimes.empty())
            {
                size_t left = 0;
                for (size_t right = 0; right < noteTimes.size(); right++)
                {
                    while (noteTimes[right] - noteTimes[left] > 1000000)
                        left++;
                    long long curWindowNPS = (long long)(right - left + 1);
                    if (curWindowNPS > m_llMaxNPS)
                        m_llMaxNPS = curWindowNPS;
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        PRE_DbgLog("MainScreen load failed: %s", e.what());
        return;
    }

    m_vTrackSettings.resize( m_MIDI.GetInfo().iNumTracks );
    for (auto note_state : m_vState)
        note_state.reserve(m_MIDI.GetInfo().iNumTracks * 16);

    InitColors();
    InitState();

    g_LoadingProgress.stage = MIDILoadingProgress::Stage::Done;
}

void MainScreen::Discard()
{
    m_bDiscarded = true;

    if (Config::GetConfig().GetAudioSettings().bPreRenderAudio && PRE_MIDIAudio)
    {
        PRE_DbgLog("Discard: Stop prerender");
        PRE_MIDIAudio->Stop();
    }
    CustomAudioStop();

    PRE_DbgLog("Discard: freeing song data (events=%zu)", m_vEvents.size());
    m_MIDI.clear();
    m_MIDI.ReleaseOwnedData();
    vector<MIDIChannelEvent>().swap(m_vEvents);
    vector<MIDIMetaEvent*>().swap(m_vMetaEvents);
    eventvec_t().swap(m_vNoteOns);
    eventvec_t().swap(m_vNonNotes);
    eventvec_t().swap(m_vProgramChange);
    eventvec_t().swap(m_vTempo);
    eventvec_t().swap(m_vSignature);
    eventvec_t().swap(m_vMarkers);
    for (int i = 0; i < 128; i++)
    {
        vector<int>().swap(m_vState[i]);
        vector<thread_work_t>().swap(m_vThreadWork[i]);
    }
    vector<TrackSettings>().swap(m_vTrackSettings);
    m_sMarker.clear();
    m_sCurBackground.clear();
    m_dNPSNotes.clear();
    m_dNPSHistory.clear();
    m_vImageData.clear();
    PRE_DbgLog("Discard: done");
}

void MainScreen::InitColors()
{
    static Config& config = Config::GetConfig();
    static const VizSettings& cViz = config.GetVizSettings();

    m_csBackground.SetColor( 0x00464646, 0.7f, 1.3f );
    m_csKBBackground.SetColor( 0x00999999, 0.4f, 0.0f );
    m_csKBRed.SetColor(cViz.iBarColor, 0.5f);
    m_csKBWhite.SetColor( 0x00FFFFFF, 0.8f, 0.6f );
    m_csKBSharp.SetColor( 0x00404040, 0.5f, 0.0f );
}

void MainScreen::InitState()
{
    s_iPreRestarts = 0;
    s_bPreFailed = false;
    g_bGenDead = false;

    static Config &config = Config::GetConfig();
    static const PlaybackSettings &cPlayback = config.GetPlaybackSettings();
    static const ViewSettings &cView = config.GetViewSettings();
    static const VizSettings& cViz = config.GetVizSettings();

    m_eGameMode = Practice;
    m_iStartPos = 0;
    m_iEndPos = -1;
    m_llStartTime = GetMinTime();
    m_llDisplayTime = m_llStartTime;
    m_fKeysTransition = 0.0f;
    s_fTransitionPhase = 0.0f;
    s_LastTransitionFrame = std::chrono::steady_clock::now();
    m_bTrackPos = m_bTrackZoom = false;
    m_fTempZoomX = 1.0f;
    m_fTempOffsetX = m_fTempOffsetY = 0.0f;
    m_dFPS = 0.0;
    m_iFPSCount = 0;
    m_llFPSTime = 0;
    m_llFrameMaxLate = 0;
    m_ullFrameLateCount = 0;
    m_llMaxLateMicros = 0;
    m_ullLateEvents = 0;
    m_dSpeed = -1.0; // Forces a speed reset upon first call to Logic

    m_fZoomX = cView.GetZoomX();
    m_fOffsetX = cView.GetOffsetX();
    m_fOffsetY = cView.GetOffsetY();
    m_bPaused = true;
    m_bMute = cPlayback.GetMute();
    double dNSpeed = cPlayback.GetNSpeed();
    m_llTimeSpan = static_cast< long long >( 3.0 * dNSpeed * 1000000 );

    m_RealTimer.Init(false);

    m_bDumpFrames = cViz.bDumpFrames;
    if (m_bDumpFrames) {
        RECT rect = {};
        GetWindowRect(g_hWndGfx, &rect);
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;

        char buf[1024] = {};
        snprintf(buf, sizeof(buf), "Waiting for connection... (%d x %d)", width, height);
        SetWindowTextA(g_hWnd, buf);
        m_hVideoPipe = CreateNamedPipe(TEXT("\\\\.\\pipe\\pfadump"),
            PIPE_ACCESS_OUTBOUND,
            PIPE_TYPE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            static_cast<DWORD>(width * height * 4 * 120),
            0,
            0,
            nullptr);
        ConnectNamedPipe(m_hVideoPipe, NULL);
        SetWindowTextA(g_hWnd, "Connected!");
    }

    memset( m_pNoteState, -1, sizeof( m_pNoteState ) );
    
    AdvanceIterators( m_llStartTime, true );
}

// Called immediately before changing to this state
GameState::GameError MainScreen::Init()
{
    static Config& config = Config::GetConfig();
    static const AudioSettings &cAudio = config.GetAudioSettings();
    static const VizSettings &cViz = config.GetVizSettings();
    if (cViz.bKDMAPI) {
        m_OutDevice.OpenKDMAPI();
    } else {
        if (cAudio.iOutDevice >= 0)
            m_OutDevice.Open(cAudio.iOutDevice);
    }

    m_OutDevice.Reset();
    m_OutDevice.SetVolume( 1.0 );
    m_Timer.Init(config.m_bManualTimer || m_bDumpFrames);
    if (m_bDumpFrames) {
        m_Timer.SetFrameRate(60);
    } else if (m_Timer.m_bManualTimer) {
        // get the screen's refresh rate
        DWM_TIMING_INFO timing_info;
        memset(&timing_info, 0, sizeof(timing_info));
        timing_info.cbSize = sizeof(timing_info);
        if (FAILED(DwmGetCompositionTimingInfo(NULL, &timing_info))) {
            MessageBox(NULL, L"Failed to get the screen refresh rate! Defaulting to 60hz...", L"", MB_ICONERROR);
            m_Timer.SetFrameRate(60);
        } else {
            m_Timer.SetFrameRate(ceil(static_cast<float>(timing_info.rateRefresh.uiNumerator) / static_cast<float>(timing_info.rateRefresh.uiDenominator)));
        }

    }

    for (auto& work : m_vThreadWork)
        work.reserve(262144); // Should be plenty for most MIDIs

    return Success;
}

void MainScreen::ColorChannel( int iTrack, int iChannel, unsigned int iColor, bool bRandom )
{
    if ( bRandom )
        m_vTrackSettings[iTrack].aChannels[iChannel].SetColor();
    else
        m_vTrackSettings[iTrack].aChannels[iChannel].SetColor( iColor );
}

// Sets to a random color
void ChannelSettings::SetColor()
{
    SetColor( Util::RandColor(), 0.6, 0.2 );
}

// Flips around windows format (ABGR) -> direct x format (ARGB)
void ChannelSettings::SetColor( unsigned int iColor, double dDark, double dVeryDark )
{
    int R = ( iColor >> 0 ) & 0xFF, dR, vdR;
    int G = ( iColor >> 8 ) & 0xFF, dG, vdG;
    int B = ( iColor >> 16 ) & 0xFF, dB, vdB;
    int A = ( iColor >> 24 ) & 0xFF;

    int H, S, V;
    Util::RGBtoHSV( R, G, B, H, S, V );
    Util::HSVtoRGB( H, S, min( 100, static_cast< int >( V * dDark ) ), dR, dG, dB );
    Util::HSVtoRGB( H, S, min( 100, static_cast< int >( V * dVeryDark ) ), vdR, vdG, vdB );

    this->iOrigBGR = iColor;
    this->iPrimaryRGB = ( A << 24 ) | ( R << 16 ) | ( G << 8 ) | ( B << 0 );
    this->iDarkRGB = ( A << 24 ) | ( dR << 16 ) | ( dG << 8 ) | ( dB << 0 );
    this->iVeryDarkRGB =  ( A << 24 ) | ( vdR << 16 ) | ( vdG << 8 ) | ( vdB << 0 );
}

ChannelSettings* MainScreen::GetChannelSettings( int iTrack )
{
    const MIDI::MIDIInfo &mInfo = m_MIDI.GetInfo();
    const vector< MIDITrack* > &vTracks = m_MIDI.GetTracks();

    int iPos = 0;
    for ( uint32_t i = 0; i < mInfo.iNumTracks; i++ )
    {
        const MIDITrack::MIDITrackInfo &mTrackInfo = vTracks[i]->GetInfo();
        for (uint32_t j = 0; j < 16; j++ )
            if ( mTrackInfo.aNoteCount[j] > 0 )
            {
                if ( iPos == iTrack ) return &m_vTrackSettings[i].aChannels[j];
                iPos++;
            }
    }
    return NULL;
}

void MainScreen::SetChannelSettings( const vector< bool > &vMuted, const vector< bool > &vHidden, const vector< unsigned > &vColor )
{
    const vector< MIDITrack* > &vTracks = m_MIDI.GetTracks();

    bool bMuted = vMuted.size() > 0;
    bool bHidden = vHidden.size() > 0;
    bool bColor = vColor.size() > 0;

    static Config& config = Config::GetConfig();
    static const VizSettings& cViz = config.GetVizSettings();

    size_t iPos = 0;
    unsigned int last_col = bColor ? vColor[0] : 0;
    for (int i = 0; i < (int)vTracks.size(); i++) {
        const MIDITrack::MIDITrackInfo& mTrackInfo = vTracks[i]->GetInfo();
        for (int j = 0; j < 16; j++) {
            if (mTrackInfo.aNoteCount[j] > 0) {
                MuteChannel(i, j, bMuted ? vMuted[min(iPos, vMuted.size() - 1)] : false);
                HideChannel(i, j, bHidden ? vHidden[min(iPos, vHidden.size() - 1)] : false);
                if (cViz.bColorLoop && bColor) {
                    last_col = vColor[iPos % vColor.size()];
                } else {
                    if (bColor && iPos < vColor.size())
                        last_col = vColor[iPos];
                    else
                        last_col = Util::RandColor();
                }
                iPos++;
            }
            ColorChannel(i, j, last_col);
        }
    }
}

GameState::GameError MainScreen::MsgProc( HWND, UINT msg, WPARAM wParam, LPARAM lParam )
{
    // Not thread safe, blah
    static Config &config = Config::GetConfig();
    static PlaybackSettings &cPlayback = config.GetPlaybackSettings();
    static ViewSettings &cView = config.GetViewSettings();
    static const ControlsSettings &cControls = config.GetControlsSettings();
    static const AudioSettings &cAudio = config.GetAudioSettings();
    static const VizSettings &cViz = config.GetVizSettings();

    switch (msg)
    {
        // Commands that were passed straight through because they're more involved than setting a state variable
        case WM_COMMAND:
        {
            int iId = LOWORD( wParam );
            switch ( iId )
            {
                case ID_CHANGESTATE:
                    m_pNextState = reinterpret_cast< GameState* >( lParam );
                    return Success;
                case ID_PLAY_STOP:
                    JumpTo(GetMinTime());
                    cPlayback.SetStopped(true);
                    s_fTransitionPhase = 0.0f;
                    m_fKeysTransition = 0.0f;
                    s_LastTransitionFrame = std::chrono::steady_clock::now();
                    return Success;
                case ID_PLAY_SKIPFWD:
                    JumpTo(static_cast<long long>(m_llStartTime + cControls.dFwdBackSecs * 1000000));
                    return Success;
                case ID_PLAY_SKIPBACK:
                    JumpTo(static_cast<long long>(m_llStartTime - cControls.dFwdBackSecs * 1000000));
                    return Success;
                case ID_VIEW_RESETDEVICE:
                    m_pRenderer->ResetDevice();
                    return Success;
                case ID_FILE_RENDERVIDEO:
                    // Open the render settings dialog; the actual render starts
                    // from the Render! button inside it.
                    m_pRenderer->m_bShowRenderDialog = true;
                    return Success;
                case ID_FILE_STOPRENDER:
                    StopVideoRender();
                    return Success;
                case ID_VIEW_MOVEANDZOOM:
                    if ( cView.GetZoomMove() )
                    {
                        cView.SetOffsetX( cView.GetOffsetX() + m_fTempOffsetX );
                        cView.SetOffsetY( cView.GetOffsetY() + m_fTempOffsetY );
                        cView.SetZoomX( cView.GetZoomX() * m_fTempZoomX );
                    }
                    else
                    {
                        cView.SetZoomMove( true, true );
                    }
                    return Success;
                case ID_VIEW_CANCELMOVEANDZOOM:
                    cView.SetZoomMove( false, true );
                    m_bTrackPos = m_bTrackZoom = false;
                    m_fTempOffsetX = 0.0f;
                    m_fTempOffsetY = 0.0f;
                    m_fTempZoomX = 1.0f;
                    return Success;
                case ID_VIEW_RESETMOVEANDZOOM:
                    cView.SetOffsetX( 0.0f );
                    cView.SetOffsetY( 0.0f );
                    cView.SetZoomX( 1.0f );
                    m_fTempOffsetX = 0.0f;
                    m_fTempOffsetY = 0.0f;
                    m_fTempZoomX = 1.0f;
                    return Success;
                case ID_UPDATE_TRACKCOLORS:
                {
                    const VisualSettings& cVisual = config.GetVisualSettings();
                    SetChannelSettings(
                        vector< bool >(),
                        vector< bool >(),
                        vector< unsigned >( cVisual.colors, cVisual.colors + sizeof( cVisual.colors ) / sizeof( cVisual.colors[0] ) ) );
                    return Success;
                }
            }
            break;
        }
        case WM_KEYDOWN:
        {
            bool bCtrl = GetKeyState( VK_CONTROL ) < 0;
            bool bAlt = GetKeyState( VK_MENU ) < 0;
            bool bShift = GetKeyState( VK_SHIFT ) < 0;

            switch( wParam )
            {
                case VK_F6:
                    m_pRenderer->m_bShowRenderDialog = true;
                    return Success;
                case VK_SPACE:
                    cPlayback.TogglePaused( true );
                    return Success;
                case VK_OEM_PERIOD:
                    JumpTo(GetMinTime());
                    cPlayback.SetStopped(true);
                    return Success;
                case VK_UP:
                    if ( bAlt && !bCtrl )
                        cPlayback.SetVolume( min( cPlayback.GetVolume() + 0.1, 1.0 ), true );
                    else if ( bShift && !bCtrl )
                        cPlayback.SetNSpeed( cPlayback.GetNSpeed() * ( 1.0 + cControls.dSpeedUpPct / 100.0 ), true );
                    else if ( !bAlt && !bShift )
                        cPlayback.SetSpeed( cPlayback.GetSpeed() / ( 1.0 + cControls.dSpeedUpPct / 100.0 ), true );
                    return Success;
                case VK_DOWN:
                    if ( bAlt && !bShift && !bCtrl )
                        cPlayback.SetVolume( max( cPlayback.GetVolume() - 0.1, 0.0 ), true );
                    else if ( bShift && !bAlt && !bCtrl )
                        cPlayback.SetNSpeed( cPlayback.GetNSpeed() / ( 1.0 + cControls.dSpeedUpPct / 100.0 ), true );
                    else if ( !bAlt && !bShift )
                        cPlayback.SetSpeed( cPlayback.GetSpeed() * ( 1.0 + cControls.dSpeedUpPct / 100.0 ), true );
                    return Success;
                case 'R':
                    cPlayback.SetSpeed( 1.0, true );
                    return Success;
                case VK_LEFT:
                    JumpTo(static_cast<long long>(m_llStartTime - cControls.dFwdBackSecs * 1000000));
                    return Success;
                case VK_RIGHT:
                    JumpTo(static_cast<long long>(m_llStartTime + cControls.dFwdBackSecs * 1000000));
                    return Success;
                case 'M':
                    cPlayback.ToggleMute( true );
                    return Success;
            }
            break;
        }
        case WM_DEVICECHANGE:
            if (!cViz.bKDMAPI) {
                if (cAudio.iOutDevice >= 0 && m_OutDevice.GetDevice() != cAudio.vMIDIOutDevices[cAudio.iOutDevice])
                    m_OutDevice.Open(cAudio.iOutDevice);
            }
            break;
        case TBM_SETPOS:
        {
            long long llFirstTime = GetMinTime();
            long long llLastTime = GetMaxTime();
            JumpTo(llFirstTime + ((llLastTime - llFirstTime) * lParam) / 1000, false);
            break;
        }
        case WM_LBUTTONDOWN:
        {
            if ( m_bZoomMove )
            {
                m_ptLastPos.x = ( SHORT )LOWORD( lParam );
                m_ptLastPos.y = ( SHORT )HIWORD( lParam );
                m_bTrackPos = true;
            }
            return Success;
        }
        case WM_RBUTTONDOWN:
        {
            if ( !m_bZoomMove ) return Success;
            m_ptLastPos.x = ( SHORT )LOWORD( lParam );
            m_ptLastPos.y = ( SHORT )HIWORD( lParam );
            m_ptStartZoom.x = static_cast< int >( ( m_ptLastPos.x - m_fOffsetX - m_fTempOffsetX ) / ( m_fZoomX * m_fTempZoomX ) );
            m_ptStartZoom.y = static_cast< int >( m_ptLastPos.y - m_fOffsetY - m_fTempOffsetY );
            m_bTrackZoom = true;
            return Success;
        }
        case WM_CAPTURECHANGED:
            m_bTrackPos = m_bTrackZoom = false;
            return Success;
        case WM_LBUTTONUP:
            m_bTrackPos = false;
            return Success;
        case WM_RBUTTONUP:
            m_bTrackZoom = false;
            return Success;
        case WM_MOUSEMOVE:
        {
            if ( !m_bTrackPos && !m_bTrackZoom && !m_bPaused ) return Success;
            short x = LOWORD( lParam );
            short y = HIWORD( lParam );
            short dx = static_cast< short >( x - m_ptLastPos.x );
            short dy = static_cast< short >( y - m_ptLastPos.y );

            if ( m_bTrackPos )
            {
                m_fTempOffsetX += dx;
                m_fTempOffsetY += dy;
            }
            if ( m_bTrackZoom )
            {
                float fOldX = m_fOffsetX + m_fTempOffsetX + m_ptStartZoom.x * m_fZoomX * m_fTempZoomX;
                m_fTempZoomX *= pow( 2.0f, dx / 200.0f );
                float fNewX = m_fOffsetX + m_fTempOffsetX + m_ptStartZoom.x * m_fZoomX * m_fTempZoomX;
                m_fTempOffsetX = m_fTempOffsetX - ( fNewX - fOldX );
            }

            m_ptLastPos.x = x;
            m_ptLastPos.y = y;
            return Success;
        }
    }

    return Success;
}

GameState::GameError MainScreen::Logic( void )
{
    if (m_bDiscarded)
    {
        // Song data is freed; keep the ImGui loading modal animating while the
        // UI thread parses the next file.
        m_pRenderer->ImGuiStartFrame();
        return Success;
    }
    // Start new ImGui frame
    m_pRenderer->ImGuiStartFrame();

    static Config &config = Config::GetConfig();
    static PlaybackSettings &cPlayback = config.GetPlaybackSettings();
    static const ViewSettings &cView = config.GetViewSettings();
    static const VisualSettings &cVisual = config.GetVisualSettings();
    static const VideoSettings &cVideo = config.GetVideoSettings();
    static const AudioSettings &cAudio = config.GetAudioSettings();
    static const VizSettings &cViz = config.GetVizSettings();
    const MIDI::MIDIInfo &mInfo = m_MIDI.GetInfo();

    // Custom audio (user-provided wav/mp3/ogg) fully bypasses the prerender; the
    // MIDI device is skipped too so the file is the only audible track.
    const bool bCustomAudio = m_bUseCustomAudio && !m_sCustomAudioPath.empty();
    {
        static bool s_bBranchLogged = false;
        if (!s_bBranchLogged)
        {
            s_bBranchLogged = true;
            PRE_DbgLog("Logic: bCustomAudio=%d bPreAudio=%d bLiveAudio=%d prerenderCfg=%d path='%ls' firstNote=%.2f", (int)bCustomAudio, (int)(!bCustomAudio && cAudio.bPreRenderAudio), (int)(!bCustomAudio && !cAudio.bPreRenderAudio), (int)cAudio.bPreRenderAudio, m_sCustomAudioPath.c_str(), (double)mInfo.llFirstNote / 1e6);
        }
    }
    const bool bPreAudioCfg = !bCustomAudio && cAudio.bPreRenderAudio;
    const bool bPreAudio = bPreAudioCfg && !s_bPreFailed;
    const bool bLiveAudio = !bCustomAudio && !bPreAudioCfg;

    // people are probably going to yell at me if you can't change the bar color during playback
    m_csKBRed.SetColor(cViz.iBarColor, 0.5f);

    if (cViz.bKDMAPI != m_OutDevice.IsKDMAPI()) {
        if (cViz.bKDMAPI)
            m_OutDevice.OpenKDMAPI();
        else if (cAudio.iOutDevice >= 0)
            m_OutDevice.Open(cAudio.iOutDevice);
        m_OutDevice.Reset();
    }

    bool bPaused = cPlayback.GetPaused();
    double dSpeed = cPlayback.GetSpeed();
    double dNSpeed = cPlayback.GetNSpeed();
    bool bMute = cPlayback.GetMute();
    long long llTimeSpan = static_cast< long long >( 3.0 * dNSpeed * 1000000 );
    bool bPausedChanged = ( bPaused != m_bPaused );
    bool bMuteChanged = ( bMute != m_bMute );
    
    // Set the state
    m_bTickMode = cViz.bTickBased;
    m_bPaused = bPaused;
    m_dSpeed = dSpeed;
    m_bMute = bMute;
    m_llTimeSpan = m_bTickMode ? dNSpeed * 3000 : llTimeSpan;
    m_dVolume = cPlayback.GetVolume();
    m_bShowKB = cView.GetKeyboard();
    m_bZoomMove = cView.GetZoomMove();
    m_fOffsetX = cView.GetOffsetX();
    m_fOffsetY = cView.GetOffsetY();
    m_fZoomX = cView.GetZoomX();
    if ( !m_bZoomMove ) m_bTrackPos = m_bTrackZoom = false;
    m_eKeysShown = cVisual.eKeysShown;
    m_eTransitionSpeed = cVisual.eTransitionSpeed;
    m_iStartNote = min( cVisual.iFirstKey, cVisual.iLastKey );
    m_iEndNote = max( cVisual.iFirstKey, cVisual.iLastKey );
    m_bShowFPS = cVideo.bShowFPS;
    if (m_bDumpFrames || m_bRenderVideo)
        m_pRenderer->SetLimitFPS(false);
    else if (m_Timer.m_bManualTimer)
        m_pRenderer->SetLimitFPS(true);
    else
        m_pRenderer->SetLimitFPS( cVideo.bLimitFPS );
    if ( cVisual.iBkgColor != m_csBackground.iOrigBGR ) m_csBackground.SetColor( cVisual.iBkgColor, 0.7f, 1.3f );

    double dMaxCorrect = ( mInfo.iMaxVolume > 0 ? 127.0 / mInfo.iMaxVolume : 1.0 );
    double dVolumeCorrect = ( mInfo.iVolumeSum > 0 ? ( m_dVolume * 127.0 * mInfo.iNoteCount ) / mInfo.iVolumeSum : 1.0 );
    dVolumeCorrect = min( dVolumeCorrect, dMaxCorrect );

    m_bAnyChannelMuted = false;
    for (auto& track : m_vTrackSettings) {
        for (auto& chan : track.aChannels) {
            if (chan.bMuted)
                m_bAnyChannelMuted = true;
        }
    }

    if (cViz.eMarkerEncoding != m_iCurEncoding) {
        m_iCurEncoding = cViz.eMarkerEncoding;
        ApplyMarker(m_pMarkerData, m_iMarkerSize);
    }

    // Time stuff
    long long llMaxTime = GetMaxTime();
    long long llElapsed = m_Timer.GetMicroSecs();
    long long llRealElapsed = m_RealTimer.GetMicroSecs();
    m_Timer.Start();
    m_RealTimer.Start();

    {
        static auto s_tPlayLog = std::chrono::steady_clock::now();
        auto tNowPlay = std::chrono::steady_clock::now();
        if ( std::chrono::duration_cast<std::chrono::milliseconds>(tNowPlay - s_tPlayLog).count() >= 1000 )
        {
            s_tPlayLog = tNowPlay;
            char buf[192];
            long long llMidTime = -1;
            if (m_vEvents.size() > 0)
                llMidTime = m_MIDI.GetEventTime(m_vEvents[m_vEvents.size() / 2]);
            sprintf_s(buf, "play:paused=%d start=%.3fs disp=%.3fs max=%.3fs events=%zu startpos=%lld endpos=%lld midtime=%.3fs",
                (int)m_bPaused, (double)m_llStartTime / 1e6, (double)m_llDisplayTime / 1e6,
                (double)llMaxTime / 1e6, m_vEvents.size(), (long long)m_iStartPos, (long long)m_iEndPos,
                (double)llMidTime / 1e6);
            HeartbeatLog(buf);
        }
    }

    m_llFPSTime += llRealElapsed;
    m_iFPSCount++;
    if ( m_llFPSTime >= 500000 )
    {
        m_dFPS = m_iFPSCount / ( m_llFPSTime / 1000000.0 );
        m_llMaxLateMicros = max( m_llMaxLateMicros, m_llFrameMaxLate );
        m_ullLateEvents += m_ullFrameLateCount;
        m_llFPSTime = m_iFPSCount = m_llFrameMaxLate = 0;
        m_ullFrameLateCount = 0;
    }

    if ( ( bPausedChanged || bMuteChanged ) && ( m_bPaused || m_bMute ) )
        m_OutDevice.AllNotesOff();

    long long llOldStartTime = m_llStartTime;
    long long llNextStartTime = 0;
    if ( bPreAudio )
        llNextStartTime = m_llStartTime + static_cast< long long >( llElapsed * m_dSpeed + 0.5 );
    else
        llNextStartTime = m_llStartTime + static_cast< long long >( min( llElapsed * m_dSpeed, 100000.0 ) + 0.5 );

    if ( !m_bPaused && m_llStartTime < llMaxTime )
    {
        // advance to a fixed slew would break speed changes; instead cap the
        const long long kDeadZone = 20000;   // ignore drift smaller than 20ms
        const long long kMaxCatchUp = 10000; // extra catch-up at most 10ms per frame
        if ( bPreAudio && config.GetAudioSettings().bPreStutterOnLag && !m_bRenderVideo
             && m_bAudioStarted && PRE_MIDIAudio && PRE_MIDIAudio->IsAudioStarted() )
        {
            long long llAudioTime = (long long)( PRE_MIDIAudio->GetPlayerTime() * 1000000.0 );
            if ( llAudioTime >= 0 && llAudioTime <= llMaxTime )
            {
                long long llAhead = llAudioTime - llNextStartTime;
                if ( llAhead > kDeadZone )
                {
                    llNextStartTime += min( llAhead - kDeadZone, kMaxCatchUp );
                }
                else if ( llAhead < -kDeadZone )
                {
                    llNextStartTime = min( llNextStartTime, llAudioTime + kDeadZone );
                }
                m_llStartTime = llNextStartTime;
            }
            else
            {
                m_llStartTime = llNextStartTime;
            }
        }
        else
        {
            m_llStartTime = llNextStartTime;
        }
    }
    if ( !m_bPaused && m_llStartTime < llMaxTime
         && bPreAudio )
        m_llDisplayTime += static_cast< long long >( llElapsed * m_dSpeed + 0.5 );
    else
        m_llDisplayTime = m_llStartTime;
    m_iStartTick = GetCurrentTick( m_llStartTime );
    long long llEndTime = 0;
    if (m_bTickMode)
        llEndTime = m_iStartTick + m_llTimeSpan;
    else
        llEndTime = m_llStartTime + m_llTimeSpan;

    RenderGlobals();

    auto iEventCount = (int64_t)m_vEvents.size();
    if (m_bTickMode) {
        while (m_iEndPos + 1 < iEventCount && m_MIDI.GetEventAbsT(m_vEvents[m_iEndPos + 1]) < llEndTime)
            m_iEndPos++;
    } else {
        while (m_iEndPos + 1 < iEventCount && m_MIDI.GetEventTime(m_vEvents[m_iEndPos + 1]) < llEndTime)
            m_iEndPos++;
    }

    // Pre-rendered audio mode drives the playback clock; skip the MIDI device.
    // With custom audio the user's file replaces the prerender entirely.
    if (bCustomAudio)
    {
        if (PRE_MIDIAudio)
        {
            SDL_PauseAudio(1); // silence any leftover prerender/SDL output
            static bool s_bPausedLogged = false;
            if (!s_bPausedLogged) { s_bPausedLogged = true; PRE_DbgLog("CUSTOM: SDL paused"); }
        }
        CustomAudioUpdate(m_sCustomAudioPath, m_llStartTime, mInfo.llFirstNote, m_bPaused, m_bMute, m_dVolume);
    }
    else
        UpdatePreRenderAudio(m_vEvents, m_MIDI, m_llStartTime, m_iStartPos, m_bAudioStarted);

    if ( !m_bPaused )
    {
        long long notes_played = 0;
        long long events_processed = 0;
        float fCorruptor = GetCorruptorAmount();
        m_pRenderer->m_fLastCorruption = fCorruptor;
        for (auto& work : m_vThreadWork)
            work.clear();
        while ( m_iStartPos < iEventCount && m_MIDI.GetEventTime(m_vEvents[m_iStartPos]) <= m_llStartTime )
        {
            MIDIChannelEvent pEvent = m_vEvents[m_iStartPos];

            int iCorruptPitch = m_MIDI.GetEventParam1(pEvent);
            int iCorruptValue = m_MIDI.GetEventParam2(pEvent);
            uint64_t hCorrupt = 0;
            switch (m_MIDI.GetEventChannelEventType(pEvent))
            {
            case MIDI::NoteOn:
            case MIDI::NoteOff:
                hCorrupt = CorruptSeed(pEvent, m_MIDI.EventHasSister(pEvent) ? m_vEvents[m_MIDI.GetEventSisterIdx(pEvent)] : UINT32_MAX);
                iCorruptPitch = CorruptPitch(fCorruptor, hCorrupt, iCorruptPitch);
                break;
            case MIDI::Controller:
            case MIDI::ProgramChange:
                hCorrupt = CorruptSeed(pEvent, UINT32_MAX);
                iCorruptPitch = CorruptValue(fCorruptor, hCorrupt, iCorruptPitch);
                break;
            case MIDI::PitchBend:
                hCorrupt = CorruptSeed(pEvent, UINT32_MAX);
                iCorruptValue = CorruptPitchBend(fCorruptor, hCorrupt,
                    (m_MIDI.GetEventParam2(pEvent) << 7) | m_MIDI.GetEventParam1(pEvent));
                break;
            case MIDI::NoteAftertouch:
            case MIDI::ChannelAftertouch:
                hCorrupt = CorruptSeed(pEvent, UINT32_MAX);
                iCorruptPitch = CorruptPitch(fCorruptor, hCorrupt, iCorruptPitch);
                iCorruptValue = CorruptValue(fCorruptor, hCorrupt, iCorruptValue);
                break;
            }

            if (m_MIDI.GetEventChannelEventType(pEvent) != MIDI::NoteOn) {
                if (m_MIDI.GetEventChannelEventType(pEvent) == MIDI::ProgramChange &&
                    m_MIDI.GetEventChannel(pEvent) != MIDI::Drums && config.m_bPianoOverride)
                    iCorruptPitch = 0; // keep the piano override intact
                if (m_MIDI.GetEventChannelEventType(pEvent) == MIDI::PitchBend) {
                    m_pBends[m_MIDI.GetEventChannel(pEvent)] = (notex_table[1] - notex_table[0]) *
                        (((short)(iCorruptValue - 8192)) / (8192.0f / 12.0f));
                    if (bLiveAudio || s_bPreFailed)
                        m_OutDevice.PlayEvent(m_MIDI.GetEventCode(pEvent), iCorruptValue & 0x7F, iCorruptValue >> 7);
                }
                else if (bLiveAudio || s_bPreFailed)
                    m_OutDevice.PlayEvent(m_MIDI.GetEventCode(pEvent), iCorruptPitch, iCorruptValue);
            }
            else if (!m_bMute && (!m_bAnyChannelMuted || !m_vTrackSettings[m_MIDI.GetEventTrack(pEvent)].aChannels[m_MIDI.GetEventChannel(pEvent)].bMuted)) {
                if (bLiveAudio || s_bPreFailed)
                    m_OutDevice.PlayEvent(m_MIDI.GetEventCode(pEvent), iCorruptPitch,
                        static_cast<int>(CorruptVelocity(fCorruptor, hCorrupt, m_MIDI.GetEventParam2(pEvent)) * dVolumeCorrect + 0.5));
                notes_played++;
            }
            if ((m_MIDI.GetEventChannelEventType(pEvent) == MIDI::NoteOn || m_MIDI.GetEventChannelEventType(pEvent) == MIDI::NoteOff)
                && iCorruptPitch < 128 && m_MIDI.EventHasSister(pEvent))
            {
                m_vThreadWork[iCorruptPitch].push_back({
                    .idx = (unsigned)m_iStartPos,
                    .sister_idx = (m_MIDI.GetEventChannelEventType(pEvent) == MIDI::NoteOn && m_MIDI.GetEventParam2(pEvent) > 0) ? ~0 : m_MIDI.GetEventSisterIdx(pEvent),
                });
            }
            m_iStartPos++;
            events_processed++;

            long long llLateness = m_llStartTime - m_MIDI.GetEventTime(pEvent);
            if (llLateness > m_llFrameMaxLate)
                m_llFrameMaxLate = llLateness;
            if (llLateness > 100000)
                m_ullFrameLateCount++;
        }

        if (events_processed < 131072) {
            for (int i = 0; i < 128; i++) {
                for (const auto& work : m_vThreadWork[i])
                    UpdateState(i, work);
            }
        }
        else {
            concurrency::parallel_for(size_t(0), size_t(128), [&](int key) {
                for (const auto& work : m_vThreadWork[key])
                    UpdateState(key, work);
            });
        }

        for (; !m_dNPSNotes.empty(); m_dNPSNotes.pop_front()) {
            if (std::get<0>(m_dNPSNotes.front()) >= m_llStartTime - 1000000)
                break;
        }
        if (notes_played != 0)
            m_dNPSNotes.push_back(std::make_tuple(m_llStartTime, notes_played));

        long long lagNps = 0;
        for (size_t i = 0; i < m_dNPSNotes.size(); i++)
            lagNps += std::get<1>(m_dNPSNotes[i]);
        m_pRenderer->SetLagNPS(lagNps);

        m_dNPSHistory.push_back(lagNps);
        if ((int)m_dNPSHistory.size() > 600)
            m_dNPSHistory.pop_front();
    }

    AdvanceIterators( m_llStartTime, false );

    long long llFirstTime = GetMinTime();
    long long llLastTime = GetMaxTime();
    long long llOldPos = ( ( llOldStartTime - llFirstTime ) * 1000 ) / ( llLastTime - llFirstTime );
    long long llNewPos = ( ( m_llStartTime - llFirstTime ) * 1000 ) / ( llLastTime - llFirstTime );
    if ( llOldPos != llNewPos ) cPlayback.SetPosition( static_cast< int >( llNewPos ) );

    if (!m_bPaused && m_llStartTime >= llMaxTime) {
        if (m_bDumpFrames)
            CloseHandle(m_hVideoPipe);
        if (m_bRenderVideo)
            FinishVideoRender();
        cPlayback.SetPaused(true, true);
    }

    if (m_Timer.m_bManualTimer)
        m_Timer.IncrementFrame();

    auto& root_consts = m_pRenderer->GetRootConstants();
    root_consts.deflate = clamp(round(m_fWhiteCX * 0.15f / 2.0f), 1.0f, 3.0f);
    root_consts.notes_y = m_fNotesY;
    root_consts.notes_cy = m_fNotesCY;
    root_consts.white_cx = m_fWhiteCX;
    root_consts.timespan = (float)m_llTimeSpan;
    root_consts.notes_x = m_fNotesX;
    root_consts.notes_cx = m_fNotesCX;

    auto& fixed_consts = m_pRenderer->GetFixedSizeConstants();
    memcpy(&fixed_consts.note_x, &notex_table, sizeof(float) * 128);
    if (cViz.bVisualizePitchBends)
        memcpy(&fixed_consts.bends, &m_pBends, sizeof(float) * 16);
    else
        memset(&fixed_consts.bends, 0, sizeof(float) * 16);

    SyncTrackColors(m_pRenderer, m_vTrackSettings);

    return Success;
}

void MainScreen::UpdateState(int key, const thread_work_t& work)
{
    auto& note_state = m_vState[key];
    if (work.sister_idx == UINT32_MAX) {
        note_state.push_back(work.idx);
        m_pNoteState[key] = work.idx;
    } else {
        auto pos = sse_bin_search(note_state, work.sister_idx);
        if (pos != -1)
            note_state.erase(note_state.begin() + pos);

        if (note_state.size() == 0)
            m_pNoteState[key] = -1;
        else
            m_pNoteState[key] = note_state.back();
    }
}

void MainScreen::JumpTo(long long llStartTime, bool bUpdateGUI)
{
    m_dNPSNotes.clear();
    m_dNPSHistory.clear();

    if (Config::GetConfig().GetAudioSettings().bPreRenderAudio && PRE_MIDIAudio)
    {
        PRE_DbgLog("JumpTo: reset start t=%.2f r=%.2f w=%.2f", (double)llStartTime / 1e6, (double)PRE_MIDIAudio->GetPlayerTime(), (double)PRE_MIDIAudio->GetBufferSeconds());
        m_bAudioStarted = false;
    }

    m_OutDevice.AllNotesOff();

    long long llFirstTime = GetMinTime();
    long long llLastTime = GetMaxTime();
    m_llStartTime = min(max(llStartTime, llFirstTime), llLastTime);
    m_llDisplayTime = m_llStartTime;
    if (m_llStartTime <= llFirstTime)
    {
        m_fKeysTransition = 0.0f;
        s_fTransitionPhase = 0.0f;
        s_LastTransitionFrame = std::chrono::steady_clock::now();
    }
    long long llEndTime = m_llStartTime + m_llTimeSpan;

    auto itBegin = m_vEvents.begin();
    auto itEnd = m_vEvents.end();
    auto itMiddle = lower_bound(itBegin, itEnd, llStartTime, [&](MIDIChannelEvent lhs, const long long rhs) {
        return m_MIDI.GetEventTime(lhs) < rhs;
    });

    m_iStartPos = (long long)m_vEvents.size();
    if (itMiddle != itEnd && itMiddle - m_vEvents.begin() < m_iStartPos)
        m_iStartPos = itMiddle - m_vEvents.begin();

    for (; itMiddle != itEnd; itMiddle++) {
        if (m_MIDI.GetEventChannelEventType(*itMiddle) == MIDI::NoteOn && m_MIDI.GetEventParam2(*itMiddle) > 0)
            break;
    }

    for (auto& note_state : m_vState)
        note_state.clear();
    memset(m_pNoteState, -1, sizeof(m_pNoteState));
    if (itMiddle != itBegin)
    {
        auto itPrev = itMiddle - 1;
        for (; itPrev != itBegin; itPrev--) {
            if (m_MIDI.GetEventChannelEventType(*itPrev) == MIDI::NoteOn && m_MIDI.GetEventParam2(*itPrev) > 0)
                break;
        }

        unsigned iFound = 0;

        auto itClusterEnd = itMiddle;
        long long llClusterTime = m_MIDI.GetEventTime(*itClusterEnd);
        while (itClusterEnd + 1 != itEnd && m_MIDI.GetEventTime(*(itClusterEnd + 1)) == llClusterTime)
            itClusterEnd++;

        unsigned iSimultaneous = m_MIDI.GetEventSimult(*itPrev) + 1;
        for (std::vector<MIDIChannelEvent>::reverse_iterator it(itClusterEnd + 1); iFound < iSimultaneous && it != m_vEvents.rend(); ++it)
        {
            auto idx = m_vEvents.size() - 1 - (it - m_vEvents.rbegin());
            MIDIChannelEvent pEvent = m_vEvents[idx];
            if (m_MIDI.GetEventChannelEventType(pEvent) == MIDI::NoteOn && m_MIDI.GetEventParam2(pEvent) > 0 && m_MIDI.EventHasSister(pEvent)) {
                MIDIChannelEvent pSister = m_vEvents[m_MIDI.GetEventSisterIdx(pEvent)];
                if (m_MIDI.GetEventTime(pSister) > m_MIDI.GetEventTime(*itPrev)) // > because itMiddle is the max for its time
                    iFound++;
                if (m_MIDI.GetEventTime(pSister) > llStartTime) // > because we don't care about simultaneous ending notes
                {
                    if (m_MIDI.GetEventTime(pEvent) >= llStartTime)
                        continue;
                    int iCorruptPitch = CorruptPitch(GetCorruptorAmount(),
                        CorruptSeed(pEvent, pSister), m_MIDI.GetEventParam1(pEvent));
                    (m_vState[iCorruptPitch]).push_back(idx);
                    if (m_pNoteState[iCorruptPitch] < 0)
                        m_pNoteState[iCorruptPitch] = idx;
                }
            }
        }
        for (auto& note_state : m_vState)
            reverse(note_state.begin(), note_state.end());
    }

    m_iEndPos = m_iStartPos - 1;
    auto iEventCount = (long long)m_vEvents.size();
    while (m_iEndPos + 1 < iEventCount && m_MIDI.GetEventTime(m_vEvents[m_iEndPos + 1]) < llEndTime)
        m_iEndPos++;

    eventvec_t::const_iterator itOldProgramChange = m_itNextProgramChange;
    AdvanceIterators(llStartTime, true);
    PlaySkippedEvents(itOldProgramChange);
    m_iStartTick = GetCurrentTick(m_llStartTime);

    if (bUpdateGUI)
    {
        static PlaybackSettings& cPlayback = Config::GetConfig().GetPlaybackSettings();
        long long llNewPos = ((m_llStartTime - llFirstTime) * 1000) / (llLastTime - llFirstTime);
        cPlayback.SetPosition(static_cast<int>(llNewPos));
    }
}

void MainScreen::PlaySkippedEvents(eventvec_t::const_iterator itOldProgramChange)
{
    if (itOldProgramChange == m_itNextProgramChange)
        return;

    bool aControl[16][128], aProgram[16], aPitchBend[16];
    bool bPianoOverride = Config::GetConfig().m_bPianoOverride;
    memset(aControl, 0, sizeof(aControl));
    memset(aProgram, 0, sizeof(aProgram));
    memset(aPitchBend, 0, sizeof(aPitchBend));

    vector< MIDIChannelEvent > vControl;
    eventvec_t::const_reverse_iterator itBegin = eventvec_t::const_reverse_iterator(m_itNextProgramChange);
    eventvec_t::const_reverse_iterator itEnd = m_vProgramChange.rend();
    if (itOldProgramChange < m_itNextProgramChange) itEnd = eventvec_t::const_reverse_iterator(itOldProgramChange);

    float fCorrupt = GetCorruptorAmount();
    for (eventvec_t::const_reverse_iterator it = itBegin; it != itEnd; ++it)
    {
        MIDIChannelEvent pEvent = m_vEvents[it->second];
        if (m_MIDI.GetEventChannelEventType(pEvent) == MIDI::Controller &&
            !aControl[m_MIDI.GetEventChannel(pEvent)][m_MIDI.GetEventParam1(pEvent)])
        {
            aControl[m_MIDI.GetEventChannel(pEvent)][m_MIDI.GetEventParam1(pEvent)] = true;
            vControl.push_back(m_vEvents[it->second]);
        }
        else if (m_MIDI.GetEventChannelEventType(pEvent) == MIDI::ProgramChange &&
            !aProgram[m_MIDI.GetEventChannel(pEvent)])
        {
            aProgram[m_MIDI.GetEventChannel(pEvent)] = true;
            int iProgram = m_MIDI.GetEventParam1(pEvent);
            if (m_MIDI.GetEventChannel(pEvent) != MIDI::Drums && bPianoOverride)
                iProgram = 0;
            else
                iProgram = CorruptValue(fCorrupt, CorruptSeed(pEvent, UINT32_MAX), iProgram);
            m_OutDevice.PlayEvent(m_MIDI.GetEventCode(pEvent), iProgram, m_MIDI.GetEventParam2(pEvent));
        }
        else if (m_MIDI.GetEventChannelEventType(pEvent) == MIDI::PitchBend &&
            !aPitchBend[m_MIDI.GetEventChannel(pEvent)])
        {
            aPitchBend[m_MIDI.GetEventChannel(pEvent)] = true;
            int iBend = CorruptPitchBend(fCorrupt, CorruptSeed(pEvent, UINT32_MAX),
                (m_MIDI.GetEventParam2(pEvent) << 7) | m_MIDI.GetEventParam1(pEvent));
            m_pBends[m_MIDI.GetEventChannel(pEvent)] = (notex_table[1] - notex_table[0]) *
                (((short)(iBend - 8192)) / (8192.0f / 12.0f));
            m_OutDevice.PlayEvent(m_MIDI.GetEventCode(pEvent), iBend & 0x7F, iBend >> 7);
        }
    }

    for (vector< MIDIChannelEvent >::reverse_iterator it = vControl.rbegin(); it != vControl.rend(); ++it)
    {
        MIDIChannelEvent pEvent = *it;
        m_OutDevice.PlayEvent(m_MIDI.GetEventCode(pEvent), m_MIDI.GetEventParam1(pEvent),
            CorruptValue(fCorrupt, CorruptSeed(pEvent, UINT32_MAX), m_MIDI.GetEventParam2(pEvent)));
    }
}

void MainScreen::ApplyMarker(unsigned char* data, size_t size) {
    m_pMarkerData = data;
    m_iMarkerSize = size;
    if (data) {
        Config& config = Config::GetConfig();
        VizSettings viz = config.GetVizSettings();

        constexpr int codepages[] = {1252, 932, CP_UTF8};

        auto temp_str = new char[size + 1];
        memcpy(temp_str, data, size);
        temp_str[size] = '\0';
        
        if (codepages[viz.eMarkerEncoding] != CP_UTF8) {
            auto wide_len = MultiByteToWideChar(codepages[viz.eMarkerEncoding], 0, temp_str, size + 1, NULL, 0);
            auto wide_temp_str = new WCHAR[wide_len];
            MultiByteToWideChar(codepages[viz.eMarkerEncoding], 0, temp_str, size + 1, wide_temp_str, wide_len);

            auto utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide_temp_str, -1, 0, 0, 0, 0);
            auto utf8_temp_str = new char[utf8_len];
            WideCharToMultiByte(CP_UTF8, 0, wide_temp_str, -1, utf8_temp_str, utf8_len, 0, 0);

            m_sMarker = std::string(utf8_temp_str);
            delete[] wide_temp_str;
            delete[] utf8_temp_str;
        } else {
            m_sMarker = temp_str;
        }

        if (m_sMarker == "Setup" || m_sMarker == "Start")
            m_sMarker = std::string();

        delete[] temp_str;
    } else {
        m_sMarker = std::string();
    }
}

void MainScreen::AdvanceIterators( long long llTime, bool bIsJump )
{
    if ( bIsJump )
    {
        m_itNextProgramChange = upper_bound( m_vProgramChange.begin(), m_vProgramChange.end(), pair< long long, int >( llTime, m_vEvents.size() ) );

        m_itNextTempo = upper_bound( m_vTempo.begin(), m_vTempo.end(), pair< long long, int >( llTime, m_vMetaEvents.size() ) );
        MIDIMetaEvent *pPrevious = GetPrevious( m_itNextTempo, m_vTempo, 3 );
        if ( pPrevious )
        {
            MIDI::Parse24Bit( pPrevious->GetData(), 3, &m_iMicroSecsPerBeat );
            m_iLastTempoTick = pPrevious->GetAbsT();
            m_llLastTempoTime = pPrevious->GetAbsMicroSec();
        }
        else
        {
            m_iMicroSecsPerBeat = 500000;
            m_llLastTempoTime = m_iLastTempoTick = 0;
        }

        m_itNextSignature = upper_bound( m_vSignature.begin(), m_vSignature.end(), pair< long long, int >( llTime, m_vMetaEvents.size() ) );
        pPrevious = GetPrevious( m_itNextSignature, m_vSignature, 4 );
        if ( pPrevious )
        {
            m_iBeatsPerMeasure = pPrevious->GetData()[0];
            m_iBeatType = 1 << pPrevious->GetData()[1];
            m_iClocksPerMet = pPrevious->GetData()[2];
            m_iLastSignatureTick = pPrevious->GetAbsT();
        }
        else
        {
            m_iBeatsPerMeasure = 4;
            m_iBeatType = 4;
            m_iClocksPerMet = 24;
            m_iLastSignatureTick = 0;
        }

        auto itCurMarker = m_itNextMarker;
        m_itNextMarker = upper_bound(m_vMarkers.begin(), m_vMarkers.end(), pair< long long, int >(llTime, m_vMetaEvents.size()));
        if (!m_bNextMarkerInited || itCurMarker != m_itNextMarker) {
            m_bNextMarkerInited = true;
            if (m_itNextMarker != m_vMarkers.begin() && (m_itNextMarker - 1)->second != -1) {
                const auto eEvent = m_vMetaEvents[(m_itNextMarker - 1)->second];
                ApplyMarker(eEvent->GetData(), eEvent->GetDataLen());
            }
            else {
                ApplyMarker(nullptr, 0);
            }
        }
    }
    else
    {
        while ( m_itNextProgramChange != m_vProgramChange.end() && m_itNextProgramChange->first <= llTime )
            ++m_itNextProgramChange;
        for ( ; m_itNextTempo != m_vTempo.end() && m_itNextTempo->first <= llTime; ++m_itNextTempo )
        {
            MIDIMetaEvent *pEvent = m_vMetaEvents[m_itNextTempo->second];
            if ( pEvent->GetDataLen() == 3 )
            {
                MIDI::Parse24Bit( pEvent->GetData(), 3, &m_iMicroSecsPerBeat );
                m_iLastTempoTick = pEvent->GetAbsT();
                m_llLastTempoTime = pEvent->GetAbsMicroSec();
            }
        }
        for ( ; m_itNextSignature != m_vSignature.end() && m_itNextSignature->first <= llTime; ++m_itNextSignature )
        {
            MIDIMetaEvent *pEvent = m_vMetaEvents[m_itNextSignature->second];
            if ( pEvent->GetDataLen() == 4 )
            {
                m_iBeatsPerMeasure = pEvent->GetData()[0];
                m_iBeatType = 1 << pEvent->GetData()[1];
                m_iClocksPerMet = pEvent->GetData()[2];
                m_iLastSignatureTick = pEvent->GetAbsT();
            }
        }
        auto itCurMarker = m_itNextMarker;
        while (m_itNextMarker != m_vMarkers.end() && m_itNextMarker->first <= llTime)
            ++m_itNextMarker;
        if (itCurMarker != m_itNextMarker) {
            if (m_itNextMarker != m_vMarkers.begin() && (m_itNextMarker - 1)->second != -1) {
                const auto eEvent = m_vMetaEvents[(m_itNextMarker - 1)->second];
                ApplyMarker(eEvent->GetData(), eEvent->GetDataLen());
            } else {
                ApplyMarker(nullptr, 0);
            }
        }
    }
}

MIDIMetaEvent* MainScreen::GetPrevious( eventvec_t::const_iterator &itCurrent,
                                        const eventvec_t &vEventMap, int iDataLen )
{
    const MIDI::MIDIInfo &mInfo = m_MIDI.GetInfo();
    eventvec_t::const_iterator it = itCurrent;
    if ( itCurrent != vEventMap.begin() )
    {
        while ( it != vEventMap.begin() )
            if ( m_vMetaEvents[( --it )->second]->GetDataLen() == iDataLen )
                return m_vMetaEvents[it->second];
    }
    else if ( vEventMap.size() > 0 && itCurrent->first <= mInfo.llFirstNote && m_vMetaEvents[itCurrent->second]->GetDataLen() == iDataLen )
    {
        MIDIMetaEvent *pPrevious = m_vMetaEvents[itCurrent->second];
        ++itCurrent;
        return pPrevious;
    }
    return NULL;
}

int  MainScreen::GetCurrentTick( long long llStartTime )
{
    return GetCurrentTick( llStartTime, m_iLastTempoTick, m_llLastTempoTime, m_iMicroSecsPerBeat );
}

int  MainScreen::GetCurrentTick( long long llStartTime, int iLastTempoTick, long long llLastTempoTime, int iMicroSecsPerBeat )
{
    int iDivision = m_MIDI.GetInfo().iDivision;
    if ( !( iDivision & 0x8000 ) )
    {
        if ( llStartTime >= llLastTempoTime )
            return iLastTempoTick + static_cast< int >( ( iDivision * ( llStartTime - llLastTempoTime ) ) / iMicroSecsPerBeat );
        else 
            return iLastTempoTick - static_cast< int >( ( iDivision * ( llLastTempoTime - llStartTime ) + 1 ) / iMicroSecsPerBeat ) - 1;
    }
    return -1;
}

long long MainScreen::GetTickTime( int iTick )
{
    return GetTickTime( iTick, m_iLastTempoTick, m_llLastTempoTime, m_iMicroSecsPerBeat );
}

long long MainScreen::GetTickTime( int iTick, int iLastTempoTick, long long llLastTempoTime, int iMicroSecsPerBeat )
{
    int iDivision = m_MIDI.GetInfo().iDivision;
    if ( !( iDivision & 0x8000 ) )
        return llLastTempoTime + ( static_cast< long long >( iMicroSecsPerBeat ) * ( iTick - iLastTempoTick ) ) / iDivision;
    //else
    // return llLastTempoTime + ( 1000000LL * ( iTick - iLastTempoTick ) ) / iTicksPerSecond;
    return -1;
}

int MainScreen::GetBeat( int iTick, int iBeatType, int iLastSignatureTick )
{
    int iDivision = m_MIDI.GetInfo().iDivision;
    int iTickOffset = iTick - iLastSignatureTick;
    if (!(iDivision & 0x8000))
    {
        m_CurBeat = (iTickOffset * iBeatType) / (iDivision * 4);

        if (iTickOffset > 0)
            return (iTickOffset * iBeatType - 1) / (iDivision * 4) + 1;
        else
            return (iTickOffset * iBeatType) / (iDivision * 4);
    }

    return -1;
}

int MainScreen::GetBeatTick( int iTick, int iBeatType, int iLastSignatureTick )
{
    int iDivision = m_MIDI.GetInfo().iDivision;
    if ( !( iDivision & 0x8000 ) )
        return iLastSignatureTick + ( GetBeat( iTick, iBeatType, iLastSignatureTick ) * iDivision * 4 ) / iBeatType;
    return -1;
}

const float MainScreen::SharpRatio = 0.65f;
const float MainScreen::KBPercent = 0.25f;
const float MainScreen::KeyRatio = 0.1775f;

GameState::GameError MainScreen::Render() 
{
    if (m_bDiscarded)
    {
        // Minimal frame: clear + present the loading modal. Keeps the GPU busy
        // (less idle -> fewer TDR windows) with no song data touched.
        m_pRenderer->ClearAndBeginScene(0x00000000);
        m_pRenderer->EndScene();
        m_pRenderer->Present();
        return Success;
    }
    if ( FAILED( m_pRenderer->ResetDeviceIfNeeded() ) ) return DirectXError;

    static Config& config = Config::GetConfig();
    static const VizSettings& cViz = config.GetVizSettings();
    if (cViz.sBackground != m_sCurBackground || cViz.sBackground.empty()) {
        m_bBackgroundLoaded = cViz.sBackground.empty() ? false : m_pRenderer->LoadBackgroundBitmap(cViz.sBackground);
        m_sCurBackground = cViz.sBackground;
        m_fLastBGBlur = -1.0f;
    }
    if (m_bBackgroundLoaded && cViz.fBGBlur != m_fLastBGBlur) {
        m_pRenderer->SetBackgroundBlur(cViz.fBGBlur);
        m_fLastBGBlur = cViz.fBGBlur;
    }

    {
        static int s_frameLog = 0;
        static double tClear = 0, tLines = 0, tNotes = 0, tRest = 0;
        auto t0 = std::chrono::steady_clock::now();
        m_pRenderer->ClearAndBeginScene( 0x00000000 );
        auto t0b = std::chrono::steady_clock::now();
        RenderLines();
        auto t1 = std::chrono::steady_clock::now();
        RenderNotes();
        auto t2 = std::chrono::steady_clock::now();
        if ( m_bShowKB )
            RenderKeys();
        RenderBorder();
        RenderText();
        auto t3 = std::chrono::steady_clock::now();
        tClear += std::chrono::duration<double, std::milli>(t0b - t0).count();
        tLines += std::chrono::duration<double, std::milli>(t1 - t0b).count();
        tNotes += std::chrono::duration<double, std::milli>(t2 - t1).count();
        tRest += std::chrono::duration<double, std::milli>(t3 - t2).count();
        if ((s_frameLog++ & 127) == 127) {
            char buf[128];
            sprintf_s(buf, "r:clear=%.2f lines=%.2f notes=%.2f rest=%.2f",
                tClear / 128.0, tLines / 128.0, tNotes / 128.0, tRest / 128.0);
            HeartbeatLog(buf);
            tClear = tLines = tNotes = tRest = 0;
        }
    }

    m_pRenderer->SetStripKeyboardColors(
        m_csKBWhite.iPrimaryRGB, m_csKBWhite.iDarkRGB, m_csKBWhite.iVeryDarkRGB,
        m_csKBSharp.iPrimaryRGB, m_csKBSharp.iDarkRGB, m_csKBSharp.iVeryDarkRGB,
        m_csKBBackground.iPrimaryRGB, m_csKBBackground.iDarkRGB);

    {
        DWORD pressedKeys[128] = {};
        DWORD ribbonColors[128] = {};
        for (int i = 0; i < 128; i++) {
            if (m_pNoteState[i] != -1) {
                MIDIChannelEvent pEvent = (m_pNoteState[i] >= 0 ? m_vEvents[m_pNoteState[i]] : UINT32_MAX);
                if (pEvent != UINT32_MAX) {
                    const int iTrack = m_MIDI.GetEventTrack(pEvent) % MaxTrackColors;
                    const int iChannel = m_MIDI.GetEventChannel(pEvent);
                    const ChannelSettings &cs = m_vTrackSettings[iTrack].aChannels[iChannel];
                    pressedKeys[i] = cs.iPrimaryRGB;
                    ribbonColors[i] = cs.iPrimaryRGB;
                }
            }
        }
        m_pRenderer->SetStripPressedKeys(pressedKeys, ribbonColors);
    }

    m_pRenderer->DrawPianoRollStripKeyboard();

    m_pRenderer->SetPlaybackPosition((float)((double)(m_llDisplayTime - GetMinTime()) / (double)(GetMaxTime() - GetMinTime())));
    m_pRenderer->SetPianoRollView(m_iStartNote, m_iEndNote,
        (float)((double)(m_llStartTime - m_llRndStartTime) / (double)m_llTimeSpan));

    {
        static int s_endLog = 0;
        static double tStrip = 0, tEnd = 0;
        auto t0 = std::chrono::steady_clock::now();
        m_pRenderer->DrawPianoRollStripKeyboard();
        auto t1 = std::chrono::steady_clock::now();
        m_pRenderer->EndScene(m_bBackgroundLoaded);
        auto t2 = std::chrono::steady_clock::now();
        m_pRenderer->Present();
        auto t3 = std::chrono::steady_clock::now();
        tStrip += std::chrono::duration<double, std::milli>(t1 - t0).count();
        tEnd += std::chrono::duration<double, std::milli>(t3 - t2).count();
        if ((s_endLog++ & 127) == 127) {
            char buf[96];
            sprintf_s(buf, "r:strip=%.2f present=%.2f", tStrip / 128.0, tEnd / 128.0);
            HeartbeatLog(buf);
            tStrip = tEnd = 0;
        }
    }

    if (m_bDumpFrames) {
        auto* frame = m_pRenderer->Screenshot();

        WriteFile(m_hVideoPipe, frame, static_cast<DWORD>(m_pRenderer->GetBufferWidth() * m_pRenderer->GetBufferHeight() * 4), nullptr, nullptr);

        const std::wstring& name = m_MIDI.GetInfo().sFilename;
        TCHAR sTitle[1024];
        _stprintf_s(sTitle, TEXT("%ws (%.1lf%%)"), name.c_str() + (name.find_last_of(L'\\') + 1), (m_dFPS / m_Timer.m_dFramerate) * 100.0);
        SetMainTitle(sTitle);
    }

    if (m_bRenderVideo) {
        // The raw stream was started with the render resolution; if the window
        // was resized mid-render the capture would corrupt, so finish cleanly.
        if ((int)m_pRenderer->GetBufferWidth() != s_FFCapW || (int)m_pRenderer->GetBufferHeight() != s_FFCapH) {
            FinishVideoRender();
            return Success;
        }
        auto* frame = m_pRenderer->Screenshot();
        if (frame) {
            DWORD dwBytes = static_cast<DWORD>(m_pRenderer->GetBufferWidth() * m_pRenderer->GetBufferHeight() * 4);
            DWORD dwWritten = 0;
            WriteFile(m_hFFPipeWrite, frame, dwBytes, &dwWritten, nullptr);
        }
        const std::wstring& name = m_MIDI.GetInfo().sFilename;
        TCHAR sTitle[1024];
        double dPct = (GetMaxTime() > GetMinTime()) ? (m_llStartTime - GetMinTime()) * 100.0 / (GetMaxTime() - GetMinTime()) : 0.0;
        _stprintf_s(sTitle, TEXT("%ws (%.0f fps, %.1f%%)"), name.c_str() + (name.find_last_of(L'\\') + 1), m_dFPS, dPct);
        SetMainTitle(sTitle);
        SetWindowText(g_hWndGfx, sTitle);

        // Feed the Win32 progress window (throttled to ~10 updates per second):
        // progress, output size, and the render speed the machine achieves.
        static ULONGLONG s_ullLastProgUpdate = 0;
        ULONGLONG ullNow = GetTickCount64();
        if (ullNow - s_ullLastProgUpdate >= 100)
        {
            s_ullLastProgUpdate = ullNow;
            VizSettings& viz2 = Config::GetConfig().GetVizSettings();
            wchar_t sProg[512];
            swprintf_s(sProg, L"Rendering %.1f%%\nOutput: %dx%d @ %d fps\nRender speed: %.0f fps",
                dPct, viz2.iRenderWidth, viz2.iRenderHeight, viz2.iRenderFPS, m_dFPS);
            UpdateRenderProgressWindow((int)(dPct * 10.0 + 0.5), sProg);
        }
    }
    return Success;
}

void MainScreen::RenderGlobals()
{
    m_fNotesX = m_fOffsetX + m_fTempOffsetX;
    m_fNotesCX = m_pRenderer->GetBufferWidth() * m_fZoomX * m_fTempZoomX;
    m_fNotesY = m_fOffsetY + m_fTempOffsetY;

    const MIDI::MIDIInfo &mInfo = m_MIDI.GetInfo();
    if ( m_eKeysShown == VisualSettings::All )
    {
        m_iStartNote = 0;
        m_iEndNote = 127;
        m_fViewStartX = (float)MIDI::WhiteCount(0, 0);
        m_fWhiteCX = m_fNotesCX / (float)MIDI::WhiteCount(0, 128);
    }
    else if ( m_eKeysShown == VisualSettings::Song )
    {
        m_iStartNote = mInfo.iMinNote;
        m_iEndNote = mInfo.iMaxNote;
        float fStartCoord = (float)MIDI::WhiteCount(0, m_iStartNote) + (MIDI::IsSharp(m_iStartNote) ? SharpRatio / 2.0f : 0.0f);
        float fEndCoord = (float)MIDI::WhiteCount(0, m_iEndNote + 1) + (MIDI::IsSharp(m_iEndNote) ? SharpRatio / 2.0f : 0.0f);
        m_fViewStartX = fStartCoord;
        m_fWhiteCX = m_fNotesCX / (fEndCoord - fStartCoord);
    }
    else if ( m_eKeysShown == VisualSettings::Custom )
    {
        float fStartCoord = (float)MIDI::WhiteCount(0, m_iStartNote) + (MIDI::IsSharp(m_iStartNote) ? SharpRatio / 2.0f : 0.0f);
        float fEndCoord = (float)MIDI::WhiteCount(0, m_iEndNote + 1) + (MIDI::IsSharp(m_iEndNote) ? SharpRatio / 2.0f : 0.0f);
        m_fViewStartX = fStartCoord;
        m_fWhiteCX = m_fNotesCX / (fEndCoord - fStartCoord);
    }
    else if ( m_eKeysShown == VisualSettings::Transition )
    {
        const int iBaseStart = MIDI::A0;
        const int iBaseEnd = MIDI::C8;

        int iRangeMin = iBaseStart;
        int iRangeMax = iBaseEnd;
        for ( size_t i = 0; i < 128; i++ )
            for ( vector<int>::const_iterator it = m_vState[i].begin(); it != m_vState[i].end(); it++ )
            {
                const int iNote = m_MIDI.GetEventParam1(m_vEvents[*it]);
                iRangeMin = min( iRangeMin, iNote );
                iRangeMax = max( iRangeMax, iNote );
            }
        for ( long long i = m_iStartPos; i <= m_iEndPos; i++ )
        {
            const MIDIChannelEvent pEvent = m_vEvents[i];
            if ( m_MIDI.GetEventChannelEventType(pEvent) == MIDI::NoteOn && m_MIDI.GetEventParam2(pEvent) > 0 )
            {
                const int iNote = m_MIDI.GetEventParam1(pEvent);
                iRangeMin = min( iRangeMin, iNote );
                iRangeMax = max( iRangeMax, iNote );
            }
        }

        // Once notes outside the 88-key range are detected, transition to 128
        // keys and stay there permanently until the song is stopped/reset.
        // The phase only moves toward 1.0 (never back to 0.0) so the view
        // never stutters from direction reversals mid-song.
        const bool bOutOfRange = iRangeMin < iBaseStart || iRangeMax > iBaseEnd;

        auto nowT = std::chrono::steady_clock::now();
        const float fDt = min( 0.25f, ( float )std::chrono::duration<double>( nowT - s_LastTransitionFrame ).count() );
        s_LastTransitionFrame = nowT;

        // Target is 1.0 (128 keys) once out-of-range notes are seen, otherwise
        // hold at whatever phase we've already reached (never zoom back).
        const float fTarget = bOutOfRange ? 1.0f : s_fTransitionPhase;

        const float fDuration = ( m_eTransitionSpeed == VisualSettings::SmoothFast || m_eTransitionSpeed == VisualSettings::LinearFast ) ? 1.0f : 2.0f;
        const float fPhaseStep = fDt / fDuration;
        s_fTransitionPhase += max( 0.0f, min( fPhaseStep, fTarget - s_fTransitionPhase ) );
        const float fPhase = min( 1.0f, max( 0.0f, s_fTransitionPhase ) );
        m_fKeysTransition = ( m_eTransitionSpeed == VisualSettings::SmoothSlow || m_eTransitionSpeed == VisualSettings::SmoothFast )
            ? fPhase * fPhase * ( 3.0f - 2.0f * fPhase )
            : fPhase;

        const float f88Start = (float)MIDI::WhiteCount(0, MIDI::A0);      // 12.0f
        const float f88End = (float)MIDI::WhiteCount(0, MIDI::C8 + 1);    // 64.0f
        const float f128Start = (float)MIDI::WhiteCount(0, 0);            // 0.0f
        const float f128End = (float)MIDI::WhiteCount(0, 128);            // 75.0f

        m_fViewStartX = ( 1.0f - m_fKeysTransition ) * f88Start + m_fKeysTransition * f128Start;
        const float fViewEndX = ( 1.0f - m_fKeysTransition ) * f88End + m_fKeysTransition * f128End;
        const float fTotalSpan = fViewEndX - m_fViewStartX;
        m_fWhiteCX = m_fNotesCX / fTotalSpan;

        m_iStartNote = ( m_fKeysTransition > 0.001f ) ? 0 : MIDI::A0;
        m_iEndNote = ( m_fKeysTransition > 0.001f ) ? 127 : MIDI::C8;
    }

    m_iAllWhiteKeys = MIDI::WhiteCount( m_iStartNote, m_iEndNote + 1 );

    if ( !m_bShowKB )
        m_fNotesCY = static_cast< float >( m_pRenderer->GetBufferHeight() );
    else
    {
        float fMaxKeyCY = m_pRenderer->GetBufferHeight() * KBPercent;
        float fIdealKeyCY = m_fWhiteCX / KeyRatio;
        fIdealKeyCY = ( fIdealKeyCY / 0.95f + 2.0f ) / 0.93f;
        m_fNotesCY = floor( m_pRenderer->GetBufferHeight() - min( fIdealKeyCY, fMaxKeyCY ) + 0.5f );
    }

    if (m_bTickMode) {
        m_llRndStartTime = m_iStartTick;
    } else {
        long long llMicroSecsPP = static_cast< long long >( m_llTimeSpan / m_fNotesCY + 0.5f );
        m_llRndStartTime = m_llStartTime - ( m_llStartTime < 0 ? llMicroSecsPP : 0 );
        m_llRndStartTime = (m_llRndStartTime / llMicroSecsPP ) * llMicroSecsPP;
    }
    memset(m_aSkipRender, 0, sizeof(m_aSkipRender));

    GenNoteXTable();
}

void MainScreen::RenderLines()
{
    if (m_bBackgroundLoaded)
        return;

    m_pRenderer->DrawRect( m_fNotesX, m_fNotesY, m_fNotesCX, m_fNotesCY, m_csBackground.iPrimaryRGB );

    for ( int i = 1; i <= 127; i++ )
        if ( !MIDI::IsSharp( i - 1 ) && !MIDI::IsSharp( i ) )
        {
            float x = m_fNotesX + ((float)MIDI::WhiteCount( 0, i ) - m_fViewStartX) * m_fWhiteCX;
            if ( x >= m_fNotesX - 2.0f && x <= m_fNotesX + m_fNotesCX + 2.0f )
            {
                m_pRenderer->DrawRect( x - 1.0f, m_fNotesY, 3.0f, m_fNotesCY,
                    m_csBackground.iDarkRGB, m_csBackground.iVeryDarkRGB, m_csBackground.iVeryDarkRGB, m_csBackground.iDarkRGB );
            }
        }

    if (!m_MIDI.IsValid())
        return;
    int iDivision = m_MIDI.GetInfo().iDivision;
    if ( !( iDivision & 0x8000 ) )
    {
        int iCurrTick = m_iStartTick - 1;
        long long llEndTime = (m_bTickMode ? m_iStartTick : m_llStartTime) + m_llTimeSpan;

        uint32_t iLastTempoTick = m_iLastTempoTick;
        uint32_t iMicroSecsPerBeat = m_iMicroSecsPerBeat;
        long long llLastTempoTime = m_llLastTempoTime;
        eventvec_t::const_iterator itNextTempo = m_itNextTempo;

        int iLastSignatureTick = m_iLastSignatureTick;
        int iBeatsPerMeasure = m_iBeatsPerMeasure;
        int iBeatType = m_iBeatType;
        eventvec_t::const_iterator itNextSignature = m_itNextSignature;

        long long llNextBeatTime = 0;
        int iNextBeatTick = 0;
        do
        {
            iNextBeatTick = GetBeatTick( iCurrTick + 1, iBeatType, iLastSignatureTick );

            while ( itNextTempo != m_vTempo.end() && m_vMetaEvents[itNextTempo->second]->GetDataLen() == 3 &&
                    iNextBeatTick > m_vMetaEvents[itNextTempo->second]->GetAbsT() )
            {
                MIDIMetaEvent *pEvent = m_vMetaEvents[itNextTempo->second];
                MIDI::Parse24Bit( pEvent->GetData(), 3, &iMicroSecsPerBeat );
                iLastTempoTick = pEvent->GetAbsT();
                llLastTempoTime = pEvent->GetAbsMicroSec();
                ++itNextTempo;
            }
            while ( itNextSignature != m_vSignature.end() && m_vMetaEvents[itNextSignature->second]->GetDataLen() == 4 &&
                    iNextBeatTick > m_vMetaEvents[itNextSignature->second]->GetAbsT() )
            {
                MIDIMetaEvent *pEvent = m_vMetaEvents[itNextSignature->second];
                iBeatsPerMeasure = pEvent->GetData()[0];
                iBeatType = 1 << pEvent->GetData()[1];
                iLastSignatureTick = pEvent->GetAbsT();
                iNextBeatTick = GetBeatTick( iLastSignatureTick + 1, iBeatType, iLastSignatureTick );
                ++itNextSignature;
            }

            int iNextBeat = GetBeat( iNextBeatTick, iBeatType, iLastSignatureTick );
            bool bIsMeasure = !( ( iNextBeat < 0 ? -iNextBeat : iNextBeat ) % iBeatsPerMeasure );
            llNextBeatTime = GetTickTime( iNextBeatTick, iLastTempoTick, llLastTempoTime, iMicroSecsPerBeat ); 
            float y = m_fNotesY + m_fNotesCY * ( 1.0f - ( (float)(m_bTickMode ? iNextBeatTick : llNextBeatTime) - m_llRndStartTime) / m_llTimeSpan );
            y = floor( y + 0.5f );
            if ( bIsMeasure && y + 1.0f > m_fNotesY )
                m_pRenderer->DrawRect( m_fNotesX, y - 1.0f, m_fNotesCX, 3.0f,
                    m_csBackground.iDarkRGB, m_csBackground.iDarkRGB, m_csBackground.iVeryDarkRGB, m_csBackground.iVeryDarkRGB );

            iCurrTick = iNextBeatTick;
        }
        while ((m_bTickMode ? iNextBeatTick : llNextBeatTime) <= llEndTime );
    }
}

float MainScreen::GetCorruptorAmount() const
{
    if (!m_pRenderer->m_bCorruptorRamp)
        return m_pRenderer->m_fCorruption;

    double dMin = (double)GetMinTime();
    double dMax = (double)GetMaxTime();
    if (dMax <= dMin)
        return 1.0f;

    double dRamp = ((double)m_llStartTime - dMin) / (dMax - dMin);
    dRamp = min(max(dRamp, 0.0), 1.0);
    return (float)dRamp;
}

void MainScreen::RenderNotes()
{
    const int64_t eventCount = (int64_t)m_vEvents.size();
    const bool hasStrip = Config::GetConfig().GetVizSettings().bDualPianoRoll;

    if ( m_iEndPos < 0 || m_iStartPos >= eventCount )
    {
        if (!hasStrip)
            return;
    }

    m_pRenderer->SplitRect();

    for (auto i = m_iEndPos; i >= m_iStartPos; i--) {
        MIDIChannelEvent pEvent = m_vEvents[i];
        if (m_MIDI.GetEventChannelEventType(pEvent) == MIDI::NoteOn &&
            m_MIDI.GetEventParam2(pEvent) > 0 && m_MIDI.EventHasSister(pEvent)) {
            RenderNote(pEvent);
        }
    }

    for (size_t i = 0; i < 128; i++) {
        for (vector< int >::reverse_iterator it = (m_vState[i]).rbegin(); it != (m_vState[i]).rend(); it++) {
            RenderNote(m_vEvents[*it]);
        }
    }

    if (hasStrip) {
        const float stripTimeSpan = m_pRenderer->GetDualRollTimeSpan(
            (float)m_llTimeSpan, m_fNotesCY);
        const long long stripEnd = m_llRndStartTime + (long long)ceilf(stripTimeSpan);


        {
            int64_t hi = max<int64_t>(m_iStartPos, 0);
            while (hi < eventCount) {
                MIDIChannelEvent pEvent = m_vEvents[hi];
                const long long eventTime = m_bTickMode ? m_MIDI.GetEventAbsT(pEvent) : m_MIDI.GetEventTime(pEvent);
                if (eventTime >= stripEnd)
                    break;
                hi++;
            }
            for (int64_t i = hi - 1; i >= max<int64_t>(m_iStartPos, 0); i--) {
                MIDIChannelEvent pEvent = m_vEvents[i];
                if (m_MIDI.GetEventChannelEventType(pEvent) == MIDI::NoteOn &&
                    m_MIDI.GetEventParam2(pEvent) > 0 && m_MIDI.EventHasSister(pEvent)) {
                    RenderPianoRollStripNote(pEvent);
                }
            }
        }

        for (size_t i = 0; i < 128; i++) {
            for (vector< int >::reverse_iterator it = (m_vState[i]).rbegin(); it != (m_vState[i]).rend(); it++) {
                RenderPianoRollStripNote(m_vEvents[*it]);
            }
        }
    }

    m_pRenderer->RenderBatch(true);
}

NoteData MainScreen::BuildRenderNoteData(const MIDIChannelEvent pNote) const
{
    int iNote = m_MIDI.GetEventParam1(pNote);
    int iTrack = m_MIDI.GetEventTrack(pNote);
    int iChannel = m_MIDI.GetEventChannel(pNote);
    long long llNoteStart = m_MIDI.GetEventTime(pNote);
    long long llNoteLength = m_MIDI.GetEventLength(pNote);
    if (m_bTickMode) {
        llNoteStart = m_MIDI.GetEventAbsT(pNote);
        llNoteLength = m_MIDI.GetEventAbsT(m_vEvents[m_MIDI.GetEventSisterIdx(pNote)]) - llNoteStart;
    }

    CorruptNote(GetCorruptorAmount(), CorruptSeed(pNote, m_vEvents[m_MIDI.GetEventSisterIdx(pNote)]),
        iNote, iTrack, iChannel, llNoteStart, llNoteLength, m_llTimeSpan, m_vTrackSettings.size());

    return NoteData{
        .key = (uint8_t)iNote,
        .channel = (uint8_t)iChannel,
        .track = (uint16_t)iTrack,
        .pos = static_cast<float>(llNoteStart - m_llRndStartTime),
        .length = static_cast<float>(llNoteLength),
    };
}

void MainScreen::RenderNote(const MIDIChannelEvent pNote)
{
    m_pRenderer->PushNoteData(BuildRenderNoteData(pNote));
}

void MainScreen::RenderPianoRollStripNote(const MIDIChannelEvent pNote)
{
    m_pRenderer->PushPianoRollStripNoteData(BuildRenderNoteData(pNote));
}

float MainScreen::GetNoteCoord(int iNote)
{
    float fNudgeX = 0.0f;
    if (MIDI::IsSharp(iNote))
    {
        MIDI::Note eNote = MIDI::NoteVal(iNote);
        if (eNote == MIDI::CS || eNote == MIDI::FS) fNudgeX = -SharpRatio / 5.0f;
        else if (eNote == MIDI::AS || eNote == MIDI::DS) fNudgeX = SharpRatio / 5.0f;
        return (float)MIDI::WhiteCount(0, iNote) - SharpRatio / 2.0f + fNudgeX;
    }
    return (float)MIDI::WhiteCount(0, iNote);
}

void MainScreen::GenNoteXTable() {
    for (int i = 0; i <= 127; i++) {
        notex_table[i] = m_fNotesX + (GetNoteCoord(i) - m_fViewStartX) * m_fWhiteCX;
    }
}

float MainScreen::GetNoteX(int iNote) {
    return notex_table[iNote];
}

void MainScreen::RenderKeys()
{
    float fKeysY = m_fNotesY + m_fNotesCY;
    float fKeysCY = m_pRenderer->GetBufferHeight() - m_fNotesCY;

    float fTransitionPct = .02f;
    float fTransitionCY = max( 3.0f, floor( fKeysCY * fTransitionPct + 0.5f ) );
    float fRedPct = .05f;
    float fRedCY = floor( fKeysCY * fRedPct + 0.5f );
    float fSpacerCY = 2.0f;
    float fTopCY = floor( ( fKeysCY - fSpacerCY - fRedCY - fTransitionCY ) * 0.95f + 0.5f );
    float fNearCY = fKeysCY - fSpacerCY - fRedCY - fTransitionCY - fTopCY;

    m_pRenderer->SetRibbonArea(m_fNotesX, fKeysY + fTransitionCY, m_fNotesCX, fRedCY);

    const VizSettings &cViz2 = Config::GetConfig().GetVizSettings();
    const bool bBloom = cViz2.bBloom;
    auto darken32 = [](DWORD c, float f) -> DWORD {
        DWORD r = (DWORD)(((c >> 16) & 0xFF) * f);
        DWORD g = (DWORD)(((c >> 8) & 0xFF) * f);
        DWORD b = (DWORD)((c & 0xFF) * f);
        return (c & 0xFF000000) | (min(r, 255u) << 16) | (min(g, 255u) << 8) | min(b, 255u);
    };
    float kb = bBloom ? 0.25f : 1.0f;
    DWORD ribDark, ribPrim, drawDark, drawPrim;
    if (cViz2.bRibbonCustomColor) {
        DWORD ribBase = cViz2.dwRibbonBaseColor & 0x00FFFFFF;
        ribDark = darken32(ribBase, 0.75f);
        ribPrim = ribBase;
        drawDark = ribDark;
        drawPrim = ribPrim;
    } else {
        ribDark = m_csKBRed.iDarkRGB;
        ribPrim = m_csKBRed.iPrimaryRGB;
        drawDark = darken32(ribDark, kb);
        drawPrim = darken32(ribPrim, kb);
    }
    if (m_bBackgroundLoaded) {
        auto dark = 0x80000000;
        auto very_dark = 0x00000000;
        m_pRenderer->DrawRect(m_fNotesX, fKeysY + fTransitionCY, m_fNotesCX, fKeysCY, very_dark);
        m_pRenderer->DrawRect(m_fNotesX, fKeysY, m_fNotesCX, fTransitionCY,
            0xFF000000, 0xFF000000, very_dark, very_dark);
        m_pRenderer->DrawRect(m_fNotesX, fKeysY + fTransitionCY, m_fNotesCX, fRedCY,
            drawDark, drawDark,
            drawPrim, drawPrim);
        m_pRenderer->DrawRect(m_fNotesX, fKeysY + fTransitionCY + fRedCY, m_fNotesCX, fSpacerCY, dark);
    } else {
        m_pRenderer->DrawRect(m_fNotesX, fKeysY, m_fNotesCX, fKeysCY, darken32(m_csKBBackground.iVeryDarkRGB, kb));
        m_pRenderer->DrawRect(m_fNotesX, fKeysY, m_fNotesCX, fTransitionCY,
            darken32(m_csBackground.iPrimaryRGB, kb), darken32(m_csBackground.iPrimaryRGB, kb),
            darken32(m_csKBBackground.iVeryDarkRGB, kb), darken32(m_csKBBackground.iVeryDarkRGB, kb));
        m_pRenderer->DrawRect(m_fNotesX, fKeysY + fTransitionCY, m_fNotesCX, fRedCY,
            drawDark, drawDark,
            drawPrim, drawPrim);
        m_pRenderer->DrawRect(m_fNotesX, fKeysY + fTransitionCY + fRedCY, m_fNotesCX, fSpacerCY,
            darken32(m_csKBBackground.iDarkRGB, kb), darken32(m_csKBBackground.iDarkRGB, kb),
            darken32(m_csKBBackground.iDarkRGB, kb), darken32(m_csKBBackground.iDarkRGB, kb));
    }

    float fKeyGap = max( 1.0f, floor( m_fWhiteCX * 0.05f + 0.5f ) );
    float fKeyGap1 = fKeyGap - floor( fKeyGap / 2.0f + 0.5f );
    float fSharpCY = fTopCY * 0.67f;
    float fSharpTop = SharpRatio * 0.7f;

    if (Config::GetConfig().GetVizSettings().bColoredRibbon) {
        float fRibY = fKeysY + fTransitionCY;
        for ( int i = 0; i <= 127; i++ )
            if ( !MIDI::IsSharp( i ) )
            {
                if ( m_pNoteState[i] != -1 )
                {
                    float fRibX = m_fNotesX + ((float)MIDI::WhiteCount(0, i) - m_fViewStartX) * m_fWhiteCX;
                    if ( fRibX + m_fWhiteCX >= m_fNotesX && fRibX <= m_fNotesX + m_fNotesCX )
                    {
                        MIDIChannelEvent pEvent = ( m_pNoteState[i] >= 0 ? m_vEvents[m_pNoteState[i]] : UINT32_MAX );
                        const int iTrack = m_MIDI.GetEventTrack(pEvent) % MaxTrackColors;
                        const int iChannel = m_MIDI.GetEventChannel(pEvent);
                        const ChannelSettings &cs = m_vTrackSettings[iTrack].aChannels[iChannel];
                        m_pRenderer->DrawRect( fRibX, fRibY, m_fWhiteCX, fRedCY, cs.iDarkRGB, cs.iDarkRGB, cs.iPrimaryRGB, cs.iPrimaryRGB );
                    }
                }
            }
        for ( int i = 0; i <= 127; i++ )
            if ( MIDI::IsSharp( i ) )
            {
                if ( m_pNoteState[i] != -1 )
                {
                    float fNudgeX = 0.0;
                    MIDI::Note eNote = MIDI::NoteVal( i );
                    if ( eNote == MIDI::CS || eNote == MIDI::FS ) fNudgeX = -SharpRatio / 5.0f;
                    else if ( eNote == MIDI::AS || eNote == MIDI::DS ) fNudgeX = SharpRatio / 5.0f;
                    const float cx = m_fWhiteCX * SharpRatio;
                    const float x = m_fNotesX + ((float)MIDI::WhiteCount(0, i) - SharpRatio / 2.0f + fNudgeX - m_fViewStartX) * m_fWhiteCX;
                    if ( x + cx >= m_fNotesX && x <= m_fNotesX + m_fNotesCX )
                    {
                        MIDIChannelEvent pEvent = ( m_pNoteState[i] >= 0 ? m_vEvents[m_pNoteState[i]] : UINT32_MAX );
                        const int iTrack = m_MIDI.GetEventTrack(pEvent) % MaxTrackColors;
                        const int iChannel = m_MIDI.GetEventChannel(pEvent);
                        const ChannelSettings &cs = m_vTrackSettings[iTrack].aChannels[iChannel];
                        m_pRenderer->DrawRect( x, fRibY, cx, fRedCY, cs.iDarkRGB, cs.iDarkRGB, cs.iPrimaryRGB, cs.iPrimaryRGB );
                    }
                }
            }
    }

    float fCurY = fKeysY + fTransitionCY + fRedCY + fSpacerCY;
    for ( int i = 0; i <= 127; i++ )
        if ( !MIDI::IsSharp( i ) )
        {
            float fCurX = m_fNotesX + ((float)MIDI::WhiteCount(0, i) - m_fViewStartX) * m_fWhiteCX;
            if ( fCurX + m_fWhiteCX < m_fNotesX - 10.0f || fCurX > m_fNotesX + m_fNotesCX + 10.0f )
                continue;

            if ( m_pNoteState[i] == -1 )
            {
                DWORD kbPrimary = darken32(m_csKBWhite.iPrimaryRGB, kb);
                DWORD kbDark = darken32(m_csKBWhite.iDarkRGB, kb);
                DWORD kbVeryDark = darken32(m_csKBWhite.iVeryDarkRGB, kb);
                m_pRenderer->DrawRect( fCurX + fKeyGap1 , fCurY, m_fWhiteCX - fKeyGap, fTopCY + fNearCY,
                    kbDark, kbDark, kbPrimary, kbPrimary );
                m_pRenderer->DrawRect( fCurX + fKeyGap1 , fCurY + fTopCY, m_fWhiteCX - fKeyGap, fNearCY,
                    kbDark, kbDark, kbVeryDark, kbVeryDark );
                m_pRenderer->DrawRect( fCurX + fKeyGap1, fCurY + fTopCY, m_fWhiteCX - fKeyGap, 2.0f,
                    m_csKBBackground.iDarkRGB, m_csKBBackground.iDarkRGB, kbVeryDark, kbVeryDark );

                if ( i == MIDI::C4 )
                {
                    float fMXGap = floor( m_fWhiteCX * 0.25f + 0.5f );
                    float fMCX = m_fWhiteCX - fMXGap * 2.0f - fKeyGap;
                    float fMY = max( fCurY + fTopCY - fMCX - 5.0f, fCurY + fSharpCY + 5.0f );
                    m_pRenderer->DrawRect( fCurX + fKeyGap1 + fMXGap, fMY, fMCX, fCurY + fTopCY - 5.0f - fMY, kbDark );
                }
            }
            else
            {
                MIDIChannelEvent pEvent = ( m_pNoteState[i] >= 0 ? m_vEvents[m_pNoteState[i]] : UINT32_MAX );
                const int iTrack = m_MIDI.GetEventTrack(pEvent) % MaxTrackColors;
                const int iChannel = m_MIDI.GetEventChannel(pEvent);

                ChannelSettings &csKBWhite = m_vTrackSettings[iTrack].aChannels[iChannel];
                m_pRenderer->DrawRect( fCurX + fKeyGap1 , fCurY, m_fWhiteCX - fKeyGap, fTopCY + fNearCY - 2.0f,
                    csKBWhite.iDarkRGB, csKBWhite.iDarkRGB, csKBWhite.iPrimaryRGB, csKBWhite.iPrimaryRGB );
                m_pRenderer->DrawRect( fCurX + fKeyGap1 , fCurY + fTopCY + fNearCY - 2.0f, m_fWhiteCX - fKeyGap, 2.0f, csKBWhite.iDarkRGB );

                if ( i == MIDI::C4 )
                {
                    float fMXGap = floor( m_fWhiteCX * 0.25f + 0.5f );
                    float fMCX = m_fWhiteCX - fMXGap * 2.0f - fKeyGap;
                    float fMY = max( fCurY + fTopCY + fNearCY - fMCX - 7.0f, fCurY + fSharpCY + 5.0f );
                    m_pRenderer->DrawRect( fCurX + fKeyGap1 + fMXGap, fMY, fMCX, fCurY + fTopCY + fNearCY - 7.0f - fMY, csKBWhite.iDarkRGB );
                }
            }
            const float fDivX = fCurX + fKeyGap1 + m_fWhiteCX - fKeyGap;
            m_pRenderer->DrawRect( fDivX, fCurY, fKeyGap, fTopCY + fNearCY,
                darken32(m_csKBBackground.iVeryDarkRGB, kb), darken32(m_csKBBackground.iPrimaryRGB, kb),
                darken32(m_csKBBackground.iPrimaryRGB, kb), darken32(m_csKBBackground.iVeryDarkRGB, kb) );
        }

    for ( int i = 0; i <= 127; i++ )
        if ( MIDI::IsSharp( i ) )
        {
            float fNudgeX = 0.0;
            MIDI::Note eNote = MIDI::NoteVal( i );
            if ( eNote == MIDI::CS || eNote == MIDI::FS ) fNudgeX = -SharpRatio / 5.0f;
            else if ( eNote == MIDI::AS || eNote == MIDI::DS ) fNudgeX = SharpRatio / 5.0f;

            const float cx = m_fWhiteCX * SharpRatio;
            const float x = m_fNotesX + ((float)MIDI::WhiteCount(0, i) - SharpRatio / 2.0f + fNudgeX - m_fViewStartX) * m_fWhiteCX;
            if ( x + cx < m_fNotesX - 10.0f || x > m_fNotesX + m_fNotesCX + 10.0f )
                continue;

            const float fSharpTopX1 = x + m_fWhiteCX * ( SharpRatio - fSharpTop ) / 2.0f;
            const float fSharpTopX2 = fSharpTopX1 + m_fWhiteCX * fSharpTop;

            if ( m_pNoteState[i] == -1 )
            {
                DWORD ksPrimary = darken32(m_csKBSharp.iPrimaryRGB, kb);
                DWORD ksDark = darken32(m_csKBSharp.iDarkRGB, kb);
                DWORD ksVeryDark = darken32(m_csKBSharp.iVeryDarkRGB, kb);
                m_pRenderer->DrawSkew( fSharpTopX1, fCurY + fSharpCY - fNearCY,
                                       fSharpTopX2, fCurY + fSharpCY - fNearCY,
                                       x + cx, fCurY + fSharpCY, x, fCurY + fSharpCY,
                                       ksPrimary, ksPrimary, ksVeryDark, ksVeryDark );
                m_pRenderer->DrawSkew( fSharpTopX1, fCurY - fNearCY,
                                       fSharpTopX1, fCurY + fSharpCY - fNearCY,
                                       x, fCurY + fSharpCY, x, fCurY,
                                       ksPrimary, ksPrimary, ksVeryDark, ksVeryDark );
                m_pRenderer->DrawSkew( fSharpTopX2, fCurY + fSharpCY - fNearCY,
                                       fSharpTopX2, fCurY - fNearCY,
                                       x + cx, fCurY, x + cx, fCurY + fSharpCY,
                                       ksPrimary, ksPrimary, ksVeryDark, ksVeryDark );
                m_pRenderer->DrawRect( fSharpTopX1, fCurY - fNearCY, fSharpTopX2 - fSharpTopX1, fSharpCY, ksVeryDark );
                m_pRenderer->DrawSkew( fSharpTopX1, fCurY - fNearCY,
                                       fSharpTopX2, fCurY - fNearCY,
                                       fSharpTopX2, fCurY - fNearCY + fSharpCY * 0.45f,
                                       fSharpTopX1, fCurY - fNearCY + fSharpCY * 0.35f,
                                       ksDark, ksDark, ksPrimary, ksPrimary );
                m_pRenderer->DrawSkew( fSharpTopX1, fCurY - fNearCY + fSharpCY * 0.35f,
                                       fSharpTopX2, fCurY - fNearCY + fSharpCY * 0.45f,
                                       fSharpTopX2, fCurY - fNearCY + fSharpCY * 0.65f,
                                       fSharpTopX1, fCurY - fNearCY + fSharpCY * 0.55f,
                                       ksPrimary, ksPrimary, ksVeryDark, ksVeryDark );
            }
            else
            {
                MIDIChannelEvent pEvent = ( m_pNoteState[i] >= 0 ? m_vEvents[m_pNoteState[i]] : UINT32_MAX );
                const int iTrack = m_MIDI.GetEventTrack(pEvent) % MaxTrackColors;
                const int iChannel = m_MIDI.GetEventChannel(pEvent);

                const float fNewNear = fNearCY * 0.25f;

                const ChannelSettings &csKBSharp = m_vTrackSettings[iTrack].aChannels[iChannel];
                m_pRenderer->DrawSkew( fSharpTopX1, fCurY + fSharpCY - fNewNear,
                                       fSharpTopX2, fCurY + fSharpCY - fNewNear,
                                       x + cx, fCurY + fSharpCY, x, fCurY + fSharpCY,
                                       csKBSharp.iPrimaryRGB, csKBSharp.iPrimaryRGB, csKBSharp.iDarkRGB, csKBSharp.iDarkRGB );
                m_pRenderer->DrawSkew( fSharpTopX1, fCurY - fNewNear,
                                       fSharpTopX1, fCurY + fSharpCY - fNewNear,
                                       x, fCurY + fSharpCY, x, fCurY,
                                       csKBSharp.iPrimaryRGB, csKBSharp.iPrimaryRGB, csKBSharp.iDarkRGB, csKBSharp.iDarkRGB );
                m_pRenderer->DrawSkew( fSharpTopX2, fCurY + fSharpCY - fNewNear,
                                       fSharpTopX2, fCurY - fNewNear,
                                       x + cx, fCurY, x + cx, fCurY + fSharpCY,
                                       csKBSharp.iPrimaryRGB, csKBSharp.iPrimaryRGB, csKBSharp.iDarkRGB, csKBSharp.iDarkRGB );
                m_pRenderer->DrawRect( fSharpTopX1, fCurY - fNewNear, fSharpTopX2 - fSharpTopX1, fSharpCY, csKBSharp.iDarkRGB );
                m_pRenderer->DrawSkew( fSharpTopX1, fCurY - fNewNear,
                                       fSharpTopX2, fCurY - fNewNear,
                                       fSharpTopX2, fCurY - fNewNear + fSharpCY * 0.35f,
                                       fSharpTopX1, fCurY - fNewNear + fSharpCY * 0.25f,
                                       csKBSharp.iPrimaryRGB, csKBSharp.iPrimaryRGB, csKBSharp.iPrimaryRGB, csKBSharp.iPrimaryRGB );
                m_pRenderer->DrawSkew( fSharpTopX1, fCurY - fNewNear + fSharpCY * 0.25f,
                                       fSharpTopX2, fCurY - fNewNear + fSharpCY * 0.35f,
                                       fSharpTopX2, fCurY - fNewNear + fSharpCY * 0.75f,
                                       fSharpTopX1, fCurY - fNewNear + fSharpCY * 0.65f,
                                       csKBSharp.iPrimaryRGB, csKBSharp.iPrimaryRGB, csKBSharp.iDarkRGB, csKBSharp.iDarkRGB );
            }
        }
}

void MainScreen::RenderBorder()
{
    // Top, bottom, left, right
    const unsigned iBlack = 0x00000000;
    float fBufferCY = static_cast< float >( m_pRenderer->GetBufferHeight() );
    float fBufferCX = static_cast< float >( m_pRenderer->GetBufferWidth() );

    if ( m_fNotesX > 0.0f )
        m_pRenderer->DrawRect( 0.0f, 0.0f, m_fNotesX, fBufferCY, iBlack );
    if ( m_fNotesX + m_fNotesCX < fBufferCX )
        m_pRenderer->DrawRect( m_fNotesX + m_fNotesCX, 0.0f, fBufferCX - (m_fNotesX + m_fNotesCX), fBufferCY, iBlack );

    m_pRenderer->DrawRect( m_fNotesX - 50.0f, m_fNotesY - 50.0f, m_fNotesCX + 100.0f, 50.0f, iBlack );
    m_pRenderer->DrawRect( m_fNotesX - 50.0f, m_fNotesY + fBufferCY, m_fNotesCX + 100.0f, 50.0f, iBlack );
    m_pRenderer->DrawRect( m_fNotesX - max(50.0f, m_fWhiteCX * 2.0f), m_fNotesY - 50.0f, max(50.0f, m_fWhiteCX * 2.0f), fBufferCY + 100.0f, iBlack );
    m_pRenderer->DrawRect( m_fNotesX + m_fNotesCX, m_fNotesY - 50.0f, max(50.0f, m_fWhiteCX * 2.0f), fBufferCY + 100.0f, iBlack );

    const float fPad = 10.0f;
    const unsigned iBkg = m_csBackground.iPrimaryRGB;
    m_pRenderer->DrawSkew( m_fNotesX, m_fNotesY + fBufferCY, m_fNotesX + m_fNotesCX, m_fNotesY + fBufferCY,
                           m_fNotesX + m_fNotesCX + fPad, m_fNotesY + fBufferCY + fPad, m_fNotesX - fPad, m_fNotesY + fBufferCY + fPad,
                           iBkg, iBkg, iBlack, iBlack );
    m_pRenderer->DrawSkew( m_fNotesX - fPad, m_fNotesY - fPad, m_fNotesX + m_fNotesCX + fPad, m_fNotesY - fPad,
                           m_fNotesX + m_fNotesCX, m_fNotesY, m_fNotesX, m_fNotesY,
                           iBlack, iBlack, iBkg, iBkg );
    m_pRenderer->DrawSkew( m_fNotesX - fPad, m_fNotesY - fPad, m_fNotesX, m_fNotesY,
                           m_fNotesX, m_fNotesY + fBufferCY, m_fNotesX - fPad, m_fNotesY + fBufferCY + fPad,
                           iBlack, iBkg, iBkg, iBlack );
    m_pRenderer->DrawSkew( m_fNotesX + m_fNotesCX, m_fNotesY, m_fNotesX + m_fNotesCX + fPad, m_fNotesY - fPad,
                           m_fNotesX + m_fNotesCX + fPad, m_fNotesY + fBufferCY + fPad, m_fNotesX + m_fNotesCX, m_fNotesY + fBufferCY,
                           iBkg, iBlack, iBlack, iBkg );
}

void MainScreen::RenderText()
{
    Config& config = Config::GetConfig();
    VizSettings viz = config.GetVizSettings();

    int iLines = IsFreePlay() ? 1 : 2;
    if (m_bShowFPS && !m_bDumpFrames)
        iLines++;
    if (viz.bNerdStats)
        iLines += 7;
    if (viz.bNerdStats && Config::GetConfig().GetAudioSettings().bPreRenderAudio && PRE_MIDIAudio)
        iLines += 3;
    if (m_Timer.m_bManualTimer && !m_bDumpFrames)
        iLines++;

    // Screen info
    int iMsgCY = 200;
    RECT rcMsg = { 0, static_cast<int>(m_pRenderer->GetBufferHeight() * (1.0f - KBPercent) - iMsgCY) / 2, 0, 0 };
    rcMsg.right = m_pRenderer->GetBufferWidth();
    rcMsg.bottom = rcMsg.top + iMsgCY;

    // Draw the text
    m_pRenderer->BeginText();

    RenderStatus(iLines);
    if (viz.bSysStats)
        RenderSysStats();
    if (!m_sMarker.empty() && viz.bShowMarkers)
        RenderMarker(m_sMarker.c_str());
    if (m_bZoomMove)
        RenderMessage(&rcMsg, (TCHAR*)TEXT("- Left-click and drag to move the screen\n- Right-click and drag to zoom horizontally\n- Press Escape to abort changes\n- Press Ctrl+V to save changes"));

    m_pRenderer->EndText();
}

void MainScreen::RenderStatusLine(int line, float width, float yOffset, float rightEdge, const char* left, const char* format, ...) {
    va_list varargs;
    va_start(varargs, format);

    float scale = Config::GetConfig().GetVizSettings().fUIScale;
    char buf[1024] = {};
    vsnprintf_s(buf, sizeof(buf), format, varargs);

    auto draw_list = m_pRenderer->GetDrawList();
    float y = yOffset + (3 + line * 16) * scale;
    ImVec2 left_pos = ImVec2(rightEdge - width + 6 * scale, y);
    ImVec2 right_pos = ImVec2(rightEdge - ImGui::CalcTextSize(buf).x - 6 * scale, y);
    draw_list->AddText(ImVec2(left_pos.x + 2, left_pos.y + 1), 0xFF404040, left);
    draw_list->AddText(ImVec2(left_pos.x, left_pos.y), 0xFFFFFFFF, left);
    draw_list->AddText(ImVec2(right_pos.x + 2, right_pos.y + 1), 0xFF404040, buf);
    draw_list->AddText(ImVec2(right_pos.x, right_pos.y), 0xFFFFFFFF, buf);

    va_end(varargs);
}

//// Applies a true 3D perspective tilt (Y-axis yaw) and optional zoom scale to vertices in an ImDrawList range.
static void Apply3DTilt(ImDrawList* dl, int vtx_start, int vtx_end, const ImVec2& center, float rotY, float scale = 1.0f, float dist = 700.0f)
{
    if (!dl || vtx_start < 0 || vtx_start >= vtx_end || vtx_end > dl->VtxBuffer.Size)
        return;

    const float cosY = cosf(rotY);
    const float sinY = sinf(rotY);

    ImDrawVert* vStart = dl->VtxBuffer.Data + vtx_start;
    ImDrawVert* vEnd = dl->VtxBuffer.Data + vtx_end;

    for (ImDrawVert* v = vStart; v < vEnd; ++v)
    {
        const float dx = (v->pos.x - center.x) * scale;
        const float dy = (v->pos.y - center.y) * scale;

        // 3D Y-axis rotation (depth yaw)
        const float x3d = dx * cosY;
        const float z3d = dx * sinY;

        // 3D perspective projection
        const float proj = dist / (dist + z3d);

        v->pos.x = center.x + x3d * proj;
        v->pos.y = center.y + dy * proj;
    }
}

float MainScreen::GetStatsBounceScale() const
{
    const VizSettings& viz = Config::GetConfig().GetVizSettings();
    if (!viz.bBounceStats || m_bPaused)
        return 1.0f;

    // Start bouncing only once the first notes have been played
    if (!IsFreePlay() && m_MIDI.IsValid() && m_llStartTime < m_MIDI.GetInfo().llFirstNote)
        return 1.0f;

    long long curNps = 0;
    for (size_t i = 0; i < m_dNPSNotes.size(); i++)
        curNps += std::get<1>(m_dNPSNotes[i]);

    long long maxNps = m_llMaxNPS;
    if (curNps > maxNps)
        maxNps = curNps;
    if (maxNps <= 0)
        maxNps = 1;

    // Rule: if NPS is around viz.iBounceNPSThreshold % lower than max NPS
    // (e.g. threshold = 90% means nps <= 10% of max NPS), bounce 2 beats per bar.
    // Otherwise (e.g. 89% lower or better, nps > 10% of max NPS), bounce 4 beats per bar.
    double lowActivityThresholdNps = (double)maxNps * (1.0 - (double)viz.iBounceNPSThreshold / 100.0);
    bool bLowActivity = ((double)curNps <= lowActivityThresholdNps);

    // Calculate beat position from current time / tick / BPM
    double beats = 0.0;
    int iDivision = m_MIDI.GetInfo().iDivision;
    if (!(iDivision & 0x8000) && iDivision > 0 && m_iBeatType > 0)
    {
        int curTick = const_cast<MainScreen*>(this)->GetCurrentTick(m_llStartTime);
        int ticksPerBeat = (iDivision * 4) / m_iBeatType;
        if (ticksPerBeat > 0)
            beats = (double)(curTick - m_iLastSignatureTick) / (double)ticksPerBeat;
    }
    else if (m_iMicroSecsPerBeat > 0)
    {
        beats = (double)(m_llStartTime - m_llLastTempoTime) / (double)m_iMicroSecsPerBeat;
    }

    int beatsPerMeasure = (m_iBeatsPerMeasure > 0) ? m_iBeatsPerMeasure : 4;
    double beatInMeasure = fmod(beats, (double)beatsPerMeasure);
    if (beatInMeasure < 0.0)
        beatInMeasure += (double)beatsPerMeasure;

    float phase = 0.0f;
    if (bLowActivity)
    {
        // 2 beats per bar: bounce every 2 beats (half note in 4/4)
        double u = fmod(beatInMeasure, 2.0) / 2.0;
        if (u < 0.0) u += 1.0;
        phase = (float)u;
    }
    else
    {
        // 4 beats per bar: bounce every 1 beat (quarter note in 4/4)
        double u = fmod(beatInMeasure, 1.0);
        if (u < 0.0) u += 1.0;
        phase = (float)u;
    }

    // Zoom in and out very slightly with an exponent of fast to slow (cubic ease-out decay)
    const float amplitude = 0.06f; // subtle ~6% peak zoom
    float bounce = amplitude * powf(1.0f - phase, 3.0f);
    return 1.0f + bounce;
}

void MainScreen::RenderStatus(int lines)
{
    auto dl = m_pRenderer->GetDrawList();
    const int vtx_start = dl ? dl->VtxBuffer.Size : 0;

    // Time
    Config& config = Config::GetConfig();
    VizSettings viz = config.GetVizSettings();
    const bool bFreePlay = IsFreePlay();
    long long llTotalMicroSecs = 0;
    if (!bFreePlay)
        llTotalMicroSecs = m_MIDI.GetInfo().llTotalMicroSecs;
    auto starttime = ((m_llDisplayTime >= 0) ? m_llDisplayTime : -m_llDisplayTime);

    auto min = starttime / 60000000;
    auto sec = (starttime % 60000000) / 1000000;
    auto cs = (starttime % 1000000) / 100000;
    auto tmin = llTotalMicroSecs / 60000000;
    auto tsec = (llTotalMicroSecs % 60000000) / 1000000;
    auto tcs = (llTotalMicroSecs % 1000000) / 100000;

    char time_buf[1024] = {};
    if (bFreePlay)
        snprintf(time_buf, sizeof(time_buf) - 1,
            "%s%lld:%02lld.%lld",
            m_llDisplayTime >= 0 ? "" : "-",
            min, sec, cs);
    else
        snprintf(time_buf, sizeof(time_buf) - 1,
            "%s%lld:%02lld.%lld / %lld:%02lld.%lld",
            m_llDisplayTime >= 0 ? "" : "-",
            min, sec, cs,
            tmin, tsec, tcs);
    float width = max(156 * viz.fUIScale, ImGui::CalcTextSize("Time:").x + ImGui::CalcTextSize(time_buf).x + 24.0f * viz.fUIScale);
    int cur_line = 0;
    const float contentTop = ImGui::GetFrameHeight() + 35.0f;
    float toolbarBottom = contentTop + 10.0f;
    if (viz.bDualPianoRoll) {
        const float stripH = max(190.0f, min((float)m_pRenderer->GetBufferHeight() * 0.45f,
            (float)m_pRenderer->GetBufferHeight() * 0.28f));
        toolbarBottom = 20.0f + 35.0f + stripH + 10.0f;
    }
    float overlayH = (6 + 16 * lines) * viz.fUIScale;
    float bh = (float)m_pRenderer->GetBufferHeight();
    const float statusRight = (float)m_pRenderer->GetBufferWidth() - 10.0f;
    const float statusLeft = statusRight - width;
    const float blurPad = 10.0f;
    const float blurLeft = max(0.0f, statusLeft - blurPad);
    const float blurTop = max(0.0f, toolbarBottom - blurPad);
    const float blurRight = min((float)m_pRenderer->GetBufferWidth(), statusRight + blurPad);
    const float blurBottom = min((float)m_pRenderer->GetBufferHeight(), toolbarBottom + overlayH + blurPad);
    const ImTextureID blurTexture = (ImTextureID)m_pRenderer->GetBlurTextureID();
    auto uv = [=](const ImVec2& p) {
        return ImVec2(p.x / (float)m_pRenderer->GetBufferWidth(), p.y / bh);
    };
    auto addGradientImageQuad = [&](const ImVec2& p1, const ImVec2& p2,
                                    const ImVec2& p3, const ImVec2& p4,
                                    ImU32 c1, ImU32 c2, ImU32 c3, ImU32 c4) {
        if (!blurTexture)
            return;
        auto drawList = m_pRenderer->GetDrawList();
        drawList->PushTextureID(blurTexture);
        drawList->PrimReserve(6, 6);
        drawList->PrimVtx(p1, uv(p1), c1);
        drawList->PrimVtx(p2, uv(p2), c2);
        drawList->PrimVtx(p3, uv(p3), c3);
        drawList->PrimVtx(p1, uv(p1), c1);
        drawList->PrimVtx(p3, uv(p3), c3);
        drawList->PrimVtx(p4, uv(p4), c4);
        drawList->PopTextureID();
    };
    const ImU32 blurOpaque = IM_COL32(255, 255, 255, 255);
    const ImU32 blurTransparent = IM_COL32(255, 255, 255, 0);

    m_pRenderer->GetDrawList()->AddImage(
        blurTexture,
        ImVec2(statusLeft, toolbarBottom),
        ImVec2(statusRight, toolbarBottom + overlayH),
        uv(ImVec2(statusLeft, toolbarBottom)),
        uv(ImVec2(statusRight, toolbarBottom + overlayH))
    );

    addGradientImageQuad(
        ImVec2(statusLeft, blurTop), ImVec2(statusRight, blurTop),
        ImVec2(statusRight, toolbarBottom), ImVec2(statusLeft, toolbarBottom),
        blurTransparent, blurTransparent, blurOpaque, blurOpaque);
    addGradientImageQuad(
        ImVec2(statusLeft, toolbarBottom + overlayH), ImVec2(statusRight, toolbarBottom + overlayH),
        ImVec2(statusRight, blurBottom), ImVec2(statusLeft, blurBottom),
        blurOpaque, blurOpaque, blurTransparent, blurTransparent);

    addGradientImageQuad(
        ImVec2(blurLeft, toolbarBottom), ImVec2(statusLeft, toolbarBottom),
        ImVec2(statusLeft, toolbarBottom + overlayH), ImVec2(blurLeft, toolbarBottom + overlayH),
        blurTransparent, blurOpaque, blurOpaque, blurTransparent);
    addGradientImageQuad(
        ImVec2(statusRight, toolbarBottom), ImVec2(blurRight, toolbarBottom),
        ImVec2(blurRight, toolbarBottom + overlayH), ImVec2(statusRight, toolbarBottom + overlayH),
        blurOpaque, blurTransparent, blurTransparent, blurOpaque);

    addGradientImageQuad(
        ImVec2(blurLeft, blurTop), ImVec2(statusLeft, blurTop),
        ImVec2(statusLeft, toolbarBottom), ImVec2(blurLeft, toolbarBottom),
        blurTransparent, blurTransparent, blurOpaque, blurTransparent);
    addGradientImageQuad(
        ImVec2(statusRight, blurTop), ImVec2(blurRight, blurTop),
        ImVec2(blurRight, toolbarBottom), ImVec2(statusRight, toolbarBottom),
        blurTransparent, blurTransparent, blurTransparent, blurOpaque);
    addGradientImageQuad(
        ImVec2(blurLeft, toolbarBottom + overlayH), ImVec2(statusLeft, toolbarBottom + overlayH),
        ImVec2(statusLeft, blurBottom), ImVec2(blurLeft, blurBottom),
        blurTransparent, blurOpaque, blurTransparent, blurTransparent);
    addGradientImageQuad(
        ImVec2(statusRight, toolbarBottom + overlayH), ImVec2(blurRight, toolbarBottom + overlayH),
        ImVec2(blurRight, blurBottom), ImVec2(statusRight, blurBottom),
        blurOpaque, blurTransparent, blurTransparent, blurTransparent);

    m_pRenderer->GetDrawList()->AddRectFilled(
        ImVec2(statusLeft, toolbarBottom),
        ImVec2(statusRight, toolbarBottom + overlayH),
        0x40000000
    );

    RenderStatusLine(cur_line++, width, toolbarBottom, statusRight, "Time:", time_buf);
    if (!bFreePlay)
        RenderStatusLine(cur_line++, width, toolbarBottom, statusRight, "Tempo:", "%.3lf bpm", 60000000.0 / m_iMicroSecsPerBeat);

    if (m_bShowFPS && !m_bDumpFrames)
        RenderStatusLine(cur_line++, width, toolbarBottom, statusRight, "FPS:", "%.1lf", m_dFPS);

    if (viz.bNerdStats) {
        long long nps = 0;
        for (size_t i = 0; i < m_dNPSNotes.size(); i++)
            nps += std::get<1>(m_dNPSNotes[i]);

        RenderStatusLine(cur_line++, width, toolbarBottom, statusRight, "NPS:", "%lld", nps);
        RenderStatusLine(cur_line++, width, toolbarBottom, statusRight, "Rendered:", "%llu", m_pRenderer->GetRenderedNotesCount());
        RenderStatusLine(cur_line++, width, toolbarBottom, statusRight, "File notes:", "%llu", m_MIDI.GetInfo().iNoteCount);
        RenderStatusLine(cur_line++, width, toolbarBottom, statusRight, "MIDI sent:", "%llu", m_OutDevice.GetEventsSent());
        RenderStatusLine(cur_line++, width, toolbarBottom, statusRight, "MIDI failed:", "%llu", m_OutDevice.GetSendFailures());
        RenderStatusLine(cur_line++, width, toolbarBottom, statusRight, "MaxLate:", "%lld ms", m_llMaxLateMicros / 1000);
        RenderStatusLine(cur_line++, width, toolbarBottom, statusRight, "Late evts:", "%llu", m_ullLateEvents);
    }

    if (viz.bNerdStats && Config::GetConfig().GetAudioSettings().bPreRenderAudio && PRE_MIDIAudio)
    {
        char buf[64] = {};
        snprintf(buf, sizeof(buf) - 1, "%6.1f ms", PRE_MIDIAudio->GetBufferSeconds() * 1000.0);
        RenderStatusLine(cur_line++, width, toolbarBottom, statusRight, "Buffer:", buf);
        RenderStatusLine(cur_line++, width, toolbarBottom, statusRight, "Underruns:", "%llu", PRE_MIDIAudio->GetBufferUnderruns());
        RenderStatusLine(cur_line++, width, toolbarBottom, statusRight, "Voices:", "%d", PRE_MIDIAudio->m_iDefaultVoices);
    }

    if (auto drawList = m_pRenderer->GetDrawList())
    {
        int vtx_end = drawList->VtxBuffer.Size;
        ImVec2 center((statusLeft + statusRight) * 0.5f, toolbarBottom + overlayH * 0.5f);
        Apply3DTilt(drawList, vtx_start, vtx_end, center, -0.20f, GetStatsBounceScale(), 750.0f);
    }
}

// Draws the blurred panel background the status overlay uses: a sampled image
// of the scene blurred + gradient fades on each edge/corner + a dark scrim.
static void DrawBlurPanel(Renderer* r, float left, float top, float right, float bottom, float pad)
{
    const float bw = (float)r->GetBufferWidth();
    const float bh = (float)r->GetBufferHeight();
    auto dl = r->GetDrawList();
    const ImTextureID blurTexture = (ImTextureID)r->GetBlurTextureID();
    if (!blurTexture)
    {
        dl->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom), 0x40000000);
        return;
    }
    auto uv = [=](const ImVec2& p) { return ImVec2(p.x / bw, p.y / bh); };
    auto quad = [&](const ImVec2& p1, const ImVec2& p2, const ImVec2& p3, const ImVec2& p4,
                    ImU32 c1, ImU32 c2, ImU32 c3, ImU32 c4) {
        dl->PushTextureID(blurTexture);
        dl->PrimReserve(6, 6);
        dl->PrimVtx(p1, uv(p1), c1);
        dl->PrimVtx(p2, uv(p2), c2);
        dl->PrimVtx(p3, uv(p3), c3);
        dl->PrimVtx(p1, uv(p1), c1);
        dl->PrimVtx(p3, uv(p3), c3);
        dl->PrimVtx(p4, uv(p4), c4);
        dl->PopTextureID();
    };
    const ImU32 op = IM_COL32(255, 255, 255, 255);
    const ImU32 tr = IM_COL32(255, 255, 255, 0);
    const float blurL = max(0.0f, left - pad), blurT = max(0.0f, top - pad);
    const float blurR = min(bw, right + pad), blurB = min(bh, bottom + pad);

    dl->AddImage(blurTexture, ImVec2(left, top), ImVec2(right, bottom),
        uv(ImVec2(left, top)), uv(ImVec2(right, bottom)));

    quad(ImVec2(left, blurT), ImVec2(right, blurT), ImVec2(right, top), ImVec2(left, top), tr, tr, op, op);
    quad(ImVec2(left, bottom), ImVec2(right, bottom), ImVec2(right, blurB), ImVec2(left, blurB), op, op, tr, tr);
    quad(ImVec2(blurL, top), ImVec2(left, top), ImVec2(left, bottom), ImVec2(blurL, bottom), tr, op, op, tr);
    quad(ImVec2(right, top), ImVec2(blurR, top), ImVec2(blurR, bottom), ImVec2(right, bottom), op, tr, tr, op);
    quad(ImVec2(blurL, blurT), ImVec2(left, blurT), ImVec2(left, top), ImVec2(blurL, top), tr, tr, op, tr);
    quad(ImVec2(right, blurT), ImVec2(blurR, blurT), ImVec2(blurR, top), ImVec2(right, top), tr, tr, tr, op);
    quad(ImVec2(blurL, bottom), ImVec2(left, bottom), ImVec2(left, blurB), ImVec2(blurL, blurB), tr, op, tr, tr);
    quad(ImVec2(right, bottom), ImVec2(blurR, bottom), ImVec2(blurR, blurB), ImVec2(right, bottom), op, tr, tr, tr);

    dl->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom), 0x40000000);
}

void MainScreen::RenderSysStats()
{
    static bool s_bDrawnLogged = false;
    if (!s_bDrawnLogged)
    {
        s_bDrawnLogged = true;
        HeartbeatLog("sysstats:panel drawn");
    }
    try
    {
        Config& config = Config::GetConfig();
        VizSettings viz = config.GetVizSettings();
    Renderer* r = m_pRenderer;
    const float scale = viz.fUIScale;
    const float bh = (float)r->GetBufferHeight();
    const float bw = (float)r->GetBufferWidth();

    auto dl = r->GetDrawList();
    const int vtx_start = dl ? dl->VtxBuffer.Size : 0;

    UpdateSysStats(r);

    char cpuB[64], ramB[64], vramB[64], gpuB[64];
    double usedGB = (double)(s_memEx.ullTotalPhys - s_memEx.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
    double totalGB = (double)s_memEx.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    snprintf(cpuB, sizeof(cpuB) - 1, "%.0f%%", s_dCpuPct);
    snprintf(ramB, sizeof(ramB) - 1, "%.1f / %.1f GB", usedGB, totalGB);
    if (s_ullVramTotal > 0)
        snprintf(vramB, sizeof(vramB) - 1, "%.1f / %.1f GB", (double)s_ullVramUsed / (1024.0 * 1024.0 * 1024.0), (double)s_ullVramTotal / (1024.0 * 1024.0 * 1024.0));
    else
        snprintf(vramB, sizeof(vramB) - 1, "--");
    if (s_bGpuAvail.load())
        snprintf(gpuB, sizeof(gpuB) - 1, "%.0f%%", s_fGpuPct.load());
    else
        snprintf(gpuB, sizeof(gpuB) - 1, "--");

    const char* labels[4] = { "CPU:", "RAM:", "VRAM:", "GPU:" };
    const char* vals[4] = { cpuB, ramB, vramB, gpuB };
    float textW = 0.0f;
    for (int i = 0; i < 4; i++)
        textW = max(textW, ImGui::CalcTextSize(labels[i]).x + ImGui::CalcTextSize(vals[i]).x + 24.0f * scale);
    const float graphH = 64.0f * scale;
    const float panelW = max(200.0f * scale, textW);

    const float contentTop = ImGui::GetFrameHeight() + 35.0f;
    float toolbarBottom = contentTop + 10.0f;
    if (viz.bDualPianoRoll)
    {
        const float stripH = max(190.0f, min(bh * 0.45f, bh * 0.28f));
        toolbarBottom = 20.0f + 35.0f + stripH + 10.0f;
    }

    float panelLeft = 10.0f;
    const float panelRight = panelLeft + panelW;

    const float textH = (6 + 16 * 4) * scale;
    const float overlayH = textH + graphH + 10.0f * scale;
    const float panelTop = toolbarBottom;

    DrawBlurPanel(r, panelLeft, panelTop, panelRight, panelTop + overlayH, 10.0f);

    int cur_line = 0;
    for (int i = 0; i < 4; i++)
        RenderStatusLine(cur_line++, panelW, panelTop, panelRight, labels[i], "%s", vals[i]);

    // NPS history graph: rolling 1s note-count samples, newest on the right.
    const float graphTop = panelTop + (6 + 16 * 4) * scale + 4.0f * scale;
    const ImVec2 g0(panelLeft + 6.0f * scale, graphTop);
    const ImVec2 g1(panelRight - 6.0f * scale, graphTop + graphH);
    if (!dl)
        dl = r->GetDrawList();
    dl->AddRectFilled(g0, g1, 0x30000000);
    for (int gi = 0; gi <= 4; gi++)
    {
        float y = g0.y + (g1.y - g0.y) * gi / 4.0f;
        dl->AddLine(ImVec2(g0.x, y), ImVec2(g1.x, y), 0x20FFFFFF);
    }
    const size_t n = m_dNPSHistory.size();
    if (n > 0)
    {
        long long maxNps = 1;
        for (size_t i = 0; i < n; i++)
            maxNps = max(maxNps, m_dNPSHistory[i]);
        const float target = max(100.0f, (float)(ceil((double)maxNps * 1.15 / 50.0) * 50.0));
        static float s_fRange = 100.0f;
        if (target > s_fRange)
            s_fRange += (target - s_fRange) * 0.25f;
        else
            s_fRange += (target - s_fRange) * 0.5f;
        if (s_fRange < 100.0f)
            s_fRange = 100.0f;
        const float yTop = s_fRange;
        const float step = (g1.x - g0.x) / 600.0f;
        ImVec2 pts[600];
        for (size_t k = 0; k < n; k++)
        {
            float x0 = g1.x - (float)(n - k) * step;
            float hgt = (float)m_dNPSHistory[k] / yTop * (g1.y - g0.y);
            if (hgt > g1.y - g0.y)
                hgt = g1.y - g0.y;
            pts[k] = ImVec2(x0, g1.y - hgt);
        }
        for (size_t k = 0; k + 1 < n; k++)
            dl->AddRectFilled(ImVec2(pts[k].x, pts[k].y), ImVec2(pts[k + 1].x, g1.y), IM_COL32(255, 255, 255, 40));
        dl->AddRectFilled(ImVec2(pts[n - 1].x, pts[n - 1].y), ImVec2(g1.x, g1.y), IM_COL32(255, 255, 255, 40));
        dl->AddPolyline(pts, (int)n, IM_COL32(255, 255, 255, 220), 0, 2.0f * scale);
        char peakB[64];
        snprintf(peakB, sizeof(peakB) - 1, "max %lld/s", (long long)yTop);
        dl->AddText(ImVec2(g1.x - ImGui::CalcTextSize(peakB).x, graphTop - 14.0f * scale), 0xFF9A9A9A, peakB);
    }

    if (dl)
    {
        const int vtx_end = dl->VtxBuffer.Size;
        const ImVec2 center((panelLeft + panelRight) * 0.5f, panelTop + overlayH * 0.5f);
        Apply3DTilt(dl, vtx_start, vtx_end, center, 0.20f, GetStatsBounceScale(), 750.0f);
    }
    }
    catch (...)
    {
        static bool s_bLogged = false;
        if (!s_bLogged)
        {
            s_bLogged = true;
            HeartbeatLog("sysstats:exception skipped");
        }
    }
}

void MainScreen::RenderMarker(const char* str) {
    float scale = Config::GetConfig().GetVizSettings().fUIScale;
    ImVec2 size = ImGui::CalcTextSize(str);
    size.x += 12 * scale;
    size.y += 6 * scale;

    auto draw_list = m_pRenderer->GetDrawList();
    draw_list->AddRectFilled(ImVec2(0, 0), size, 0x80000000);
    draw_list->AddText(ImVec2((6 + 2) * scale, (3 + 1) * scale), 0xFF404040, str);
    draw_list->AddText(ImVec2(6 * scale, 3 * scale), 0xFFFFFFFF, str);
}

void MainScreen::RenderMessage(LPRECT prcMsg, TCHAR* sMsg)
{
    RECT rcMsg = {};
    Renderer::FontSize eFontSize = Renderer::Medium;
    m_pRenderer->DrawText(sMsg, eFontSize, &rcMsg, DT_CALCRECT, 0xFF000000);
    if (rcMsg.right > m_pRenderer->GetBufferWidth())
    {
        eFontSize = Renderer::Small;
        m_pRenderer->DrawText(sMsg, eFontSize, &rcMsg, DT_CALCRECT, 0xFF000000);
    }

    OffsetRect(&rcMsg, 2 + prcMsg->left + (prcMsg->right - prcMsg->left - rcMsg.right) / 2,
        2 + prcMsg->top + (prcMsg->bottom - prcMsg->top - rcMsg.bottom) / 2);
    m_pRenderer->DrawText(sMsg, eFontSize, &rcMsg, 0, 0xFF404040);
    OffsetRect(&rcMsg, -2, -2);
    m_pRenderer->DrawText(sMsg, eFontSize, &rcMsg, 0, 0xFFFFFFFF);
}


FreePlayScreen::FreePlayScreen(HWND hWnd, Renderer* pRenderer)
    : MainScreen(L"", GameState::Practice, hWnd, pRenderer)
{
    InitColors();

    m_vTrackSettings.resize(MaxTrackColors);
    for (int i = 0; i < MaxTrackColors; i++)
        m_vFreeSlots.push_back(i);

    static Config& config = Config::GetConfig();
    static const VisualSettings& cVisual = config.GetVisualSettings();
    static const PlaybackSettings& cPlayback = config.GetPlaybackSettings();
    static const ViewSettings& cView = config.GetViewSettings();
    m_iStartNote = min(cVisual.iFirstKey, cVisual.iLastKey);
    m_iEndNote = max(cVisual.iFirstKey, cVisual.iLastKey);
    m_eKeysShown = cVisual.eKeysShown;

    m_dSpeed = 1.0;
    m_bPaused = false;
    m_llTimeSpan = 3000000LL; // 3 seconds visible
    m_llStartTime = 0;
    m_llDisplayTime = 0;
    m_llRndStartTime = 0;
    m_iStartPos = 0;
    m_iEndPos = -1;
    m_iStartTick = 0;

    memset(m_pNoteState, -1, sizeof(m_pNoteState));
    m_dFPS = 0;
    m_iFPSCount = 0;
    m_llFPSTime = 0;
    m_bShowFPS = false;
    m_bShowKB = true;
    m_fZoomX = cView.GetZoomX();
    m_fOffsetX = cView.GetOffsetX();
    m_fOffsetY = cView.GetOffsetY();
    m_fTempZoomX = 1.0f;
    m_fTempOffsetX = m_fTempOffsetY = 0.0f;
    m_bTrackPos = m_bTrackZoom = false;
    m_bPaused = false;
    m_bMute = cPlayback.GetMute();
    m_dVolume = cPlayback.GetVolume();
    m_eKeysShown = cVisual.eKeysShown;

    m_csKBRed.SetColor(config.GetVizSettings().iBarColor, 0.5f);
}

GameState::GameError FreePlayScreen::Init()
{
    static Config& config = Config::GetConfig();
    static const AudioSettings& cAudio = config.GetAudioSettings();
    static const VizSettings& cViz = config.GetVizSettings();

    if (cViz.bKDMAPI)
        m_OutDevice.OpenKDMAPI();
    else
        m_OutDevice.Open(cAudio.iOutDevice);

    m_Timer.Init(false);
    m_RealTimer.Init(false);
    m_llFreePlayLastFrame = m_RealTimer.GetMicroSecs();

    m_vLoops.clear();
    m_vRecordingEvents.clear();
    m_bRecording = false;
    m_bCountdown = false;
    m_iLoopCounter = 0;
    for (int i = 0; i < 128; i++) m_iChordEvent[i] = -1;

    return Success;
}

int FreePlayScreen::NoteFromMousePos(int mx, int my) const
{
    float fKeysY = m_fNotesY + m_fNotesCY;
    if ((float)my < fKeysY)
        return -1;

    float fKeysCY = (float)m_pRenderer->GetBufferHeight() - m_fNotesCY;
    float fTransitionCY = max(3.0f, floor(fKeysCY * 0.02f + 0.5f));
    float fRedCY = floor(fKeysCY * 0.05f + 0.5f);
    float fSpacerCY = 2.0f;
    float fCurY = fKeysY + fTransitionCY + fRedCY + fSpacerCY;
    float fTopCY = floor((fKeysCY - fSpacerCY - fRedCY - fTransitionCY) * 0.95f + 0.5f);
    float fNearCY = fKeysCY - fSpacerCY - fRedCY - fTransitionCY - fTopCY;
    float fSharpCY = fTopCY * 0.67f;

    float fStartX = (MIDI::IsSharp(m_iStartNote) ? m_fWhiteCX * SharpRatio / 2.0f : 0.0f);
    float fCurX = m_fNotesX + fStartX;
    for (int i = m_iStartNote; i <= m_iEndNote; i++) {
        if (!MIDI::IsSharp(i)) {
            fCurX += m_fWhiteCX;
        } else {
            float fNudgeX = 0.0;
            MIDI::Note eNote = MIDI::NoteVal(i);
            if (eNote == MIDI::CS || eNote == MIDI::FS) fNudgeX = -SharpRatio / 5.0f;
            else if (eNote == MIDI::AS || eNote == MIDI::DS) fNudgeX = SharpRatio / 5.0f;
            float cx = m_fWhiteCX * SharpRatio;
            float x = fCurX - m_fWhiteCX * (SharpRatio / 2.0f - fNudgeX);
            if ((float)mx >= x && (float)mx < x + cx &&
                (float)my >= fCurY - fNearCY && (float)my < fCurY + fSharpCY)
                return i;
        }
    }

    fCurX = m_fNotesX;
    for (int i = m_iStartNote; i <= m_iEndNote; i++) {
        if (!MIDI::IsSharp(i)) {
            if ((float)mx >= fCurX && (float)mx < fCurX + m_fWhiteCX && (float)my >= fCurY)
                return i;
            fCurX += m_fWhiteCX;
        }
    }

    return -1;
}

void FreePlayScreen::NoteOn(int note, int velocity, long long llStamp)
{
    int iRange = max(1, min(128, m_iFreePlayRange));
    bool bHit[128] = {};
    int iStart = note - (iRange - 1) / 2;
    for (int i = 0; i < iRange; i++) {
        int n = iStart + i;
        if (n < 0 || n >= 128) continue;
        NoteOnSingle(n, velocity, llStamp);
        m_iChordEvent[n] = (int)m_vEvents.size() - 1;
        bHit[n] = true;
    }
    if (m_bMirrorKeys) {
        iStart = (127 - note) - (iRange - 1) / 2;
        for (int i = 0; i < iRange; i++) {
            int n = iStart + i;
            if (n < 0 || n >= 128 || bHit[n]) continue;
            NoteOnSingle(n, velocity, llStamp);
            m_iChordEvent[n] = (int)m_vEvents.size() - 1;
        }
    }
    if (m_bRainbow) m_iRainbowOffset++;
}

void FreePlayScreen::NoteOnSingle(int note, int velocity, long long llStamp)
{
    m_OutDevice.PlayEvent(0x90, (unsigned char)note, (unsigned char)velocity);

    const long long ts = llStamp >= 0 ? llStamp : m_llFreePlayTime;

    int slot;
    if (m_vFreeSlots.empty()) {
        slot = -1;
        while (!m_dReleaseOrder.empty()) {
            int cand = m_dReleaseOrder.front();
            m_dReleaseOrder.pop_front();
            if (m_mReleasedNotes.find(cand) != m_mReleasedNotes.end()) {
                slot = cand;
                break;
            }
        }
        if (slot >= 0) {
            int nOld = m_MIDI.GetEventParam1(m_vEvents[slot]);
            auto& st = m_vState[nOld];
            st.erase(std::remove(st.begin(), st.end(), slot), st.end());
            m_mReleasedNotes.erase(slot);
            slot = m_MIDI.GetEventTrack(m_vEvents[slot]);
        }
        else slot = 0; // all notes held; should not happen at 65536 slots
    }
    else { slot = m_vFreeSlots.back(); m_vFreeSlots.pop_back(); }
    unsigned int ulColor;
    if (m_bPlaybackColorPinned) {
        ulColor = m_uPlaybackColor;
    } else if (m_bRainbow) {
        int iHue = (int)fmod((note + m_iRainbowOffset) * (360.0 / 128.0), 360.0);
        int r2, g2, b2;
        Util::HSVtoRGB(iHue, 100, 100, r2, g2, b2);
        ulColor = (0x00 << 24) | (b2 << 16) | (g2 << 8) | r2;
    } else {
        unsigned int r = (unsigned int)(m_fFreePlayColor[0] * 255.0f);
        unsigned int g = (unsigned int)(m_fFreePlayColor[1] * 255.0f);
        unsigned int b = (unsigned int)(m_fFreePlayColor[2] * 255.0f);
        ulColor = (0x00 << 24) | (b << 16) | (g << 8) | r;
    }
    m_vTrackSettings[slot].aChannels[0].SetColor(ulColor, 0.6f, 0.2f);
    m_pRenderer->MarkTrackColorsDirty(slot);

    m_pNoteState[note] = (int)m_vEvents.size();

    MIDIChannelEvent pEvent = m_MIDI.AppendChannelEvent(0, 0);
    m_MIDI.SetEventTrack(pEvent, (unsigned short)slot);
    m_MIDI.SetEventChannel(pEvent, 0);
    m_MIDI.SetEventParam1(pEvent, (unsigned char)note);
    m_MIDI.SetEventParam2(pEvent, (unsigned char)velocity);
    m_MIDI.SetEventTime(pEvent, ts);
    m_MIDI.SetEventSisterIdx(pEvent, UINT32_MAX);
    m_vEvents.push_back(pEvent);

    m_vState[note].push_back((int)m_vEvents.size() - 1);
    m_vThreadWork[note].push_back({ .idx = (unsigned)(m_vEvents.size() - 1), .sister_idx = UINT32_MAX });

    if (m_bRecording && !m_bPlayback && ts >= m_llRecordStart)
        m_vRecordingEvents.push_back({ true, (unsigned char)note, (unsigned char)velocity, ulColor, ts - m_llRecordStart });

    m_iFreePlayNoteCount++;
}

void FreePlayScreen::NoteOff(int note, bool bStretch, long long llStamp)
{
    int iRange = max(1, min(128, m_iFreePlayRange));
    bool bHit[128] = {};
    int iStart = note - (iRange - 1) / 2;
    for (int i = 0; i < iRange; i++) {
        int n = iStart + i;
        if (n < 0 || n >= 128) continue;
        NoteOffSingle(n, bStretch, llStamp);
        bHit[n] = true;
    }
    if (m_bMirrorKeys) {
        iStart = (127 - note) - (iRange - 1) / 2;
        for (int i = 0; i < iRange; i++) {
            int n = iStart + i;
            if (n < 0 || n >= 128 || bHit[n]) continue;
            NoteOffSingle(n, bStretch, llStamp);
        }
    }
}

void FreePlayScreen::ChordRelease(bool bStretch, long long llStamp)
{
    for (int n = 0; n < 128; n++) {
        if (m_iChordEvent[n] >= 0) {
            NoteOffSingle(n, bStretch, llStamp, m_iChordEvent[n]);
            m_iChordEvent[n] = -1;
        }
    }
}

void FreePlayScreen::SlideTo(int note)
{
    const int iRange = max(1, min(128, m_iFreePlayRange));
    bool bNew[128] = {};
    int iStart = note - (iRange - 1) / 2;
    for (int i = 0; i < iRange; i++) {
        int n = iStart + i;
        if (n >= 0 && n < 128) bNew[n] = true;
    }
    if (m_bMirrorKeys) {
        iStart = (127 - note) - (iRange - 1) / 2;
        for (int i = 0; i < iRange; i++) {
            int n = iStart + i;
            if (n >= 0 && n < 128) bNew[n] = true;
        }
    }
    for (int n = 0; n < 128; n++) {
        if (!bNew[n] && m_iChordEvent[n] >= 0) {
            NoteOffSingle(n, false, -1, m_iChordEvent[n]);
            m_iChordEvent[n] = -1;
        }
    }
    for (int n = 0; n < 128; n++) {
        if (bNew[n] && m_iChordEvent[n] < 0) {
            m_iChordEvent[n] = (int)m_vEvents.size();
            NoteOnSingle(n, 100);
        }
    }
}

void FreePlayScreen::NoteOffSingle(int note, bool bStretch, long long llStamp, int iSpecificIdx)
{
    const long long llRel = llStamp >= 0 ? llStamp : m_llFreePlayTime;
    if (m_bRecording && !m_bPlayback && llRel >= m_llRecordStart)
        m_vRecordingEvents.push_back({ false, (unsigned char)note, 0, 0, llRel - m_llRecordStart });

    if (iSpecificIdx >= 0) {
        auto& state = m_vState[note];
        auto it = std::find(state.begin(), state.end(), iSpecificIdx);
        if (it == state.end()) {
            m_pNoteState[note] = -1; // already released by something else
            return;
        }
        if (!state.empty() && state.back() == iSpecificIdx)
            m_OutDevice.PlayEvent(0x80, (unsigned char)note, 0);

        int idx = iSpecificIdx;
        MIDIChannelEvent pEvent = m_vEvents[idx];
        long long length = llRel - m_MIDI.GetEventTime(pEvent);
        if (length < 5000) length = 5000;
        m_MIDI.SetEventLength(pEvent, (unsigned)length);

        m_mReleasedNotes[idx] = { llRel, length };
        m_dReleaseOrder.push_back(idx);

        MIDIChannelEvent pOff = m_MIDI.AppendChannelEvent(0, 0);
        m_MIDI.SetEventTrack(pOff, m_MIDI.GetEventTrack(pEvent));
        m_MIDI.SetEventChannel(pOff, 0);
        m_MIDI.SetEventParam1(pOff, (unsigned char)note);
        m_MIDI.SetEventParam2(pOff, 0);
        m_MIDI.SetEventTime(pOff, llRel);
        m_MIDI.SetEventSisterIdx(pOff, (unsigned)idx);
        m_MIDI.SetEventSisterIdx(pEvent, (unsigned)m_vEvents.size());
        m_vEvents.push_back(pOff);

        // it scrolls off; the scroll-off cleanup reclaims it later.
        m_pNoteState[note] = -1;
        return;
    }

    m_OutDevice.PlayEvent(0x80, (unsigned char)note, 0);

    if (!m_vState[note].empty()) {
        int idx = m_vState[note].back();
        MIDIChannelEvent pEvent = m_vEvents[idx];
        long long length = llRel - m_MIDI.GetEventTime(pEvent);
        if (bStretch && m_fRepeaterNPS > 1.0f) {
            length = (long long)(1000000.0 / m_fRepeaterNPS);
        }
        else if (length < 5000) length = 5000;
        m_MIDI.SetEventLength(pEvent, (unsigned)length);

        m_mReleasedNotes[idx] = { llRel, length };
        m_dReleaseOrder.push_back(idx);

        MIDIChannelEvent pOff = m_MIDI.AppendChannelEvent(0, 0);
        m_MIDI.SetEventTrack(pOff, m_MIDI.GetEventTrack(pEvent));
        m_MIDI.SetEventChannel(pOff, 0);
        m_MIDI.SetEventParam1(pOff, (unsigned char)note);
        m_MIDI.SetEventParam2(pOff, 0);
        m_MIDI.SetEventTime(pOff, llRel);
        m_MIDI.SetEventSisterIdx(pOff, (unsigned)idx);
        m_MIDI.SetEventSisterIdx(pEvent, (unsigned)m_vEvents.size());
        m_vEvents.push_back(pOff);

    }

    m_pNoteState[note] = -1;
}

void FreePlayScreen::StartLoopRecording()
{
    if (m_bRecording || m_bCountdown) return;
    m_bCountdown = true;
    m_llCountdownStart = m_llFreePlayTime;
    m_vRecordingEvents.clear();
}

void FreePlayScreen::StopLoopRecording()
{
    if (!m_bRecording) return;
    m_bRecording = false;

    const long long dur = max(1LL, m_llRecordDuration);

    std::vector<LoopEvent> events;
    std::map<int, std::deque<std::tuple<long long, unsigned char, unsigned int>>> pending; // note -> (time, velocity, color)
    std::sort(m_vRecordingEvents.begin(), m_vRecordingEvents.end(),
        [](const LoopEvent& a, const LoopEvent& b) {
            if (a.time != b.time) return a.time < b.time;
            return a.isOn && !b.isOn; // note-ons before offs at the same instant
        });
    for (auto& e : m_vRecordingEvents) {
        if (e.isOn) {
            pending[e.note].push_back({ e.time, e.velocity, e.color });
        } else {
            auto& v = pending[e.note];
            if (v.empty()) continue; // off with no on (note was held before recording)
            // the same instant on the overlapping keys; pairing newest-first
            auto p = v.front();
            v.pop_front();
            events.push_back({ true, e.note, std::get<1>(p), std::get<2>(p), std::get<0>(p) });
            if (e.time < dur) // offs at the loop boundary are handled by the wrap
                events.push_back({ false, e.note, 0, 0, e.time });
        }
    }
    for (auto& kv : pending) {
        for (auto& p : kv.second)
            events.push_back({ true, (unsigned char)kv.first, std::get<1>(p), std::get<2>(p), std::get<0>(p) });
    }
    std::sort(events.begin(), events.end(),
        [](const LoopEvent& a, const LoopEvent& b) {
            if (a.time != b.time) return a.time < b.time;
            return !a.isOn && b.isOn; // offs first: release before the next strike on the same key
        });

    Loop L;
    for (int i = 0; i < 128; i++) L.held[i] = -1;
    if (!events.empty()) {
        snprintf(L.name, sizeof(L.name), "Loop %d", ++m_iLoopCounter);
        L.duration = dur;
        L.events = std::move(events);
        L.lastTick = m_llFreePlayTime;
        m_vLoops.push_back(std::move(L));
    }
    m_vRecordingEvents.clear();
}

void FreePlayScreen::TickLooper()
{
    if (m_bCountdown) {
        if (m_llFreePlayTime - m_llCountdownStart >= 3000000LL) {
            m_bCountdown = false;
            m_bRecording = true;
            m_llRecordStart = m_llFreePlayTime;
        }
    }
    if (m_bRecording && m_llFreePlayTime - m_llRecordStart >= m_llRecordDuration)
        StopLoopRecording();

    const long long tick = m_llFreePlayTime;
    m_bPlayback = true;
    for (auto& L : m_vLoops) {
        if (!L.playing || L.duration <= 0 || L.events.empty()) continue;
        if (L.lastTick == 0) { L.lastTick = tick; L.playhead = 0; }
        long long elapsed = tick - L.lastTick;
        L.lastTick = tick;
        if (elapsed <= 0) continue;
        L.playhead += elapsed;

        int guard = 0;
        while (L.playhead >= L.duration && guard++ < 100000) {
            L.playhead -= L.duration;
            for (int n = 0; n < 128; n++) {
                if (L.held[n] >= 0) {
                    NoteOffSingle(n, false, -1, L.held[n]);
                    L.held[n] = -1;
                }
            }
            L.nextEvent = 0;
        }
        while (L.nextEvent < (int)L.events.size() && L.events[L.nextEvent].time <= L.playhead) {
            const LoopEvent& e = L.events[L.nextEvent++];
            if (e.isOn) {
                m_bPlaybackColorPinned = true;
                m_uPlaybackColor = L.bColorOverride ? L.uColorOverride : e.color;
                int v = max(1, min(127, (int)(e.velocity * L.velocity)));
                int iIdx = (int)m_vEvents.size(); // the event NoteOnSingle is about to push
                NoteOnSingle(e.note, v, -1);
                m_bPlaybackColorPinned = false;
                L.held[e.note] = iIdx;
            } else {
                NoteOffSingle(e.note, false, -1, L.held[e.note]);
                L.held[e.note] = -1;
            }
        }
    }
    m_bPlayback = false;
}

void FreePlayScreen::DeleteLoop(int i)
{
    if (i < 0 || i >= (int)m_vLoops.size()) return;
    m_bPlayback = true;
    for (int n = 0; n < 128; n++) {
        if (m_vLoops[i].held[n] >= 0) {
            NoteOffSingle(n, false, -1, m_vLoops[i].held[n]);
            m_vLoops[i].held[n] = -1;
        }
    }
    m_bPlayback = false;
    m_vLoops.erase(m_vLoops.begin() + i);
}

GameState::GameError FreePlayScreen::MsgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_LBUTTONDOWN: {
        int mx = (short)LOWORD(lParam);
        int my = (short)HIWORD(lParam);
        int note = NoteFromMousePos(mx, my);
        if (note >= 0 && note < 128) {
            m_bMouseDown = true;
            m_iLastClickedNote = note;
            NoteOn(note, 100);
        }
        return Success;
    }
    case WM_LBUTTONUP: {
        if (m_bMouseDown && m_iLastClickedNote >= 0) {
            ChordRelease();
            m_bMouseDown = false;
            m_iLastClickedNote = -1;
        }
        return Success;
    }
    case WM_MOUSEMOVE: {
        if (m_bMouseDown) {
            int mx = (short)LOWORD(lParam);
            int my = (short)HIWORD(lParam);
            int note = NoteFromMousePos(mx, my);
            if (note >= 0 && note < 128 && note != m_iLastClickedNote) {
                SlideTo(note);
                m_iLastClickedNote = note;
            }
        }
        return Success;
    }
    }

    return MainScreen::MsgProc(hWnd, msg, wParam, lParam);
}

GameState::GameError FreePlayScreen::Logic()
{
    m_pRenderer->ImGuiStartFrame();

    static Config& config = Config::GetConfig();
    static PlaybackSettings& cPlayback = config.GetPlaybackSettings();
    static const ViewSettings& cView = config.GetViewSettings();
    static const VisualSettings& cVisual = config.GetVisualSettings();
    static const VideoSettings& cVideo = config.GetVideoSettings();
    static const AudioSettings& cAudio = config.GetAudioSettings();
    static const VizSettings& cViz = config.GetVizSettings();

    m_csKBRed.SetColor(cViz.iBarColor, 0.5f);
    m_bPaused = false;
    m_dSpeed = 1.0;
    m_dVolume = cPlayback.GetVolume();
    m_bShowKB = cView.GetKeyboard();
    m_bZoomMove = cView.GetZoomMove();
    m_fOffsetX = cView.GetOffsetX();
    m_fOffsetY = cView.GetOffsetY();
    m_fZoomX = cView.GetZoomX();
    m_eKeysShown = cVisual.eKeysShown;
    m_iStartNote = min(cVisual.iFirstKey, cVisual.iLastKey);
    m_iEndNote = max(cVisual.iFirstKey, cVisual.iLastKey);
    m_bShowFPS = cVideo.bShowFPS;

    if (cViz.bKDMAPI != m_OutDevice.IsKDMAPI()) {
        if (cViz.bKDMAPI)
            m_OutDevice.OpenKDMAPI();
        else if (cAudio.iOutDevice >= 0)
            m_OutDevice.Open(cAudio.iOutDevice);
        m_OutDevice.Reset();
    }

    long long llElapsed = m_RealTimer.GetMicroSecs();
    m_RealTimer.Start();
    m_llFreePlayTime += llElapsed;

    m_llFPSTime += llElapsed;
    m_iFPSCount++;
    if (m_llFPSTime >= 500000) {
        m_dFPS = m_iFPSCount / (m_llFPSTime / 1000000.0);
        m_llFPSTime = m_iFPSCount = 0;
    }

    m_llStartTime = m_llFreePlayTime;
    m_llDisplayTime = m_llFreePlayTime;
    m_llTimeSpan = 3000000LL;
    m_llRndStartTime = m_llFreePlayTime;

    m_fNotesX = m_fOffsetX + m_fTempOffsetX;
    m_fNotesCX = m_pRenderer->GetBufferWidth() * m_fZoomX * m_fTempZoomX;
    m_iAllWhiteKeys = MIDI::WhiteCount(m_iStartNote, m_iEndNote + 1);
    float fBuffer = (MIDI::IsSharp(m_iStartNote) ? SharpRatio / 2.0f : 0.0f) +
                    (MIDI::IsSharp(m_iEndNote) ? SharpRatio / 2.0f : 0.0f);
    m_fWhiteCX = m_fNotesCX / (m_iAllWhiteKeys + fBuffer);
    m_fNotesY = m_fOffsetY + m_fTempOffsetY;
    if (!m_bShowKB)
        m_fNotesCY = static_cast<float>(m_pRenderer->GetBufferHeight());
    else {
        float fMaxKeyCY = m_pRenderer->GetBufferHeight() * KBPercent;
        float fIdealKeyCY = m_fWhiteCX / KeyRatio;
        fIdealKeyCY = (fIdealKeyCY / 0.95f + 2.0f) / 0.93f;
        m_fNotesCY = floor(m_pRenderer->GetBufferHeight() - min(fIdealKeyCY, fMaxKeyCY) + 0.5f);
    }
    GenNoteXTable();

    auto& root_consts = m_pRenderer->GetRootConstants();
    root_consts.deflate = clamp(round(m_fWhiteCX * 0.15f / 2.0f), 1.0f, 3.0f);
    root_consts.notes_y = m_fNotesY;
    root_consts.notes_cy = m_fNotesCY;
    root_consts.white_cx = m_fWhiteCX;
    root_consts.timespan = (float)m_llTimeSpan;
    root_consts.notes_x = m_fNotesX;
    root_consts.notes_cx = m_fNotesCX;

    auto& fixed_consts = m_pRenderer->GetFixedSizeConstants();
    memcpy(&fixed_consts.note_x, &notex_table, sizeof(float) * 128);
    memset(&fixed_consts.bends, 0, sizeof(float) * 16);

    SyncTrackColors(m_pRenderer, m_vTrackSettings);

    ImGui::SetNextWindowPos(ImVec2(4.0f, 24.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340.0f, 660.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(280.0f, 220.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.75f);

    if (ImGui::Begin("Free Play Controls")) {
        ImGui::Text("Note color:");
        ImGui::SameLine();
        ImGui::Checkbox("Rainbow", &m_bRainbow);
        ImGui::ColorPicker4("##fpcolor", m_fFreePlayColor,
            ImGuiColorEditFlags_NoInputs);
        ImGui::Separator();
        ImGui::Text("Note speed:");
        ImGui::SliderFloat("##fpspeed", &m_fFreePlaySpeed, 0.1f, 4.0f, "%.2fx");
        ImGui::Separator();
        ImGui::Text("Repeater:");
        ImGui::SliderFloat("##fprepeater", &m_fRepeaterNPS, 0.0f, 100000.0f, "%.0f nps",
            ImGuiSliderFlags_Logarithmic);
        ImGui::Separator();
        ImGui::Text("Key range:");
        ImGui::SliderInt("##fprange", &m_iFreePlayRange, 1, 128, "%d keys");
        ImGui::SameLine();
        ImGui::Checkbox("Mirror", &m_bMirrorKeys);
        ImGui::Separator();
        ImGui::Button("SLAM", ImVec2(-1.0f, 0.0f));
        const bool bSlamDown = ImGui::IsItemActive();
        if (bSlamDown && !m_bSlamHeld) {
            m_bSlamHeld = true;
            if (m_fRepeaterNPS > 0.0f) {
                // interval; make the repeater's first fire immediate instead.
                m_llRepeaterLast = m_llFreePlayTime -
                    (long long)(1000000.0 / m_fRepeaterNPS);
            } else {
                for (int i = 0; i < 128; i++) {
                    if (m_pNoteState[i] == -1)
                        NoteOnSingle(i, 100);
                }
            }
        }
        if (!bSlamDown && m_bSlamHeld) {
            m_bSlamHeld = false;
            for (int i = 0; i < 128; i++)
                NoteOffSingle(i);
        }
        ImGui::Separator();
        ImGui::Text("Looper:");
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::DragFloat("sec", &m_fRecordSeconds, 0.25f, 0.5f, 120.0f, "%.1f"))
            m_llRecordDuration = (long long)(m_fRecordSeconds * 1000000.0);
        ImGui::SameLine();
        if (ImGui::Button(m_bCountdown ? "Cancel" : (m_bRecording ? "Recording..." : "Record"))) {
            if (m_bRecording) StopLoopRecording();
            else if (m_bCountdown) m_bCountdown = false;
            else StartLoopRecording();
        }
        if (m_bCountdown) {
            double dStart = ceil((3000000LL - (m_llFreePlayTime - m_llCountdownStart)) / 1000000.0);
            if (dStart < 0.0) dStart = 0.0;
            ImGui::SameLine();
            ImGui::Text("starts in %.0f", dStart);
        } else if (m_bRecording) {
            ImGui::SameLine();
            double dLeft = (double)(m_llRecordDuration - (m_llFreePlayTime - m_llRecordStart)) / 1000000.0;
            if (dLeft < 0.0) dLeft = 0.0;
            ImGui::Text("%.1fs left", dLeft);
        }
        if (ImGui::BeginChild("##looperList", ImVec2(0.0f, 148.0f), true)) {
            if (m_vLoops.empty()) {
                ImGui::TextDisabled("No loops yet. Press Record and play some keys.");
            } else {
                for (int i = 0; i < (int)m_vLoops.size(); i++) {
                    ImGui::PushID(i);
                    Loop& L = m_vLoops[i];
                    ImGui::SetNextItemWidth(150.0f);
                    ImGui::InputText("##name", L.name, 64);
                    ImGui::SameLine();
                    int iOns = (int)std::count_if(L.events.begin(), L.events.end(),
                        [](const LoopEvent& e) { return e.isOn; });
                    ImGui::Text("%d n %.2fs", iOns, L.duration / 1000000.0);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(70.0f);
                    ImGui::DragFloat("##vel", &L.velocity, 0.05f, 0.0f, 2.0f, "vel %.2f");
                    ImGui::SameLine();
                    if (ImGui::ColorEdit3("##col", L.color,
                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoLabel)) {
                        L.bColorOverride = true;
                        unsigned int r = (unsigned int)(L.color[0] * 255.0f);
                        unsigned int g = (unsigned int)(L.color[1] * 255.0f);
                        unsigned int b = (unsigned int)(L.color[2] * 255.0f);
                        L.uColorOverride = (0x00 << 24) | (b << 16) | (g << 8) | r;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Delete")) {
                        DeleteLoop(i);
                        ImGui::PopID();
                        continue;
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();
        ImGui::End();
    }

    const bool bHolding = m_bSlamHeld || (m_bMouseDown && m_iLastClickedNote >= 0);
    if (m_fRepeaterNPS > 0.0f && bHolding) {
        const long long llPeriod = (long long)(1000000.0 / m_fRepeaterNPS);
        if (llPeriod > 0) {
            int iFires = 0;
            while (m_llFreePlayTime - m_llRepeaterLast >= llPeriod) {
                m_llRepeaterLast += llPeriod;
                const long long llStamp = m_llRepeaterLast;
                if (m_bSlamHeld) {
                    for (int i = 0; i < 128; i++)
                        NoteOffSingle(i, true, llStamp);
                    for (int i = 0; i < 128; i++) {
                        if (m_pNoteState[i] == -1)
                            NoteOnSingle(i, 100, llStamp);
                    }
                } else {
                    ChordRelease(true, llStamp);
                    NoteOn(m_iLastClickedNote, 100, llStamp);
                }
                if (++iFires > 10000) break; // safety cap per frame
            }
        }
    } else {
        m_llRepeaterLast = m_llFreePlayTime;
    }

    TickLooper();

    for (int n = 0; n < 128; n++) {
        auto& state = m_vState[n];
        for (auto it = state.begin(); it != state.end(); ) {
            auto relIt = m_mReleasedNotes.find(*it);
            if (relIt != m_mReleasedNotes.end()) {
                float elapsed = (float)(m_llFreePlayTime - relIt->second.releaseTime);
                if (elapsed * m_fFreePlaySpeed >= (float)m_llTimeSpan) {
                    m_vFreeSlots.push_back(m_MIDI.GetEventTrack(m_vEvents[*it]));
                    m_mReleasedNotes.erase(relIt);
                    it = state.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }
    while (!m_dReleaseOrder.empty() &&
        m_mReleasedNotes.find(m_dReleaseOrder.front()) == m_mReleasedNotes.end())
        m_dReleaseOrder.pop_front();

    m_dNPSNotes.push_back(make_tuple(m_llFreePlayTime, m_iFreePlayNoteCount));
    m_iFreePlayNoteCount = 0;

    while (!m_dNPSNotes.empty() && std::get<0>(m_dNPSNotes.front()) < m_llFreePlayTime - 1000000)
        m_dNPSNotes.pop_front();

    long long lagNps = 0;
    for (size_t i = 0; i < m_dNPSNotes.size(); i++)
        lagNps += std::get<1>(m_dNPSNotes[i]);
    m_pRenderer->SetLagNPS(lagNps);

    m_dNPSHistory.push_back(lagNps);
    if ((int)m_dNPSHistory.size() > 600)
        m_dNPSHistory.pop_front();

    m_pRenderer->ClearAndBeginScene(0xFF000000);
    RenderLines();
    m_pRenderer->SplitRect();
    for (int n = 0; n < 128; n++) {
        for (auto it = m_vState[n].rbegin(); it != m_vState[n].rend(); it++)
            RenderNoteIdx(*it);
    }
    m_pRenderer->RenderBatch(true);
    if (m_bShowKB) RenderKeys();
    RenderBorder();
    RenderText();
    m_pRenderer->EndScene(true);
    m_pRenderer->Present();

    return Success;
}

NoteData FreePlayScreen::BuildRenderNoteData(const MIDIChannelEvent pNote) const
{
    int idx = -1;
    int note = m_MIDI.GetEventParam1(pNote);
    for (int i : m_vState[note]) {
        if (m_vEvents[i] == pNote) { idx = i; break; }
    }

    auto relIt = (idx >= 0) ? m_mReleasedNotes.find(idx) : m_mReleasedNotes.end();
    if (relIt != m_mReleasedNotes.end()) {
        long long elapsed = m_llFreePlayTime - relIt->second.releaseTime;
        return NoteData{
            .key = (uint8_t)note,
            .channel = 0,
            .track = (uint16_t)m_MIDI.GetEventTrack(pNote),
            .pos = static_cast<float>(elapsed) * m_fFreePlaySpeed,
            .length = static_cast<float>(relIt->second.finalLength) * m_fFreePlaySpeed,
        };
    }

    long long llNoteLength = (m_llFreePlayTime - m_MIDI.GetEventTime(pNote)) * (long long)m_fFreePlaySpeed;
    if (llNoteLength < 5000) llNoteLength = 5000;

    return NoteData{
        .key = (uint8_t)note,
        .channel = 0,
        .track = (uint16_t)m_MIDI.GetEventTrack(pNote),
        .pos = 0.0f,
        .length = static_cast<float>(llNoteLength),
    };
}

void FreePlayScreen::RenderNoteIdx(int idx)
{
    auto relIt = m_mReleasedNotes.find(idx);
    if (relIt != m_mReleasedNotes.end()) {
        MIDIChannelEvent pNote = m_vEvents[idx];
        int note = m_MIDI.GetEventParam1(pNote);
        long long elapsed = m_llFreePlayTime - relIt->second.releaseTime;
        m_pRenderer->PushNoteData(NoteData{
            .key = (uint8_t)note,
            .channel = 0,
            .track = (uint16_t)m_MIDI.GetEventTrack(pNote),
            .pos = static_cast<float>(elapsed) * m_fFreePlaySpeed,
            .length = static_cast<float>(relIt->second.finalLength) * m_fFreePlaySpeed,
        });
    } else {
        MIDIChannelEvent pNote = m_vEvents[idx];
        int note = m_MIDI.GetEventParam1(pNote);
        long long llNoteLength = (m_llFreePlayTime - m_MIDI.GetEventTime(pNote)) * (long long)m_fFreePlaySpeed;
        if (llNoteLength < 5000) llNoteLength = 5000;
        m_pRenderer->PushNoteData(NoteData{
            .key = (uint8_t)note,
            .channel = 0,
            .track = (uint16_t)m_MIDI.GetEventTrack(pNote),
            .pos = 0.0f,
            .length = static_cast<float>(llNoteLength),
        });
    }
}
