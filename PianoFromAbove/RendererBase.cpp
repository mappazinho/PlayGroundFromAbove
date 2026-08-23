/*************************************************************************************************
*
> File: RendererBase.cpp
*
> Description: Implements the API-agnostic parts of the renderer: batching, UI, state.
*
> Copyright (c) 2010 Brian Pantano. All rights reserved.
*
*************************************************************************************************/
#include "Globals.h"
#include "Renderer.h"

#include <chrono>
#include <cmath>
#include <ShlObj.h>
#include <psapi.h>
#include <mmsystem.h>
#include "resource.h"
#include "Config.h"
#include "Misc.h"
#include "MIDI.h"
#include "MIDIPreRenderPlayer.h"
#include "BASSMIDI.h"
#include "MainProcs.h"
#include "RendererD3D11.h"

// Picks the D3D12 backend when the user asked for it and this machine can
// actually create a D3D12 device (probed once at startup here); otherwise
// D3D11. The D3D12 option is disabled in the UI whenever g_bD3D12Available
// is false or the session already fell back (g_bBootedFallback).
Renderer* Renderer::CreateInstance() {
    ComPtr<ID3D12Device> probe;
    g_bD3D12Available = SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&probe)));
    const auto& cVideo = Config::GetConfig().GetVideoSettings();
    if (cVideo.eRenderer == VideoSettings::DirectX12 && g_bD3D12Available && !g_bBootedFallback)
        return new D3D12Renderer();
    return new D3D11Renderer();
}

static bool PickFolderDialog(HWND hwndOwner, const wchar_t* title, std::wstring& outFolderPath) {
    IFileDialog* pfd = NULL;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
    if (SUCCEEDED(hr))
    {
        DWORD dwOptions;
        if (SUCCEEDED(pfd->GetOptions(&dwOptions)))
        {
            pfd->SetOptions(dwOptions | FOS_PICKFOLDERS);
        }
        if (title) pfd->SetTitle(title);
        if (SUCCEEDED(pfd->Show(hwndOwner)))
        {
            IShellItem* psiResult;
            if (SUCCEEDED(pfd->GetResult(&psiResult)))
            {
                PWSTR pszPath = NULL;
                if (SUCCEEDED(psiResult->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)))
                {
                    outFolderPath = pszPath;
                    CoTaskMemFree(pszPath);
                    psiResult->Release();
                    pfd->Release();
                    return true;
                }
                psiResult->Release();
            }
        }
        pfd->Release();
    }
    return false;
}

ComPtr<IWICImagingFactory> Renderer::s_pWICFactory;

HRESULT Renderer::ResetDeviceIfNeeded() {
    // Device-loss recovery is owned by the game loop: it drains the pipeline
    // and rebuilds the device via RecoverDevice() whenever DeviceLost() is
    // set, and only calls Logic()/Render() again once the backend is valid.
    // Failing the frame here is just a safety net so the caller never renders
    // into a dead device.
    if (m_bDeviceLost)
        return E_FAIL;
    return S_OK;
}

HRESULT Renderer::RecoverDevice(HWND hWnd, bool bLimitFPS) {
    HeartbeatLog("recover:start");
    ImGuiBackendShutdown();
    ImGui_ImplWin32_Shutdown();
    ReleaseDeviceResources();
    m_bDeviceLost = false;
    g_bInRecovery = true;
    auto res = Init(hWnd, bLimitFPS);
    g_bInRecovery = false;
    HeartbeatLog("recover:done");
    return std::get<0>(res);
}

void Renderer::UpdateWarpConstants() {
    // Overclock artifacts: geometry warps harder the farther below 60 FPS we are.
    // The range is exponential on purpose: mild lag is nearly invisible, and
    // things only start tearing once the frame rate truly collapses.
    if (m_bOverclockArtifacts && m_dLagFPS > 0.0) {
        float inst = m_dLagFPS < 60.0 ? (float)(60.0 - m_dLagFPS) / 59.0f : 0.0f;
        inst = inst * inst * inst * inst;
        m_RootConstants.fWarp = min(1.0f, inst);
    } else {
        m_RootConstants.fWarp = 0.0f;
    }
    static std::chrono::steady_clock::time_point s_warpLast = std::chrono::steady_clock::now();
    const auto warpNow = std::chrono::steady_clock::now();
    float dt = (float)std::chrono::duration<double>(warpNow - s_warpLast).count();
    s_warpLast = warpNow;
    m_RootConstants.fWarpTime += min(dt, 0.25f);
    m_RootConstants.fWarpSeedX = (float)(rand() % 10000) / 10000.0f;
    m_RootConstants.fWarpSeedY = (float)(rand() % 10000) / 10000.0f;
    m_RootConstants.fMT = m_bMTMode ? 1.0f : 0.0f;
    m_RootConstants.fMTTilt = m_fMTTilt;
}

// Shared Present front-half: FPS EMA, lag intensifier, frame-rate stats, and the
// minimized-window gate. Returns true when the frame must be dropped (minimized
// gate active) and Present() should not touch the swapchain.
bool Renderer::PresentPrelude() {
    HeartbeatLog("present:start");
    static int s_branch = -1;
    static int s_frames = 0;
    static long long s_fpsStart = 0;

    // EMA-smoothed FPS feed for the overclock artifact effect
    static std::chrono::steady_clock::time_point s_fpsLast;
    const auto fpsNow = std::chrono::steady_clock::now();
    const double fpsDt = std::chrono::duration<double>(fpsNow - s_fpsLast).count();
    s_fpsLast = fpsNow;
    if (fpsDt > 0.0 && fpsDt < 1.0) {
        const double inst = 1.0 / fpsDt;
        m_dLagFPS = m_dLagFPS > 0.0 ? m_dLagFPS * 0.9 + inst * 0.1 : inst;
    }
    int branch = 0; // 0 = minimized skip, 1 = vsync, 2 = tearing
    if (IsIconic(g_hWnd)) {
        branch = 0;
    } else if (m_bLimitFPS)
        branch = 1;
    else
        branch = 2;
    if (branch != s_branch) {
        s_branch = branch;
        if (branch == 0) HeartbeatLog("present:minimized-skip");
        else if (branch == 1) HeartbeatLog("present:vsync-on");
        else HeartbeatLog("present:tearing-on");
    }

    // Lag Intensifier: throttle the frame rate by NPS tier. 1x does nothing.
    // 2x activates at >= 1M NPS, 3x halves that threshold, 4x halves it again.
    // FPS caps are randomly re-rolled once per second within the tier's range.
    if (m_iLagIntensity > 1 && branch != 0 && !g_bVideoRendering) {
        const long long tier = 1000000LL >> (m_iLagIntensity - 1);
        int capMin, capMax;
        if (m_llLagNPS >= tier * 2) {
            capMin = 1; capMax = 5;
        } else if (m_llLagNPS >= tier) {
            capMin = 15; capMax = 20;
        } else {
            capMin = 30; capMax = 60;
        }
        static std::chrono::steady_clock::time_point s_lastRoll;
        static int s_capMin = 0, s_capMax = 0, s_cap = 60;
        static std::chrono::steady_clock::time_point s_lastPresent;
        const auto now = std::chrono::steady_clock::now();
        const bool tierChanged = s_capMin != capMin || s_capMax != capMax;
        const double sinceRoll = std::chrono::duration<double>(now - s_lastRoll).count();
        if (tierChanged || sinceRoll >= 1.0) {
            s_capMin = capMin; s_capMax = capMax;
            s_cap = capMin + (int)(rand() % (capMax - capMin + 1));
            s_lastRoll = now;
            static bool s_periodSet = false;
            if (!s_periodSet) {
                timeBeginPeriod(1);
                s_periodSet = true;
            }
        }
        const double frameMs = 1000.0 / s_cap;
        const double elapsed = std::chrono::duration<double, std::milli>(now - s_lastPresent).count();
        const double sleepMs = frameMs - elapsed;
        if (sleepMs > 0.5)
            Sleep((DWORD)sleepMs);
        s_lastPresent = std::chrono::steady_clock::now();
        static int s_lagLog = 0;
        if ((s_lagLog++ & 63) == 0) {
            char buf[96];
            sprintf_s(buf, "lag:nps=%lld tier=%lld cap=%d", m_llLagNPS, tier, s_cap);
            HeartbeatLog(buf);
        }
    }
    s_frames++;
    if (s_frames == 1 || s_frames == 128) {
        long long now = GetTickCount64();
        if (s_frames == 1) s_fpsStart = now;
        else {
            char buf[64];
            sprintf_s(buf, "present:fps=%d wait=%.2f", (int)llround(128000.0 / max(now - s_fpsStart, 1)),
                m_dPresentWaitMs / (double)s_frames);
            HeartbeatLog(buf);
            m_dPresentWaitMs = 0.0;
            s_frames = 0;
        }
    }
    if (!g_bDisableGates && IsIconic(g_hWnd)) {
        Sleep(16);
        return true;
    }
    return false;
}

HRESULT Renderer::Present() {
    if (PresentPrelude())
        return S_OK;
    return PresentBackend();
}

float Renderer::GetDualRollTimeSpan(float normalTimeSpan, float normalRollPixels) const {
    const float normalPixels = max(normalRollPixels, 1.0f);
    const float stripPixels = max((float)m_iBufferWidth, 1.0f);
    return max(normalTimeSpan * 2.0f * stripPixels / normalPixels, 1.0f);
}

HRESULT Renderer::BeginText() {
    ImGui::Render();
    m_pDrawList->_ResetForNewFrame();
    m_pDrawList->PushClipRectFullScreen();
    m_pDrawList->PushTextureID(ImGui::GetIO().Fonts->TexID);
    return S_OK;
}

HRESULT Renderer::DrawTextW(const WCHAR*, FontSize, LPRECT, DWORD, DWORD, INT) {
    return S_OK;
}

HRESULT Renderer::DrawTextA(const CHAR*, FontSize, LPRECT, DWORD, DWORD, INT) {
    return S_OK;
}

HRESULT Renderer::EndText() {
    return S_OK;
}

HRESULT Renderer::DrawRect(float x, float y, float cx, float cy, DWORD color) {
    return DrawRect(x, y, cx, cy, color, color, color, color);
}

HRESULT Renderer::DrawRect(float x, float y, float cx, float cy, DWORD c1, DWORD c2, DWORD c3, DWORD c4) {
    m_vRectsIntermediate.insert(m_vRectsIntermediate.end(), {
        {x,      y,      c1},
        {x + cx, y,      c2},
        {x + cx, y + cy, c3},
        {x,      y + cy, c4},
    });
    return S_OK;
}

HRESULT Renderer::DrawSkew(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, DWORD color) {
    return DrawSkew(x1, y1, x2, y2, x3, y3, x4, y4, color, color, color, color);
}

HRESULT Renderer::DrawSkew(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, DWORD c1, DWORD c2, DWORD c3, DWORD c4) {
    m_vRectsIntermediate.insert(m_vRectsIntermediate.end(), {
        {x1, y1, c1},
        {x2, y2, c2},
        {x3, y3, c3},
        {x4, y4, c4},
    });
    return S_OK;
}

HRESULT Renderer::RenderBatch(bool) {
    return S_OK;
}

HRESULT Renderer::SetLimitFPS(bool bLimitFPS) {
    m_bLimitFPS = bLimitFPS;
    return S_OK;
}

// --- Image buffer (pre-rendered note chunks) --------------------------------

void Renderer::ImageBufferBeginFrame() {
    m_vChunkNotes.clear();
    m_vChunkBuilds.clear();
    m_vChunkQuads.clear();
    m_uImageBufferFrame++;

    m_bImageBufferCanRender = false;
    // Warp moves every vertex every frame; the baked chunks would be stale.
    if (m_RootConstants.fWarp > 0.0f)
        return;

    ChunkCacheKey key = {};
    key.width = m_iBufferWidth;
    key.height = m_iBufferHeight;
    key.deflate = m_RootConstants.deflate;
    key.notes_y = m_RootConstants.notes_y;
    key.notes_cy = m_RootConstants.notes_cy;
    key.white_cx = m_RootConstants.white_cx;
    key.timespan = m_RootConstants.timespan;
    key.stripTimeSpan = m_RootConstants.stripTimeSpan;
    key.fWarp = m_RootConstants.fWarp;
    key.fWarpSeedX = m_RootConstants.fWarpSeedX;
    key.fWarpSeedY = m_RootConstants.fWarpSeedY;
    key.notes_x = m_RootConstants.notes_x;
    key.notes_cx = m_RootConstants.notes_cx;
    key.fMT = m_RootConstants.fMT;
    key.fMTTilt = m_RootConstants.fMTTilt;
    key.corruption = m_fCorruption;
    key.eventCount = m_ullImageBufferEventCount;
    key.fixedStamp = m_uImageBufferFixedStamp;
    key.trackColorStamp = m_uImageBufferTrackColorStamp;

    if (!(key == m_ImageBufferKey)) {
        // Content-affecting change invalidates every cached chunk.
        for (auto& entry : m_ChunkCache)
            entry.chunk = ImageBufferInvalidChunk;
        m_ImageBufferKey = key;
    }
    m_bImageBufferCanRender = true;
}

int Renderer::ImageBufferGetChunkSlot(long long chunk) const {
    for (unsigned i = 0; i < ChunkPoolSize; i++)
        if (m_ChunkCache[i].chunk == chunk)
            return (int)i;
    return -1;
}

int Renderer::ImageBufferAllocateSlot() {
    int slot = -1;
    for (unsigned i = 0; i < ChunkPoolSize; i++)
        if (m_ChunkCache[i].chunk == ImageBufferInvalidChunk) {
            slot = (int)i;
            break;
        }
    if (slot < 0) {
        // Evict the lowest-numbered chunk that is not queued for drawing this
        // frame (oldest screens recycle first; visible chunks are protected).
        long long bestChunk = 0;
        int bestSlot = -1;
        bool found = false;
        for (unsigned i = 0; i < ChunkPoolSize; i++) {
            const long long chunk = m_ChunkCache[i].chunk;
            if (chunk == ImageBufferInvalidChunk || chunk == ImageBufferInvalidChunk - 1)
                continue;
            bool bVisible = false;
            for (const auto& quad : m_vChunkQuads) {
                if (quad.chunk == chunk) {
                    bVisible = true;
                    break;
                }
            }
            if (!bVisible && (!found || chunk < bestChunk)) {
                bestChunk = chunk;
                bestSlot = (int)i;
                found = true;
            }
        }
        slot = bestSlot;
    }
    if (slot < 0) {
        // Every slot is visible this frame (> ChunkPoolSize quads); fall back
        // to LRU so allocation still succeeds.
        unsigned oldest = UINT_MAX;
        for (unsigned i = 0; i < ChunkPoolSize; i++)
            if (m_ChunkCache[i].lastUsed < oldest) {
                oldest = m_ChunkCache[i].lastUsed;
                slot = (int)i;
            }
    }
    if (slot >= 0)
        m_ChunkCache[slot].chunk = ImageBufferInvalidChunk - 1; // in-use
    return slot;
}

void Renderer::ImageBufferMarkBaked(int slot, long long chunk) {
    if (slot < 0 || slot >= (int)ChunkPoolSize)
        return;
    m_ChunkCache[slot].chunk = chunk;
    m_ChunkCache[slot].lastUsed = m_uImageBufferFrame;
}

unsigned Renderer::ImageBufferGetCachedCount() const {
    unsigned count = 0;
    for (unsigned i = 0; i < ChunkPoolSize; i++) {
        if (m_ChunkCache[i].chunk != ImageBufferInvalidChunk &&
            m_ChunkCache[i].chunk != ImageBufferInvalidChunk - 1) {
            count++;
        }
    }
    return count;
}

bool Renderer::ImageBufferChunkCached(long long chunk) const {
    return ImageBufferGetChunkSlot(chunk) >= 0;
}

bool Renderer::ImageBufferRenderChunk(long long chunk, const NoteData* notes, unsigned noteCount) {
    if (!notes)
        noteCount = 0;
    noteCount = min(noteCount, (unsigned)MaxNotesPerPass);
    if (m_vChunkNotes.size() + noteCount > MaxNotesPerPass)
        return false; // per-frame budget hit; caller falls back to the note path
    m_vChunkBuilds.push_back({ chunk, (unsigned)m_vChunkNotes.size(), noteCount });
    if (notes && noteCount > 0)
        m_vChunkNotes.insert(m_vChunkNotes.end(), notes, notes + noteCount);
    return true;
}

void Renderer::ImageBufferDrawChunk(long long chunk, float yTop, float yBottom) {
    m_vChunkQuads.push_back({ chunk, yTop, yBottom });
}

void Renderer::ImageBufferBuildChunkFixed(FixedSizeConstants& out) const {
    out = m_FixedConstants;
    const float notes_x = m_RootConstants.notes_x;
    for (int i = 0; i < 128; i++)
        out.note_x[i] -= notes_x;
}

bool Renderer::LoadBackgroundBitmap(std::wstring path) {
    if (!s_pWICFactory) {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&s_pWICFactory))))
            return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(s_pWICFactory->CreateDecoderFromFilename(path.c_str(), NULL, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)))
        return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
        return false;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(s_pWICFactory->CreateFormatConverter(&converter)))
        return false;

    WICPixelFormatGUID orig_pixel_format;
    if (FAILED(frame->GetPixelFormat(&orig_pixel_format)))
        return false;

    BOOL can_convert;
    if (FAILED(converter->CanConvert(orig_pixel_format, GUID_WICPixelFormat32bppRGBA, &can_convert)) || !can_convert)
        return false;

    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeErrorDiffusion, 0, 0, WICBitmapPaletteTypeCustom)))
        return false;

    if (FAILED(converter.As(&m_pUnscaledBackground)))
        return false;

    return UploadBackgroundBitmap();
}

void Renderer::ImGuiStartFrame()
{
    const auto& viz = Config::GetConfig().GetVizSettings();
    if (m_fLastUIScale != viz.fUIScale || m_sLastFont != viz.sUIFont)
        UpdateImGuiSettings();
    ImGuiBackendNewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    RenderImGuiFrame();
}

void Renderer::RenderImGuiFrame() {
    auto& io = ImGui::GetIO();
    auto& viz = Config::GetConfig().GetVizSettings();
    if (io.KeyCtrl && io.KeyAlt &&
        (ImGui::IsKeyPressed(ImGuiKey_LeftCtrl, false) || ImGui::IsKeyPressed(ImGuiKey_RightCtrl, false) ||
         ImGui::IsKeyPressed(ImGuiKey_LeftAlt, false) || ImGui::IsKeyPressed(ImGuiKey_RightAlt, false)))
        viz.bDisableUI = false;
    if (viz.bDisableUI) return;

    // While a video render runs, draw only a slim song-progress bar at the
    // top of the window. Everything else (menu bar, toolbar, dialogs, stats)
    // would end up in the captured output; the render info lives in the
    // separate Win32 progress window instead.
    if (g_bVideoRendering) {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(vp->Size.x, 4.0f), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGuiWindowFlags progFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##RenderSongProgress", nullptr, progFlags))
            ImGui::ProgressBar(m_fPlaybackPosition, ImVec2(-1, 0));
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        return;
    }

    auto& config = Config::GetConfig();
    auto& playback = config.GetPlaybackSettings();
    auto& view = config.GetViewSettings();
    auto& visual = config.GetVisualSettings();

    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Song...", "Ctrl+O"))
                PostMessage(g_hWnd, WM_COMMAND, ID_FILE_PRACTICESONG, 0);
            if (ImGui::MenuItem("Open Song with Custom Settings..."))
                PostMessage(g_hWnd, WM_COMMAND, ID_FILE_PRACTICESONGCUSTOM, 0);
            if (ImGui::MenuItem("Free Play"))
                PostMessage(g_hWnd, WM_COMMAND, ID_FILE_FREEPLAY, 0);
            if (ImGui::MenuItem("Close File", "Ctrl+W", false, playback.GetPlayMode() != GameState::Intro))
                PostMessage(g_hWnd, WM_COMMAND, ID_FILE_CLOSEFILE, 0);
            ImGui::Separator();
            if (ImGui::MenuItem("Render to Video...", nullptr, m_bShowRenderDialog, playback.GetPlayMode() != GameState::Intro))
                m_bShowRenderDialog = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Play")) {
            bool hasSong = playback.GetPlayMode() != GameState::Intro;
            if (ImGui::MenuItem("Play", nullptr, false, hasSong && playback.GetPaused()))
                PostMessage(g_hWnd, WM_COMMAND, ID_PLAY_PLAY, 0);
            if (ImGui::MenuItem("Pause", nullptr, false, hasSong && !playback.GetPaused()))
                PostMessage(g_hWnd, WM_COMMAND, ID_PLAY_PAUSE, 0);
            if (ImGui::MenuItem("Play/Pause", "Space", false, hasSong))
                PostMessage(g_hWnd, WM_COMMAND, ID_PLAY_PLAYPAUSE, 0);
            if (ImGui::MenuItem("Stop", nullptr, false, hasSong))
                PostMessage(g_hWnd, WM_COMMAND, ID_PLAY_STOP, 0);
            ImGui::Separator();
            if (ImGui::MenuItem("Skip Back", nullptr, false, hasSong))
                PostMessage(g_hWnd, WM_COMMAND, ID_PLAY_SKIPBACK, 0);
            if (ImGui::MenuItem("Skip Fwd", nullptr, false, hasSong))
                PostMessage(g_hWnd, WM_COMMAND, ID_PLAY_SKIPFWD, 0);
            ImGui::Separator();
            bool muted = playback.GetMute();
            if (ImGui::MenuItem("Mute", nullptr, &muted, hasSong))
                playback.SetMute(muted, true);
            ImGui::Separator();
            if (ImGui::MenuItem("Faster", "]"))
                PostMessage(g_hWnd, WM_COMMAND, ID_PLAY_INCREASERATE, 0);
            if (ImGui::MenuItem("Slower", "["))
                PostMessage(g_hWnd, WM_COMMAND, ID_PLAY_DECREASERATE, 0);
            if (ImGui::MenuItem("Reset Speed"))
                PostMessage(g_hWnd, WM_COMMAND, ID_PLAY_RESETRATE, 0);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            bool showControls = view.GetControls();
            if (ImGui::MenuItem("Controls", nullptr, &showControls))
                view.SetControls(showControls, true);
            bool showKeyboard = view.GetKeyboard();
            if (ImGui::MenuItem("Keyboard", nullptr, &showKeyboard))
                view.SetKeyboard(showKeyboard, true);
            bool onTop = view.GetOnTop();
            if (ImGui::MenuItem("Always On Top", nullptr, &onTop))
                view.SetOnTop(onTop, true);
            bool fullscreen = view.GetFullScreen();
            if (ImGui::MenuItem("Fullscreen", nullptr, &fullscreen))
                view.SetFullScreen(fullscreen, true);
            ImGui::Separator();
            bool zoomMove = view.GetZoomMove();
            if (ImGui::MenuItem("Move && Zoom", nullptr, &zoomMove))
                PostMessage(g_hWnd, WM_COMMAND, ID_VIEW_MOVEANDZOOM, 0);
            if (ImGui::MenuItem("Reset Move && Zoom"))
                PostMessage(g_hWnd, WM_COMMAND, ID_VIEW_RESETMOVEANDZOOM, 0);
            ImGui::Separator();
            if (ImGui::MenuItem("Set Window Size..."))
                m_bShowSetResolution = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Options")) {
            if (ImGui::MenuItem("Preferences..."))
                m_bShowPreferences = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About"))
                m_bShowAbout = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Fun")) {
            int vbo = (int)m_NotesPerPass;
            if (ImGui::Checkbox("VBO Scale: Unlimited", &m_bUnlimitedNotes)) {
                m_NotesPerPass = max(100u, min(m_NotesPerPass, MaxNotesPerPass));
            }
            ImGui::BeginDisabled(m_bUnlimitedNotes);
            ImGui::SetNextItemWidth(200);
            if (ImGui::SliderInt("VBO Scale", &vbo, 100, (int)MaxNotesPerPass, "%d", ImGuiSliderFlags_Logarithmic))
                m_NotesPerPass = (unsigned)vbo;
            ImGui::EndDisabled();
            if (ImGui::Checkbox("Ramp Over Song", &m_bCorruptorRamp)) {
                if (m_bCorruptorRamp)
                    m_fCorruption = 1.0f;
            }
            ImGui::BeginDisabled(m_bCorruptorRamp);
            int corrupt = (int)(m_fCorruption * 100.0f + 0.5f);
            ImGui::SetNextItemWidth(200);
            if (ImGui::SliderInt("Corruptor", &corrupt, 0, 100, "%d%%"))
                m_fCorruption = corrupt / 100.0f;
            ImGui::EndDisabled();
            ImGui::Text("Effective: %d%%", (int)(m_fLastCorruption * 100.0f + 0.5f));
            ImGui::Separator();
            int lag = (int)m_iLagIntensity - 1;
            if (ImGui::Combo("Lag Intensifier", &lag, "1x (No Lag)\0" "2x\0" "3x\0" "4x\0\0"))
                m_iLagIntensity = (unsigned)lag + 1;
            ImGui::Separator();
            ImGui::Checkbox("GPU Overclock Artifacts", &m_bOverclockArtifacts);
            ImGui::Separator();
            ImGui::Checkbox("Bounce stats to the beat", &viz.bBounceStats);
            if (viz.bBounceStats) {
                ImGui::Indent();
                ImGui::SetNextItemWidth(200);
                ImGui::SliderInt("Low Activity Threshold", &viz.iBounceNPSThreshold, 0, 100, "%d%% lower");
                ImGui::Unindent();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("RenderMode")) {
            if (ImGui::MenuItem("Default", nullptr, !viz.bDualPianoRoll))
                viz.bDualPianoRoll = false;
            if (ImGui::MenuItem("DualRoll", nullptr, viz.bDualPianoRoll))
                viz.bDualPianoRoll = true;
            ImGui::BeginDisabled(!viz.bDualPianoRoll);
            ImGui::Checkbox("DualRoll Keyboard", &viz.bDualRollKeyboard);
            ImGui::EndDisabled();
            ImGui::Checkbox("Tick-based Mode", &viz.bTickBased);
            ImGui::Checkbox("Visualize Pitch Bends", &viz.bVisualizePitchBends);
            ImGui::Checkbox("Image Buffer Notes", &viz.bImageBufferNotes);
            if (viz.bImageBufferNotes) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%u/64)", ImageBufferGetCachedCount());
            }
            ImGui::Separator();
            ImGui::Checkbox("Bloom", &viz.bBloom);
            if (viz.bBloom) {
                ImGui::Indent();
                float intensity = viz.fBloomIntensity;
                if (ImGui::SliderFloat("Bloom Intensity", &intensity, 0.0f, 1.0f, "%.2f"))
                    viz.fBloomIntensity = intensity;
                float brightness = viz.fBloomBrightness;
                if (ImGui::SliderFloat("Bloom Brightness", &brightness, 0.0f, 4.0f, "%.2f"))
                    viz.fBloomBrightness = brightness;
                float spread = viz.fBloomSpread;
                if (ImGui::SliderFloat("Bloom Spread", &spread, 1.0f, 30.0f, "%.1f"))
                    viz.fBloomSpread = spread;
                float saturation = viz.fBloomSaturation;
                if (ImGui::SliderFloat("Bloom Saturation", &saturation, 0.0f, 3.0f, "%.2f"))
                    viz.fBloomSaturation = saturation;
                float height = viz.fRibbonBloomHeight;
                if (ImGui::SliderFloat("Ribbon Bloom Height", &height, 0.0f, 300.0f, "%.0f"))
                    viz.fRibbonBloomHeight = height;
                float rintensity = viz.fRibbonBloomIntensity;
                if (ImGui::SliderFloat("Ribbon Bloom Intensity", &rintensity, 0.0f, 4.0f, "%.2f"))
                    viz.fRibbonBloomIntensity = rintensity;
                float rbrightness = viz.fRibbonBloomBrightness;
                if (ImGui::SliderFloat("Ribbon Bloom Brightness", &rbrightness, 0.0f, 4.0f, "%.2f"))
                    viz.fRibbonBloomBrightness = rbrightness;
                int steps = viz.iRibbonBloomSteps;
                if (ImGui::SliderInt("Ribbon Bloom Steps", &steps, 1, 100))
                    viz.iRibbonBloomSteps = steps;
                ImGui::Unindent();
            }
            ImGui::Checkbox("Vignette", &viz.bVignette);
            if (viz.bVignette) {
                ImGui::Indent();
                float vignette = viz.fVignetteIntensity;
                if (ImGui::SliderFloat("Vignette Intensity", &vignette, 0.0f, 1.0f, "%.2f"))
                    viz.fVignetteIntensity = vignette;
                float width = viz.fVignetteWidth;
                if (ImGui::SliderFloat("Vignette Width", &width, 0.0f, 3.0f, "%.2f"))
                    viz.fVignetteWidth = width;
                ImGui::Unindent();
            }
            ImGui::Checkbox("Colored Ribbon", &viz.bColoredRibbon);
            ImGui::Checkbox("Custom Ribbon Color", &viz.bRibbonCustomColor);
            if (viz.bRibbonCustomColor) {
                float rc[3] = {
                    ((viz.dwRibbonBaseColor >> 16) & 0xFF) / 255.0f,
                    ((viz.dwRibbonBaseColor >> 8) & 0xFF) / 255.0f,
                    (viz.dwRibbonBaseColor & 0xFF) / 255.0f,
                };
                if (ImGui::ColorEdit3("Ribbon Base Color", rc, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                    viz.dwRibbonBaseColor = 0xFF000000 |
                        ((DWORD)(rc[0] * 255.0f + 0.5f) << 16) |
                        ((DWORD)(rc[1] * 255.0f + 0.5f) << 8) |
                        (DWORD)(rc[2] * 255.0f + 0.5f);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    const float toolbarHeight = 35.0f;
    const float menuBarHeight = ImGui::GetFrameHeight();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    
    if (m_bShowToolbar && view.GetControls()) {
        bool showToolbar = true;
        if (view.GetFullScreen() && !visual.bAlwaysShowControls) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(g_hWndGfx, &pt);
            showToolbar = (pt.y < toolbarHeight + menuBarHeight);
            if (!showToolbar) {
                static ULONGLONG lastMove = 0;
                if (pt.x >= 0 && pt.y >= 0) {
                    lastMove = GetTickCount64();
                    showToolbar = true;
                } else if (GetTickCount64() - lastMove < 2500) {
                    showToolbar = true;
                }
            }
        }

        if (showToolbar) {
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + menuBarHeight), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGuiWindowFlags tbFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoNav;

if (ImGui::Begin("##Toolbar", &m_bShowToolbar, tbFlags)) {
                float buttonWidth = 35.0f;

                bool hasSong = playback.GetPlayMode() != GameState::Intro;
                ImGui::BeginDisabled(!hasSong);
                if (ImGui::Button(playback.GetPaused() ? "|>" : "||", ImVec2(buttonWidth, 0)))
                    PostMessage(g_hWnd, WM_COMMAND, ID_PLAY_PLAYPAUSE, 0);
                ImGui::SameLine();
                if (ImGui::Button("[]", ImVec2(buttonWidth, 0)))
                    PostMessage(g_hWnd, WM_COMMAND, ID_PLAY_STOP, 0);
                ImGui::EndDisabled();

                ImGui::SameLine(); ImGui::Dummy(ImVec2(4,0)); ImGui::SameLine();

                ImGui::BeginDisabled(!hasSong);
                if (ImGui::ArrowButton("##skipb", ImGuiDir_Left))
                    PostMessage(g_hWnd, WM_COMMAND, ID_PLAY_SKIPBACK, 0);
                ImGui::SameLine();
                if (ImGui::ArrowButton("##skipf", ImGuiDir_Right))
                    PostMessage(g_hWnd, WM_COMMAND, ID_PLAY_SKIPFWD, 0);
                ImGui::EndDisabled();

                ImGui::SameLine(); ImGui::Separator(); ImGui::SameLine();

                int volPct = (int)(playback.GetVolume() * 100.0 + 0.5);
                ImGui::SetNextItemWidth(70);
                if (ImGui::SliderInt("##vol", &volPct, 0, 100, "%d%%"))
                    playback.SetVolume(volPct / 100.0, true);

                ImGui::SameLine(); ImGui::Separator(); ImGui::SameLine();

                ImGui::BeginDisabled(!hasSong);
                static int seekPos = 0;
                if (m_fPlaybackPosition < 0.001f) seekPos = 0;
                if (!ImGui::IsItemActive()) {
                    seekPos = (int)(m_fPlaybackPosition * 1000.0f);
                }
                float sliderW = 300.0f;
                ImGui::SetNextItemWidth(sliderW);
                if (ImGui::SliderInt("##pos", &seekPos, 0, 1000, "%d", ImGuiSliderFlags_NoInput))
                    HandOffMsg(TBM_SETPOS, 0, seekPos);
                ImGui::EndDisabled();

                ImGui::SameLine(); ImGui::Separator(); ImGui::SameLine();

                int speedPct = (int)(playback.GetSpeed() * 100.0 + 0.5);
                ImGui::SetNextItemWidth(70);
                if (ImGui::SliderInt("% Spd", &speedPct, 0, 200, "%d%%"))
                    playback.SetSpeed(speedPct / 100.0, true);

                ImGui::SameLine(); ImGui::Separator(); ImGui::SameLine();

                int nspeedPct = (int)(playback.GetNSpeed() * 100.0 + 0.5);
                ImGui::SetNextItemWidth(70);
                if (ImGui::SliderInt("% Nts", &nspeedPct, 0, 200, "%d%%"))
                    playback.SetNSpeed(nspeedPct / 100.0, true);

                // Soft rounded capsule behind the centered controls: blurred
                // content clipped to a rounded rect, dark glass fill built from
                // concentric rounded layers so the feathering follows the
                // rounded corners instead of crossing them with straight cuts.
                {
                    ImDrawList* bgl = ImGui::GetBackgroundDrawList();
                    ImVec2 p0 = ImGui::GetWindowPos();
                    ImVec2 wsz = ImGui::GetWindowSize();
                    const float hpad = 14.0f;
                    const float radius = toolbarHeight * 0.5f;
                    const float feather = 16.0f;
                    const int nLayers = 8;
                    // Center the pill vertically on the actual control row.
                    ImVec2 rowMin = ImGui::GetItemRectMin();
                    ImVec2 rowMax = ImGui::GetItemRectMax();
                    float cy = (rowMin.y + rowMax.y) * 0.5f;
                    ImVec2 c0 = ImVec2(p0.x - hpad, cy - radius);
                    ImVec2 c1 = ImVec2(p0.x + wsz.x + hpad, cy + radius);
                    if (m_BlurTextureID) {
                        float bw = (float)m_iBufferWidth, bh = (float)m_iBufferHeight;
                        bgl->AddImageRounded(m_BlurTextureID, c0, c1,
                            ImVec2(c0.x / bw, c0.y / bh), ImVec2(c1.x / bw, c1.y / bh),
                            IM_COL32(255, 255, 255, 255), radius);
                    }
                    for (int l = 1; l <= nLayers; l++)
                    {
                        float inset = feather * (float)(l - 1) / (float)nLayers;
                        bgl->AddRectFilled(ImVec2(c0.x + inset, c0.y + inset),
                            ImVec2(c1.x - inset, c1.y - inset),
                            IM_COL32(0, 0, 0, 0x14), radius);
                    }
                }

                ImGui::End();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(2);
            }
        }
    }

    if (m_bShowPreferences) {
        ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        if (ImGui::Begin("Preferences", &m_bShowPreferences, ImGuiWindowFlags_NoCollapse)) {
            if (m_BlurTextureID) {
                ImVec2 wpos = ImGui::GetWindowPos();
                ImVec2 wsize = ImGui::GetWindowSize();
                float bw = (float)m_iBufferWidth, bh = (float)m_iBufferHeight;
                ImGui::GetWindowDrawList()->AddImage(m_BlurTextureID,
                    wpos, ImVec2(wpos.x + wsize.x, wpos.y + wsize.y),
                    ImVec2(wpos.x / bw, wpos.y / bh),
                    ImVec2((wpos.x + wsize.x) / bw, (wpos.y + wsize.y) / bh));
                ImGui::GetWindowDrawList()->AddRectFilled(wpos, ImVec2(wpos.x + wsize.x, wpos.y + wsize.y), 0x80000000);
            }
            if (ImGui::BeginTabBar("PrefTabs")) {
                if (ImGui::BeginTabItem("Visual")) {
                    auto& vs = config.GetVisualSettings();
                    if (vs.eKeysShown == VisualSettings::Custom)
                        vs.eKeysShown = VisualSettings::All;
                    int keysShown = (vs.eKeysShown == VisualSettings::Transition) ? 2 : (int)vs.eKeysShown;
                    if (ImGui::Combo("Keys Shown", &keysShown, "128 keys\0Song\0Transition\0"))
                        vs.eKeysShown = (keysShown == 2) ? VisualSettings::Transition : (VisualSettings::KeysShown)keysShown;
                    if (vs.eKeysShown == VisualSettings::Transition) {
                        int transSpeed = vs.eTransitionSpeed;
                        if (ImGui::Combo("Transition Speed", &transSpeed, "Smooth Slow\0Smooth Fast\0Linear Slow\0Linear Fast\0"))
                            vs.eTransitionSpeed = (VisualSettings::TransitionSpeed)transSpeed;
                    }
                    ImGui::Separator();
                    ImGui::Text("Track Colors");
                    auto unpackCol = [](unsigned col, float v[3]) {
                        v[0] = (col & 0xFF) / 255.0f;
                        v[1] = ((col >> 8) & 0xFF) / 255.0f;
                        v[2] = ((col >> 16) & 0xFF) / 255.0f;
                    };
                    auto packColor = [](const float c[3]) -> unsigned {
                        return (unsigned)(c[0] * 255.0f + 0.5f) & 0xFF |
                               ((unsigned)(c[1] * 255.0f + 0.5f) & 0xFF) << 8 |
                               ((unsigned)(c[2] * 255.0f + 0.5f) & 0xFF) << 16;
                    };
                    for (int i = 0; i < 8; i++) {
                        ImGui::PushID(i);
                        float cA[3];
                        unpackCol(vs.colors[i], cA);
                        if (ImGui::ColorEdit3("##a", cA, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
                            vs.colors[i] = packColor(cA);
                        ImGui::SameLine();
                        if (i + 8 < 16) {
                            float cB[3];
                            unpackCol(vs.colors[i + 8], cB);
                            if (ImGui::ColorEdit3("##b", cB, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
                                vs.colors[i + 8] = packColor(cB);
                            ImGui::SameLine();
                        }
                        ImGui::Text("Track %d-%d", i * 2 + 1, i * 2 + 2);
                        ImGui::PopID();
                    }
                    if (ImGui::Button("Rainbow")) {
                        int R, G, B;
                        for (int i = 0; i < 16; i++) {
                            Util::HSVtoRGB(360 * i / 16, 100, 100, R, G, B);
                            vs.colors[i] = (R & 0xFF) | ((G & 0xFF) << 8) | ((B & 0xFF) << 16);
                        }
                    }
                    ImGui::SameLine();
                    ImGui::Checkbox("Color Loop", &viz.bColorLoop);
                    ImGui::Separator();
                    if (ImGui::Button("Import config.xml...")) {
                        OPENFILENAMEA ofn = {};
                        char fn[1024] = {};
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = g_hWnd;
                        ofn.lpstrFilter = "Config Files\0*.xml\0";
                        ofn.lpstrFile = fn;
                        ofn.nMaxFile = sizeof(fn);
                        ofn.lpstrTitle = "Import settings from a config.xml file";
                        ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                        if (GetOpenFileNameA(&ofn)) {
                            TiXmlDocument doc(fn);
                            if (doc.LoadFile()) {
                                TiXmlElement* txRoot = doc.FirstChildElement();
                                if (txRoot) {
                                    config.LoadConfigValues(txRoot);
                                    config.GetVizSettings().LoadConfigValues(txRoot);
                                }
                            }
                        }
                    }
                    static unsigned uLastCols[16] = {};
                    if (memcmp(uLastCols, vs.colors, sizeof(uLastCols)) != 0) {
                        memcpy(uLastCols, vs.colors, sizeof(uLastCols));
                        HandOffMsg(WM_COMMAND, ID_UPDATE_TRACKCOLORS, 0);
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Audio")) {
                    auto& as = config.GetAudioSettings();
                    ImGui::Text("MIDI Output Device:");
                    const char* deviceName = (as.iOutDevice >= 0 && as.iOutDevice < (int)as.vMIDIOutDevices.size())
                        ? Util::WstringToString(as.vMIDIOutDevices[as.iOutDevice]) : "None";
                    if (ImGui::BeginCombo("##midiout", deviceName)) {
                        for (int i = 0; i < (int)as.vMIDIOutDevices.size(); i++) {
                            if (ImGui::Selectable(Util::WstringToString(as.vMIDIOutDevices[i]), as.iOutDevice == i))
                                as.iOutDevice = i;
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::Checkbox("KDMAPI", &viz.bKDMAPI);
                    ImGui::Separator();
                    ImGui::Checkbox("Prerendered Audio", &as.bPreRenderAudio);
                    if (as.bPreRenderAudio)
                    {
                        ImGui::Separator();
                        ImGui::TextUnformatted("Prerender Audio Settings:");
                        if (ImGui::DragInt("Voices", &as.iPreVoices, 20.0f, 32, 5000))
                            as.iPreVoices = clamp(as.iPreVoices, 32, 5000);
                        if (ImGui::DragInt("Prerender Buffer (ms)", &as.iPreBufferMs, 500.0f, 1000, 300000))
                            as.iPreBufferMs = clamp(as.iPreBufferMs, 1000, 300000);
                        if (ImGui::DragInt("Limiter Attack (ms)", &as.iPreLMAttack, 1.0f, 0, 1000))
                            as.iPreLMAttack = clamp(as.iPreLMAttack, 0, 1000);
                        if (ImGui::DragInt("Limiter Release (ms)", &as.iPreLMRelease, 1.0f, 1, 1000))
                            as.iPreLMRelease = clamp(as.iPreLMRelease, 1, 1000);
                        float fFPS = (float)as.dPreFPS;
                        if (ImGui::DragFloat("Note FPS (0 = off)", &fFPS, 1.0f, 0.0f, 1000.0f, "%.1f"))
                            as.dPreFPS = fFPS;
                        ImGui::Checkbox("Repeat last chunk on buffer underrun", &as.bPreUnderrunRepeat);
                        ImGui::Checkbox("Custom repeat chunk length (ms)", &as.bPreRepeatCustom);
                        if (ImGui::DragInt("Repeat length (ms)", &as.iPreRepeatMs, 1.0f, 10, 5000))
                            as.iPreRepeatMs = clamp(as.iPreRepeatMs, 10, 5000);
                        ImGui::Checkbox("Lock visuals to prerender audio clock (fixes sync, MAY cause note hitches)", &as.bPreStutterOnLag);
                        int iLow = as.iPreVelThreshLow;
                        if (ImGui::DragInt("Vel. threshold low", &iLow, 1.0f, 0, 126))
                            as.iPreVelThreshLow = clamp(iLow, 0, 126);
                        int iUpp = as.iPreVelThreshUpp;
                        if (ImGui::DragInt("Vel. threshold high", &iUpp, 1.0f, 1, 127))
                            as.iPreVelThreshUpp = clamp(iUpp, 1, 127);
                        ImGui::Checkbox("No FX (drum-only synth)", &as.bNoFX);
                        
                        ImGui::Separator();
                        ImGui::TextUnformatted("Soundfont Settings: (applies in next MIDI reset)");

                        WCHAR wchExe[MAX_PATH];
                        GetModuleFileNameW(NULL, wchExe, MAX_PATH);
                        std::wstring exeDir = std::wstring(wchExe);
                        size_t pos = exeDir.find_last_of(L"\\/");
                        if (pos != std::wstring::npos) exeDir = exeDir.substr(0, pos + 1);

                        std::wstring effectiveDir = as.sPreSoundfontDir;
                        if (effectiveDir.empty() && !as.sPreSoundfontPath.empty())
                        {
                            size_t lastSlash = as.sPreSoundfontPath.find_last_of(L"\\/");
                            if (lastSlash != std::wstring::npos)
                                effectiveDir = as.sPreSoundfontPath.substr(0, lastSlash);
                        }
                        if (effectiveDir.empty())
                        {
                            effectiveDir = exeDir + L"Soundfonts";
                        }

                        std::string dirStr = Util::WstringToString(effectiveDir);
                        char dirBuf[1024];
                        strncpy_s(dirBuf, dirStr.c_str(), sizeof(dirBuf));
                        if (ImGui::InputText("Soundfont Directory", dirBuf, sizeof(dirBuf)))
                        {
                            as.sPreSoundfontDir = Util::StringToWstring(dirBuf);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Browse Dir...##sfdir")) {
                            std::wstring pickedDir;
                            if (PickFolderDialog(g_hWnd, L"Select Soundfont Directory", pickedDir)) {
                                as.sPreSoundfontDir = pickedDir;
                                std::vector<std::wstring> sfs = BASSMIDI::EnumerateSoundfonts(as.sPreSoundfontDir);
                                if (!sfs.empty()) {
                                    as.sPreSoundfontPath = sfs[0];
                                    BASSMIDI::LoadSoundfont(as.sPreSoundfontPath.c_str());
                                }
                            }
                        }

                        std::vector<std::wstring> detectedSFs = BASSMIDI::EnumerateSoundfonts(effectiveDir);
                        if (detectedSFs.empty() && effectiveDir != exeDir + L"Soundfonts") {
                            std::vector<std::wstring> fallbackSFs = BASSMIDI::EnumerateSoundfonts(exeDir + L"Soundfonts");
                            if (!fallbackSFs.empty()) detectedSFs = fallbackSFs;
                        }

                        std::wstring resolvedCurr = BASSMIDI::ResolveSoundfontPath(as.sPreSoundfontPath, effectiveDir);
                        std::string currName = detectedSFs.empty() ? "No Soundfonts Found in Directory" : "Select Soundfont...";
                        for (const auto& sf : detectedSFs) {
                            if (sf == as.sPreSoundfontPath || sf == resolvedCurr) {
                                size_t lastSlash = sf.find_last_of(L"\\/");
                                currName = Util::WstringToString(lastSlash != std::wstring::npos ? sf.substr(lastSlash + 1) : sf);
                                break;
                            }
                        }

                        if (ImGui::BeginCombo("Soundfont Selector", currName.c_str())) {
                            for (const auto& sf : detectedSFs) {
                                size_t lastSlash = sf.find_last_of(L"\\/");
                                std::wstring fileName = (lastSlash != std::wstring::npos) ? sf.substr(lastSlash + 1) : sf;
                                std::string nameStr = Util::WstringToString(fileName);
                                bool isSelected = (as.sPreSoundfontPath == sf || resolvedCurr == sf);
                                if (ImGui::Selectable(nameStr.c_str(), isSelected)) {
                                    as.sPreSoundfontPath = sf;
                                    BASSMIDI::LoadSoundfont(as.sPreSoundfontPath.c_str());
                                }
                            }
                            ImGui::EndCombo();
                        }

                        if (ImGui::Button("Browse Soundfont File...##sffile")) {
                            OPENFILENAMEA ofn = {};
                            char fn[1024] = {};
                            ofn.lStructSize = sizeof(ofn);
                            ofn.hwndOwner = g_hWnd;
                            ofn.lpstrFilter = "SoundFont Files (*.sf2, *.sf3, *.sfz)\0*.sf2;*.sf3;*.sfz;*.sf2pack\0All Files (*.*)\0*.*\0";
                            ofn.lpstrFile = fn;
                            ofn.nMaxFile = sizeof(fn);
                            ofn.lpstrTitle = "Select Soundfont File";
                            ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                            if (GetOpenFileNameA(&ofn)) {
                                as.sPreSoundfontPath = Util::StringToWstring(fn);
                                size_t lastSlash = as.sPreSoundfontPath.find_last_of(L"\\/");
                                if (lastSlash != std::wstring::npos)
                                    as.sPreSoundfontDir = as.sPreSoundfontPath.substr(0, lastSlash);
                                BASSMIDI::LoadSoundfont(as.sPreSoundfontPath.c_str());
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Reset##sfreset")) {
                            as.sPreSoundfontPath = L"";
                            as.sPreSoundfontDir = L"";
                            BASSMIDI::LoadSoundfont(L"");
                        }

                        if (PRE_MIDIAudio)
                        {
                            PRE_MIDIAudio->SetMaxAheadMs(as.iPreBufferMs);
                            PRE_MIDIAudio->m_dFPS = as.dPreFPS;
                            PRE_MIDIAudio->m_dAttack = (double)as.iPreLMAttack / 1000.0;
                            PRE_MIDIAudio->m_dRelease = (double)as.iPreLMRelease / 1000.0;
                            PRE_MIDIAudio->m_iVelThreshLow = as.iPreVelThreshLow;
                            PRE_MIDIAudio->m_iVelThreshUpp = as.iPreVelThreshUpp;
                            PRE_MIDIAudio->m_bUnderrunRepeat = as.bPreUnderrunRepeat;
                            PRE_MIDIAudio->m_iRepeatFrames = as.bPreRepeatCustom ? as.iPreRepeatMs * 48 : 12000;
                            PRE_MIDIAudio->m_bExtendVisualsOnSkip = as.bPreStutterOnLag;
                            PRE_MIDIAudio->m_bDefaultNoFx = as.bNoFX;
                        }
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Video")) {
                    auto& vis = config.GetVideoSettings();
                    // Renderer selection. DirectX 12 is disabled when the machine
                    // can't do D3D12 or the session already fell back to D3D11;
                    // the selection only takes effect on the next launch.
                    const bool dx12Blocked = !g_bD3D12Available || g_bBootedFallback;
                    int renderer = dx12Blocked ? 0 : (int)vis.eRenderer;
                    ImGui::BeginDisabled(dx12Blocked);
                    if (ImGui::Combo("Renderer", &renderer, "DirectX 11\0DirectX 12\0"))
                        vis.eRenderer = (VideoSettings::Renderer)renderer;
                    ImGui::EndDisabled();
                    if (dx12Blocked)
                        ImGui::TextDisabled("DirectX 12 unavailable on this system");
                    ImGui::Checkbox("Show FPS", &vis.bShowFPS);
                    ImGui::Checkbox("Limit FPS", &vis.bLimitFPS);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Controls")) {
                    auto& cs = config.GetControlsSettings();
                    float fwdSecs = (float)cs.dFwdBackSecs;
                    if (ImGui::SliderFloat("Skip Forward/Back (sec)", &fwdSecs, 1.0f, 60.0f, "%.0f"))
                        cs.dFwdBackSecs = fwdSecs;
                    float speedPct = (float)cs.dSpeedUpPct;
                    if (ImGui::SliderFloat("Speed adjustment %", &speedPct, 1.0f, 50.0f, "%.0f"))
                        cs.dSpeedUpPct = speedPct;
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Viz")) {
                    ImGui::Checkbox("Show Markers", &viz.bShowMarkers);
                    ImGui::Combo("Marker Encoding", (int*)&viz.eMarkerEncoding, "CP-1252 (Western)\0CP-932 (Japanese)\0UTF-8\0");
                    ImGui::Checkbox("Nerd Stats", &viz.bNerdStats);
                    ImGui::Checkbox("Sys Stats", &viz.bSysStats);
                    ImGui::Checkbox("Bounce stats to the beat", &viz.bBounceStats);
                    if (viz.bBounceStats) {
                        ImGui::Indent();
                        ImGui::SetNextItemWidth(200);
                        ImGui::SliderInt("Low Activity Threshold", &viz.iBounceNPSThreshold, 0, 100, "%d%% lower");
                        ImGui::Unindent();
                    }
                    ImGui::Checkbox("Dump Frames", &viz.bDumpFrames);
                    ImGui::Checkbox("Disable UI (Reenable by pressing CTRL + ALT)", &viz.bDisableUI);
                    ImGui::Separator();
                    std::string splashStr = Util::WstringToString(viz.sSplashMIDI);
                    char splashBuf[1024];
                    strncpy_s(splashBuf, splashStr.c_str(), sizeof(splashBuf));
                    ImGui::InputText("Splash MIDI", splashBuf, sizeof(splashBuf));
                    ImGui::SameLine();
                    if (ImGui::Button("Browse##splash")) {
                        OPENFILENAMEA ofn = {};
                        char fn[1024] = {};
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = g_hWnd;
                        ofn.lpstrFilter = "MIDI Files\0*.mid;*.xz\0";
                        ofn.lpstrFile = fn;
                        ofn.nMaxFile = sizeof(fn);
                        ofn.lpstrTitle = "Select a splash MIDI!";
                        ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                        if (GetOpenFileNameA(&ofn))
                            viz.sSplashMIDI = Util::StringToWstring(fn);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Reset##splash"))
                        viz.sSplashMIDI = L"";
                    std::string bgStr = Util::WstringToString(viz.sBackground);
                    char bgBuf[1024];
                    strncpy_s(bgBuf, bgStr.c_str(), sizeof(bgBuf));
                    ImGui::InputText("Background Image", bgBuf, sizeof(bgBuf));
                    ImGui::SameLine();
                    if (ImGui::Button("Browse##bg")) {
                        OPENFILENAMEA ofn = {};
                        char fn[1024] = {};
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = g_hWnd;
                        ofn.lpstrFilter = "Image files\0*.png;*.jpg;*.jpeg\0";
                        ofn.lpstrFile = fn;
                        ofn.nMaxFile = sizeof(fn);
                        ofn.lpstrTitle = "Select a background image!";
                        ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                        if (GetOpenFileNameA(&ofn))
                            viz.sBackground = Util::StringToWstring(fn);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Reset##bg"))
                        viz.sBackground = L"";
                    ImGui::SliderFloat("Background Blur", &viz.fBGBlur, 0.0f, 50.0f, "%.1f");
                    ImGui::SliderFloat("Background Opacity", &viz.fBGOpacity, 0.0f, 1.0f, "%.2f");
                    std::string fontStr = Util::WstringToString(viz.sUIFont);
                    char fontBuf[1024];
                    strncpy_s(fontBuf, fontStr.c_str(), sizeof(fontBuf));
                    ImGui::InputText("UI Font", fontBuf, sizeof(fontBuf));
                    ImGui::SameLine();
                    if (ImGui::Button("Browse##font")) {
                        OPENFILENAMEA ofn = {};
                        char fn[1024] = {};
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = g_hWnd;
                        ofn.lpstrFilter = "Font files\0*.ttf;*.ttc;*.cff;*.otf;*.woff\0";
                        ofn.lpstrFile = fn;
                        ofn.nMaxFile = sizeof(fn);
                        ofn.lpstrTitle = "Select a font!";
                        ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                        if (GetOpenFileNameA(&ofn))
                            viz.sUIFont = Util::StringToWstring(fn);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Reset##font"))
                        viz.sUIFont = L"";
                    ImGui::SliderFloat("UI Scale", &viz.fUIScale, 0.1f, 10.0f, "%.2f");
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::End();
        }
        ImGui::PopStyleColor();
    }

    if (m_bShowRenderDialog) {
        ImGui::SetNextWindowSize(ImVec2(560, 530), ImGuiCond_FirstUseEver);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        if (ImGui::Begin("Render to Video", &m_bShowRenderDialog, ImGuiWindowFlags_NoCollapse)) {
            if (m_BlurTextureID) {
                ImVec2 wpos = ImGui::GetWindowPos();
                ImVec2 wsize = ImGui::GetWindowSize();
                float bw = (float)m_iBufferWidth, bh = (float)m_iBufferHeight;
                ImGui::GetWindowDrawList()->AddImage(m_BlurTextureID,
                    wpos, ImVec2(wpos.x + wsize.x, wpos.y + wsize.y),
                    ImVec2(wpos.x / bw, wpos.y / bh),
                    ImVec2((wpos.x + wsize.x) / bw, (wpos.y + wsize.y) / bh));
                ImGui::GetWindowDrawList()->AddRectFilled(wpos, ImVec2(wpos.x + wsize.x, wpos.y + wsize.y), 0x80000000);
            }
            const bool hasSong = VideoRenderSongLoaded();
            const bool hasFFmpeg = !viz.sFFmpegDir.empty() &&
                GetFileAttributesW((viz.sFFmpegDir + L"\\ffmpeg.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
            const bool canRender = hasSong && hasFFmpeg;

            // --- Status Header ---
            ImGui::BeginGroup();
            {
                // MIDI Status
                ImGui::Text("MIDI Source:");
                ImGui::SameLine(130);
                if (hasSong)
                    ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.45f, 1.0f), "[Ready] Song loaded in player");
                else
                    ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f), "[None] No MIDI file loaded");

                // FFmpeg Status
                ImGui::Text("FFmpeg Engine:");
                ImGui::SameLine(130);
                if (hasFFmpeg) {
                    ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.45f, 1.0f), "[Ready] FFmpeg executable detected");
                }
                else {
                    ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f), "[Missing] ffmpeg.exe not found");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Download"))
                        ShellExecuteA(g_hWnd, "open", "https://ffmpeg.org/download.html", NULL, NULL, SW_SHOWNORMAL);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Auto-Detect")) {
                        wchar_t exePath[MAX_PATH] = {};
                        if (GetModuleFileNameW(NULL, exePath, MAX_PATH)) {
                            std::wstring dir = exePath;
                            size_t sp = dir.find_last_of(L"\\/");
                            if (sp != std::wstring::npos)
                                dir = dir.substr(0, sp);
                            if (GetFileAttributesW((dir + L"\\ffmpeg.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
                                viz.sFFmpegDir = dir;
                        }
                    }
                }

                // Audio track option
                ImGui::Text("Audio Track:");
                ImGui::SameLine(130);
                ImGui::Checkbox("Include synthesized audio in export", &viz.bRenderIncludeAudio);
                ImGui::Text("Preview:");
                ImGui::SameLine(130);
                ImGui::Checkbox("Show preview window while rendering", &viz.bRenderShowPreview);
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Disable to render headless: no preview window is shown, only the progress window. Slightly faster.");
            }
            ImGui::EndGroup();

            ImGui::Separator();
            ImGui::Spacing();

            // --- Tabbed Configuration ---
            if (ImGui::BeginTabBar("##RenderTabBar", ImGuiTabBarFlags_None)) {
                // TAB 1: Video & Resolution
                if (ImGui::BeginTabItem("Video & Resolution")) {
                    ImGui::Spacing();

                    // Resolution Presets
                    ImGui::Text("Resolution Preset:");
                    static int resPreset = 0;
                    if (viz.iRenderWidth == 1920 && viz.iRenderHeight == 1080) resPreset = 0;
                    else if (viz.iRenderWidth == 2560 && viz.iRenderHeight == 1440) resPreset = 1;
                    else if (viz.iRenderWidth == 3840 && viz.iRenderHeight == 2160) resPreset = 2;
                    else if (viz.iRenderWidth == 1280 && viz.iRenderHeight == 720) resPreset = 3;
                    else if (viz.iRenderWidth == m_iBufferWidth && viz.iRenderHeight == m_iBufferHeight) resPreset = 4;
                    else resPreset = 5;

                    const char* resPresetNames[] = {
                        "1080p Full HD (1920 x 1080)",
                        "1440p QHD (2560 x 1440)",
                        "4K UHD (3840 x 2160)",
                        "720p HD (1280 x 720)",
                        "Match Window Size",
                        "Custom Resolution"
                    };

                    if (ImGui::Combo("##resPresetCombo", &resPreset, resPresetNames, IM_ARRAYSIZE(resPresetNames))) {
                        if (resPreset == 0) { viz.iRenderWidth = 1920; viz.iRenderHeight = 1080; }
                        else if (resPreset == 1) { viz.iRenderWidth = 2560; viz.iRenderHeight = 1440; }
                        else if (resPreset == 2) { viz.iRenderWidth = 3840; viz.iRenderHeight = 2160; }
                        else if (resPreset == 3) { viz.iRenderWidth = 1280; viz.iRenderHeight = 720; }
                        else if (resPreset == 4) { viz.iRenderWidth = max(128, m_iBufferWidth); viz.iRenderHeight = max(128, m_iBufferHeight); }
                    }

                    ImGui::Spacing();
                    ImGui::Text("Dimensions (Pixels):");
                    ImGui::SetNextItemWidth(120);
                    ImGui::InputInt("##renderWidth", &viz.iRenderWidth, 0, 0);
                    viz.iRenderWidth = max(128, min(viz.iRenderWidth, 8192));
                    ImGui::SameLine();
                    ImGui::Text("x");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(120);
                    ImGui::InputInt("##renderHeight", &viz.iRenderHeight, 0, 0);
                    viz.iRenderHeight = max(128, min(viz.iRenderHeight, 8192));

                    // Aspect ratio indicator
                    ImGui::SameLine();
                    float aspect = (float)viz.iRenderWidth / (float)max(1, viz.iRenderHeight);
                    if (fabsf(aspect - 16.0f / 9.0f) < 0.01f)
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[16:9]");
                    else if (fabsf(aspect - 16.0f / 10.0f) < 0.01f)
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[16:10]");
                    else if (fabsf(aspect - 4.0f / 3.0f) < 0.01f)
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[4:3]");
                    else if (fabsf(aspect - 21.0f / 9.0f) < 0.02f)
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[21:9]");
                    else
                        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[%.2f:1]", aspect);

                    ImGui::Separator();
                    ImGui::Spacing();

                    // Framerate
                    ImGui::Text("Target Framerate:");
                    ImGui::SetNextItemWidth(120);
                    ImGui::InputInt("FPS##renderFps", &viz.iRenderFPS, 0, 0);
                    viz.iRenderFPS = max(1, min(viz.iRenderFPS, 1000));
                    ImGui::SameLine();
                    ImGui::Text("Quick:");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("60"))  viz.iRenderFPS = 60;
                    ImGui::SameLine();
                    if (ImGui::SmallButton("120")) viz.iRenderFPS = 120;
                    ImGui::SameLine();
                    if (ImGui::SmallButton("144")) viz.iRenderFPS = 144;
                    ImGui::SameLine();
                    if (ImGui::SmallButton("240")) viz.iRenderFPS = 240;

                    ImGui::Spacing();
                    ImGui::TextDisabled("Playback runs uncapped on GPU; video is encoded at this target rate.");

                    ImGui::EndTabItem();
                }

                // TAB 2: Encoder & Quality
                if (ImGui::BeginTabItem("Encoder & Quality")) {
                    ImGui::Spacing();

                    // Container format
                    ImGui::Text("Container Format:");
                    const char* formatNames[] = { "MP4 (.mp4 - Standard, recommended)", "MOV (.mov - Apple QuickTime)", "AVI (.avi - Audio Video Interleave)" };
                    ImGui::Combo("##formatCombo", &viz.iRenderFormat, formatNames, IM_ARRAYSIZE(formatNames));
                    viz.iRenderFormat = max(0, min(viz.iRenderFormat, 2));

                    // Codec
                    ImGui::Spacing();
                    ImGui::Text("Video Codec:");
                    const char* codecNames[] = { "H.264 / AVC (libx264 - Fast, maximum compatibility)", "H.265 / HEVC (libx265 - Better compression, higher CPU load)" };
                    ImGui::Combo("##codecCombo", &viz.iRenderCodec, codecNames, IM_ARRAYSIZE(codecNames));
                    viz.iRenderCodec = max(0, min(viz.iRenderCodec, 1));

                    // Encoding preset
                    ImGui::Spacing();
                    ImGui::Text("Encoding Speed Preset:");
                    static const char* sPresetNames[] = {
                        "ultrafast (Fastest render, larger file)",
                        "superfast",
                        "veryfast",
                        "faster",
                        "fast",
                        "medium (Default balance)",
                        "slow",
                        "slower",
                        "veryslow (Slowest render, optimal quality)",
                        "placebo"
                    };
                    viz.iRenderPreset = max(0, min(viz.iRenderPreset, 9));
                    ImGui::Combo("##presetCombo", &viz.iRenderPreset, sPresetNames, IM_ARRAYSIZE(sPresetNames));

                    ImGui::Separator();
                    ImGui::Spacing();

                    // Rate Control
                    ImGui::Text("Rate Control Mode:");
                    if (ImGui::RadioButton("Constant Quality (CRF)", viz.iRenderBitrateMode == 1))
                        viz.iRenderBitrateMode = 1;
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Target Bitrate (CBR)", viz.iRenderBitrateMode == 0))
                        viz.iRenderBitrateMode = 0;

                    ImGui::Spacing();
                    if (viz.iRenderBitrateMode == 1) {
                        ImGui::SliderInt("CRF Factor", &viz.iRenderCRF, 0, 51);
                        viz.iRenderCRF = max(0, min(viz.iRenderCRF, 51));
                        if (viz.iRenderCRF <= 16)
                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.9f, 1.0f), "Mode: Near-lossless / Large file size");
                        else if (viz.iRenderCRF <= 23)
                            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Mode: High quality / Visually lossless (Recommended: 18)");
                        else
                            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.4f, 1.0f), "Mode: Higher compression / Compact file size");
                    }
                    else {
                        ImGui::InputInt("Bitrate (Kbps)", &viz.iRenderBitrateKbps, 500, 2000);
                        if (viz.iRenderBitrateKbps < 100)
                            viz.iRenderBitrateKbps = 100;
                        ImGui::SameLine();
                        ImGui::Text("Quick:");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("8M"))  viz.iRenderBitrateKbps = 8000;
                        ImGui::SameLine();
                        if (ImGui::SmallButton("16M")) viz.iRenderBitrateKbps = 16000;
                        ImGui::SameLine();
                        if (ImGui::SmallButton("30M")) viz.iRenderBitrateKbps = 30000;
                        ImGui::SameLine();
                        if (ImGui::SmallButton("60M")) viz.iRenderBitrateKbps = 60000;
                    }

                    ImGui::EndTabItem();
                }

                // TAB 3: Output & Advanced
                if (ImGui::BeginTabItem("Output & Advanced")) {
                    ImGui::Spacing();

                    const char* sExt = viz.iRenderFormat == 1 ? "mov" : (viz.iRenderFormat == 2 ? "avi" : "mp4");
                    static char sOut[MAX_PATH] = {};
                    static bool sOutInit = false;
                    if (!sOutInit) {
                        std::string s = Util::WstringToString(viz.sRenderOutputPath);
                        strncpy_s(sOut, s.c_str(), sizeof(sOut));
                        sOutInit = true;
                    }

                    ImGui::Text("Output File Path:");
                    ImGui::SetNextItemWidth(max(100.0f, ImGui::GetContentRegionAvail().x - 85.0f));
                    if (ImGui::InputText("##outpath", sOut, sizeof(sOut)))
                        viz.sRenderOutputPath = Util::StringToWstring(sOut);
                    ImGui::SameLine();
                    if (ImGui::Button("Browse##out", ImVec2(75, 0))) {
                        OPENFILENAMEA ofn = {};
                        char fn[1024] = {};
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = g_hWnd;
                        std::string filter = "Video files (*.";
                        filter += sExt;
                        filter += ")";
                        filter += '\0';
                        filter += "*.";
                        filter += sExt;
                        filter += '\0';
                        filter += "All Files (*.*)";
                        filter += '\0';
                        filter += "*.*";
                        filter += '\0';
                        filter += '\0';
                        ofn.lpstrFilter = filter.c_str();
                        ofn.lpstrFile = fn;
                        ofn.nMaxFile = sizeof(fn);
                        ofn.lpstrDefExt = sExt;
                        ofn.lpstrTitle = "Save render video as...";
                        ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT;
                        if (GetSaveFileNameA(&ofn)) {
                            viz.sRenderOutputPath = Util::StringToWstring(fn);
                            strncpy_s(sOut, fn, sizeof(sOut));
                        }
                    }

                    if (viz.sRenderOutputPath.empty())
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Default: Video will be saved into the application folder.");

                    ImGui::Separator();
                    ImGui::Spacing();

                    // FFmpeg Folder Path
                    static char sFfDir[MAX_PATH] = {};
                    static bool sFfDirInit = false;
                    if (!sFfDirInit) {
                        std::string s = Util::WstringToString(viz.sFFmpegDir);
                        strncpy_s(sFfDir, s.c_str(), sizeof(sFfDir));
                        sFfDirInit = true;
                    }

                    ImGui::Text("FFmpeg Binary Location:");
                    ImGui::SetNextItemWidth(max(100.0f, ImGui::GetContentRegionAvail().x - 85.0f));
                    if (ImGui::InputText("##ffdir", sFfDir, sizeof(sFfDir)))
                        viz.sFFmpegDir = Util::StringToWstring(sFfDir);
                    ImGui::SameLine();
                    if (ImGui::Button("Browse##ffdir", ImVec2(75, 0))) {
                        OPENFILENAMEA ofn = {};
                        char fn[1024] = {};
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = g_hWnd;
                        ofn.lpstrFilter = "Executables\0*.exe\0";
                        ofn.lpstrFile = fn;
                        ofn.nMaxFile = sizeof(fn);
                        ofn.lpstrTitle = "Select ffmpeg.exe";
                        ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                        if (GetOpenFileNameA(&ofn)) {
                            std::string s = fn;
                            size_t sp = s.find_last_of("\\/");
                            if (sp != std::string::npos)
                                s = s.substr(0, sp);
                            viz.sFFmpegDir = Util::StringToWstring(s);
                            strncpy_s(sFfDir, s.c_str(), sizeof(sFfDir));
                        }
                    }

                    ImGui::Separator();
                    ImGui::Spacing();

                    // Advanced Options
                    ImGui::Checkbox("Enable custom FFmpeg arguments (advanced)", &viz.bRenderAdvanced);
                    if (viz.bRenderAdvanced) {
                        static char sAdv[4096] = {};
                        static bool sAdvInit = false;
                        if (!sAdvInit) {
                            std::string s = Util::WstringToString(viz.sRenderAdvancedOptions);
                            strncpy_s(sAdv, s.c_str(), sizeof(sAdv));
                            sAdvInit = true;
                        }
                        ImGui::TextDisabled("Additional flags injected before video output:");
                        if (ImGui::InputTextMultiline("##advOptions", sAdv, sizeof(sAdv), ImVec2(-1, 60)))
                            viz.sRenderAdvancedOptions = Util::StringToWstring(sAdv);
                    }

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // --- Footer Area ---
            const char* sExtLabel = viz.iRenderFormat == 1 ? "MOV" : (viz.iRenderFormat == 2 ? "AVI" : "MP4");
            const char* sCodecLabel = viz.iRenderCodec == 1 ? "H.265" : "H.264";
            if (viz.iRenderBitrateMode == 1) {
                ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                    "Profile: %dx%d @ %d FPS | %s (%s, CRF %d)",
                    viz.iRenderWidth, viz.iRenderHeight, viz.iRenderFPS, sExtLabel, sCodecLabel, viz.iRenderCRF);
            }
            else {
                ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                    "Profile: %dx%d @ %d FPS | %s (%s, %d Kbps)",
                    viz.iRenderWidth, viz.iRenderHeight, viz.iRenderFPS, sExtLabel, sCodecLabel, viz.iRenderBitrateKbps);
            }

            ImGui::Spacing();

            if (g_bVideoRendering) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.3f, 0.3f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.15f, 0.15f, 1.0f));
                if (ImGui::Button("Stop Render", ImVec2(-FLT_MIN, 36)))
                    StopVideoRender();
                ImGui::PopStyleColor(3);
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Rendering in progress... (check main window title)");
            }
            else {
                ImGui::BeginDisabled(!canRender);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.52f, 0.28f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.65f, 0.35f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.42f, 0.22f, 1.0f));
                if (ImGui::Button("Start Render", ImVec2(-FLT_MIN, 36)))
                    RequestVideoRender();
                ImGui::PopStyleColor(3);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Starts video rendering. Renders as fast as possible on GPU until the song ends.");
                ImGui::EndDisabled();

                if (!canRender) {
                    if (!hasSong)
                        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "* Load a MIDI file to render (File -> Open Song...)");
                    if (!hasFFmpeg)
                        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "* Configure FFmpeg directory in Output & Advanced tab");
                }
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }

    if (m_bShowAbout) {
        ImGui::SetNextWindowSize(ImVec2(400, 250), ImGuiCond_FirstUseEver);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        if (ImGui::Begin("About PlayGroundFromAbove", &m_bShowAbout, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
            if (m_BlurTextureID) {
                ImVec2 wpos = ImGui::GetWindowPos();
                ImVec2 wsize = ImGui::GetWindowSize();
                float bw = (float)m_iBufferWidth, bh = (float)m_iBufferHeight;
                ImGui::GetWindowDrawList()->AddImage(m_BlurTextureID,
                    wpos, ImVec2(wpos.x + wsize.x, wpos.y + wsize.y),
                    ImVec2(wpos.x / bw, wpos.y / bh),
                    ImVec2((wpos.x + wsize.x) / bw, (wpos.y + wsize.y) / bh));
                ImGui::GetWindowDrawList()->AddRectFilled(wpos, ImVec2(wpos.x + wsize.x, wpos.y + wsize.y), 0x80000000);
            }
            ImGui::TextWrapped("PlayGroundFromAbove");
            ImGui::TextWrapped("Build: " __DATE__);
            ImGui::Separator();
            ImGui::TextWrapped("A Frankenstein fork of PFAviz, containing fixes and new additions.");
            ImGui::TextWrapped("PFA by Brain Pantano, PFAviz by khang06, PGFA by mappazinho");
            ImGui::Separator();
            if (ImGui::Button("OK", ImVec2(80, 0)))
                m_bShowAbout = false;
            ImGui::End();
        }
        ImGui::PopStyleColor();
    }

    if (m_bShowSetResolution) {
        ImGui::SetNextWindowSize(ImVec2(250, 120), ImGuiCond_FirstUseEver);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
        if (ImGui::Begin("Set Window Size", &m_bShowSetResolution, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
            if (m_BlurTextureID) {
                ImVec2 wpos = ImGui::GetWindowPos();
                ImVec2 wsize = ImGui::GetWindowSize();
                float bw = (float)m_iBufferWidth, bh = (float)m_iBufferHeight;
                ImGui::GetWindowDrawList()->AddImage(m_BlurTextureID,
                    wpos, ImVec2(wpos.x + wsize.x, wpos.y + wsize.y),
                    ImVec2(wpos.x / bw, wpos.y / bh),
                    ImVec2((wpos.x + wsize.x) / bw, (wpos.y + wsize.y) / bh));
                ImGui::GetWindowDrawList()->AddRectFilled(wpos, ImVec2(wpos.x + wsize.x, wpos.y + wsize.y), 0x80000000);
            }
            ImGui::InputInt("Width", &m_resWidth);
            ImGui::InputInt("Height", &m_resHeight);
            if (ImGui::Button("OK", ImVec2(80, 0))) {
                RECT adjusted = {0, 0, max(MINWIDTH, min(m_resWidth, 65535)), max(MINHEIGHT, min(m_resHeight, 65535))};
                if (view.GetControls())
                    adjusted.bottom += (int)toolbarHeight;
                AdjustWindowRectEx(&adjusted, GetWindowLongPtrA(g_hWnd, GWL_STYLE), TRUE, GetWindowLongPtrA(g_hWnd, GWL_EXSTYLE));
                SetWindowPos(g_hWnd, NULL, 0, 0, adjusted.right - adjusted.left, adjusted.bottom - adjusted.top, SWP_NOMOVE);
                g_bResetPending = true;
                PostMessage(g_hWnd, WM_COMMAND, ID_VIEW_RESETDEVICE, 0);
                m_bShowSetResolution = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(80, 0)))
                m_bShowSetResolution = false;
            ImGui::End();
        }
        ImGui::PopStyleColor();
    }

    if (g_bShowLoading) {
        ImGui::SetNextWindowSize(ImVec2(400, 120), ImGuiCond_FirstUseEver);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
        ImGui::OpenPopup("Loading Song");
        if (ImGui::BeginPopupModal("Loading Song", &g_bShowLoading, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (m_BlurTextureID) {
                ImVec2 wpos = ImGui::GetWindowPos();
                ImVec2 wsize = ImGui::GetWindowSize();
                float bw = (float)m_iBufferWidth, bh = (float)m_iBufferHeight;
                ImGui::GetWindowDrawList()->AddImage(m_BlurTextureID,
                    wpos, ImVec2(wpos.x + wsize.x, wpos.y + wsize.y),
                    ImVec2(wpos.x / bw, wpos.y / bh),
                    ImVec2((wpos.x + wsize.x) / bw, (wpos.y + wsize.y) / bh));
                ImGui::GetWindowDrawList()->AddRectFilled(wpos, ImVec2(wpos.x + wsize.x, wpos.y + wsize.y), 0x80000000);
            }
            const char* desc = "placeholder";
            switch (g_LoadingProgress.stage) {
            case MIDILoadingProgress::Stage::CopyToMem: desc = "Copying MIDI into memory..."; break;
            case MIDILoadingProgress::Stage::Decompress: desc = "Decompressing..."; break;
            case MIDILoadingProgress::Stage::ParseTracks: desc = "Parsing tracks..."; break;
            case MIDILoadingProgress::Stage::ConnectNotes: desc = "Connecting notes..."; break;
            case MIDILoadingProgress::Stage::SortEvents: desc = "Sorting events..."; break;
            case MIDILoadingProgress::Stage::Finalize: desc = "Finalizing..."; break;
            case MIDILoadingProgress::Stage::Done: g_bShowLoading = false; break;
            }
            ImGui::Text("%s", desc);
            auto progLoaded = g_LoadingProgress.progress.load();
            auto progMax = g_LoadingProgress.max;
            float prog = progMax > 0 ? (float)progLoaded / (float)progMax : 0.0f;
            ImGui::ProgressBar(prog, ImVec2(350, 0));
            char buf[128];
            {
                static double s_fStageStart = 0.0;
                static MIDILoadingProgress::Stage s_eLastStage = (MIDILoadingProgress::Stage)(-1);
                static uint64_t s_iLastProgress = 0;
                const MIDILoadingProgress::Stage eStage = g_LoadingProgress.stage;
                const double fNow = ImGui::GetTime();
                if ( eStage != s_eLastStage || progLoaded < s_iLastProgress )
                    s_fStageStart = fNow;
                s_eLastStage = eStage;
                s_iLastProgress = progLoaded;
                double fETA = 0.0;
                const double fElapsed = fNow - s_fStageStart;
                if ( progMax > 0 && fElapsed > 0.5 && progLoaded > 0 && progLoaded < progMax )
                {
                    const double fRate = ( double )progLoaded / fElapsed;
                    fETA = ( double )( progMax - progLoaded ) / fRate;
                }
                if ( fETA > 0.0 )
                {
                    if ( fETA < 60.0 )
                        snprintf( buf, sizeof( buf ), "%llu / %llu - ETA %.0fs", progLoaded, progMax, fETA );
                    else
                        snprintf( buf, sizeof( buf ), "%llu / %llu - ETA %.1f min", progLoaded, progMax, fETA / 60.0 );
                }
                else
                    snprintf( buf, sizeof( buf ), "%llu / %llu", progLoaded, progMax );
            }
            ImGui::Text("%s", buf);
            PROCESS_MEMORY_COUNTERS mem{};
            mem.cb = sizeof(mem);
            GetProcessMemoryInfo(GetCurrentProcess(), &mem, sizeof(mem));
            snprintf(buf, sizeof(buf), "%llu MB used", mem.PagefileUsage / 1048576);
            ImGui::Text("%s", buf);
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor();
    }
}

void Renderer::UpdateImGuiSettings() {
    const auto& viz = Config::GetConfig().GetVizSettings();
    float scale = viz.fUIScale;
    m_fLastUIScale = scale;
    m_sLastFont = viz.sUIFont;

    char fonts_dir[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_FONTS, NULL, SHGFP_TYPE_CURRENT, fonts_dir)))
        strcpy_s(fonts_dir, "C:\\Windows\\Fonts");

    auto& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig font_config = {};
    if (!viz.sUIFont.empty()) {
        io.Fonts->AddFontFromFileTTF(Util::WstringToString(viz.sUIFont), 14.0f * scale, &font_config, io.Fonts->GetGlyphRangesDefault());
    } else {
        font_config.FontNo = 0; // TAHOMA!
        io.Fonts->AddFontFromFileTTF((std::string(fonts_dir) + "\\Tahoma.ttf").c_str(), 14.0f * scale, &font_config, io.Fonts->GetGlyphRangesDefault());
        font_config.FontNo = 1; // MS UI Gothic
        font_config.MergeMode = true;
        io.Fonts->AddFontFromFileTTF((std::string(fonts_dir) + "\\msgothic.ttc").c_str(), 14.0f * scale, &font_config, io.Fonts->GetGlyphRangesJapanese());
    }
    io.IniFilename = nullptr;
    if (!io.Fonts->Build()) {
        io.Fonts->Clear();
        io.Fonts->AddFontDefault();
    }
    ImGuiBackendCreateDeviceObjects();

    auto& style = ImGui::GetStyle();
    style.WindowMinSize = ImVec2(1, 1);
    style.WindowBorderSize = 0.0f;
    style.WindowPadding = ImVec2(6 * scale, 6 * scale);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.50f);
}

void Renderer::DrawPianoRollStripKeyboard() {
    if (!Config::GetConfig().GetVizSettings().bDualPianoRoll || !Config::GetConfig().GetVizSettings().bDualRollKeyboard)
        return;

    const float menuBarHeight = 20.0f;
    const float toolbarHeight = 35.0f;
    const float stripTop = menuBarHeight + toolbarHeight;
    const float stripH = max(190.0f, min((float)m_iBufferHeight * 0.45f, (float)m_iBufferHeight * 0.28f));

    const int nKeys = 128;
    static const int sSharpsBelow[12] = { 0, 0, 1, 1, 2, 2, 2, 3, 3, 4, 4, 5 };
    const int nWhiteKeys = nKeys - (nKeys / 12) * 5 - sSharpsBelow[nKeys % 12];
    const float whiteH = stripH / (float)nWhiteKeys;

    const float kbWidth = max(20.0f, min(60.0f, stripH * 0.10f));
    float keyGap = max(1.0f, floor(kbWidth * 0.05f + 0.5f));

    ImDrawList* dl = m_pDrawList;

    auto toImCol = [](DWORD c) -> ImU32 {
        int r = (c >> 16) & 0xFF;
        int g = (c >> 8) & 0xFF;
        int b = c & 0xFF;
        return IM_COL32(r, g, b, 255);
    };

    ImU32 bgPrim = toImCol(m_stripKBBackground);
    ImU32 bgDark = toImCol(m_stripKBBackgroundDark);
    ImU32 wDark = toImCol(m_stripKBWhiteDark);
    ImU32 wVeryDark = toImCol(m_stripKBWhiteVeryDark);
    ImU32 sPrim = toImCol(m_stripKBSharp);
    ImU32 sDark = toImCol(m_stripKBSharpDark);
    ImU32 sVeryDark = toImCol(m_stripKBSharpVeryDark);

    float gapHalf = floor(keyGap / 2.0f + 0.5f);

    float ribbonW = max(2.0f, floor(kbWidth * 0.08f + 0.5f));
    float ribbonX = kbWidth + 1.0f;

    dl->AddRectFilled(ImVec2(ribbonX, stripTop), ImVec2(ribbonX + ribbonW, stripTop + stripH), bgDark);

    dl->AddRectFilled(ImVec2(0, stripTop), ImVec2(kbWidth, stripTop + stripH), bgPrim);

    for (int note = 0; note < nKeys; note++) {
        int r = note % 12;
        int octave = note / 12;
        if (((1 << r) & 0x54A) != 0) continue;
        int whitesBelow = note - octave * 5 - sSharpsBelow[r];
        float y = stripTop + whiteH * whitesBelow;
        float kw = kbWidth - keyGap;
        float kh = whiteH - keyGap;
        float kx = gapHalf;
        float ky = y + gapHalf;

        bool pressed = m_stripPressedKeys[note] != 0;
        if (pressed) {
            ImU32 pColor = toImCol(m_stripPressedKeys[note]);
            dl->AddRectFilled(ImVec2(kx, ky), ImVec2(kx + kw, ky + kh), pColor);
            ImU32 rColor = toImCol(m_stripRibbonColors[note]);
            dl->AddRectFilled(ImVec2(ribbonX, ky), ImVec2(ribbonX + ribbonW, ky + kh), rColor);
        } else {
            dl->AddRectFilled(ImVec2(kx, ky), ImVec2(kx + kw, ky + kh * 0.55f), wDark);
            dl->AddRectFilled(ImVec2(kx, ky + kh * 0.55f), ImVec2(kx + kw, ky + kh * 0.55f + 2.0f), bgDark);
            dl->AddRectFilled(ImVec2(kx, ky + kh * 0.55f + 2.0f), ImVec2(kx + kw, ky + kh), wVeryDark);
        }
    }

    for (int note = 0; note < nKeys; note++) {
        int r = note % 12;
        int octave = note / 12;
        if (((1 << r) & 0x54A) == 0) continue;
        int whitesBelow = note - octave * 5 - sSharpsBelow[r];
        float boundary = stripTop + whiteH * whitesBelow;
        float nudge = 0.0f;
        if (r == 1 || r == 6) nudge = -whiteH * 0.05f;
        else if (r == 3 || r == 10) nudge = whiteH * 0.05f;
        float sH = whiteH - keyGap;
        float sy = (boundary - whiteH) + gapHalf + nudge;
        float sw = kbWidth * 0.6f;
        float sx = (kbWidth - sw) / 2.0f;
        float sTopH = sH * 0.3f;

        bool pressed = m_stripPressedKeys[note] != 0;
        if (pressed) {
            ImU32 pColor = toImCol(m_stripPressedKeys[note]);
            dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + sw, sy + sH), pColor);
            ImU32 rColor = toImCol(m_stripRibbonColors[note]);
            dl->AddRectFilled(ImVec2(ribbonX, sy), ImVec2(ribbonX + ribbonW, sy + sH), rColor);
        } else {
            dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + sw, sy + sTopH), sDark);
            dl->AddRectFilled(ImVec2(sx, sy + sTopH), ImVec2(sx + sw, sy + sTopH + 1.0f), sPrim);
            dl->AddRectFilled(ImVec2(sx, sy + sTopH + 1.0f), ImVec2(sx + sw, sy + sH), sVeryDark);
        }
    }
}