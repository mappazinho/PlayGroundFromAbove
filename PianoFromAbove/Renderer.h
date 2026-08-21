/*************************************************************************************************
*
> File: Renderer.h
*
> Description: Defines rendering objects. Only one for now.
*
> Copyright (c) 2010 Brian Pantano. All rights reserved.
*
*************************************************************************************************/
#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <tuple>
#include <limits>
#include <wincodec.h>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"

using Microsoft::WRL::ComPtr;

enum class Pipeline {
    Rect,
    Note,
    Background,
};

struct RectVertex {
    float x;
    float y;
    DWORD color;
};

struct NoteData {
    uint8_t key;
    uint8_t channel;
    uint16_t track;
    float pos;
    float length;
};

struct TrackColor {
    DWORD primary;
    DWORD dark;
    DWORD darker;
};

struct RootConstants {
    float proj[4][4];
    float deflate;
    float notes_y;
    float notes_cy;
    float white_cx;
    float timespan;
    float stripMode;
    float stripTimeSpan;
    float fWarp;
    float fWarpTime;
    float fWarpSeedX;
    float fWarpSeedY;
    float notes_x;
    float notes_cx;
    float fMT;
    float fMTTilt;
};
static_assert(sizeof(RootConstants) % 4 == 0);

struct BackgroundConstants {
    float fadeStart;
    float fadeEnd;
    float fadeEnabled;
    float padding;
};
static_assert(sizeof(BackgroundConstants) % 4 == 0);

constexpr unsigned MaxTrackColors = 65536;
struct FixedSizeConstants {
    float note_x[128];
    float bends[16];
};

// Image buffer cache key: everything that changes the baked chunk content.
// fWarpTime is deliberately excluded (it advances every frame; warp disables
// the image buffer entirely).
struct ChunkCacheKey {
    int width = 0;
    int height = 0;
    float deflate = 0, notes_y = 0, notes_cy = 0, white_cx = 0, timespan = 0;
    float stripTimeSpan = 0;
    float fWarp = 0, fWarpSeedX = 0, fWarpSeedY = 0;
    float notes_x = 0, notes_cx = 0, fMT = 0, fMTTilt = 0;
    float corruption = 0;
    unsigned long long eventCount = 0;
    unsigned fixedStamp = 0;
    unsigned trackColorStamp = 0;
    bool operator==(const ChunkCacheKey& o) const {
        return width == o.width && height == o.height &&
            deflate == o.deflate && notes_y == o.notes_y && notes_cy == o.notes_cy &&
            white_cx == o.white_cx && timespan == o.timespan &&
            stripTimeSpan == o.stripTimeSpan && fWarp == o.fWarp &&
            fWarpSeedX == o.fWarpSeedX && fWarpSeedY == o.fWarpSeedY &&
            notes_x == o.notes_x && notes_cx == o.notes_cx &&
            fMT == o.fMT && fMTTilt == o.fMTTilt &&
            corruption == o.corruption && eventCount == o.eventCount &&
            fixedStamp == o.fixedStamp && trackColorStamp == o.trackColorStamp;
    }
};

class D3D11Renderer;

// Base renderer: API-agnostic state, batching, and UI. GPU work happens in
// D3D12Renderer (d3d12) / D3D11Renderer (d3d11) via the virtual hooks below.
class Renderer {
public:
    enum FontSize { Small, SmallBold, SmallComic, Medium, Large };

    virtual ~Renderer() {}

    // Picks the backend: D3D12 when the user requested it and this machine can
    // create a D3D12 device (g_bD3D12Available probe, no boot-time fallback),
    // otherwise D3D11.
    static Renderer* CreateInstance();

    // Human-readable name of the active backend, e.g. "DirectX 12".
    virtual const wchar_t* GetModeName() const { return L"Unknown"; }

    // --- GPU-backend surface -------------------------------------------------
    virtual std::tuple<HRESULT, const char*> Init(HWND hWnd, bool bLimitFPS) = 0;
    virtual HRESULT ResetDevice() = 0;
    virtual HRESULT ClearAndBeginScene(DWORD color) = 0;
    virtual HRESULT EndScene(bool draw_bg = false) = 0;
    virtual HRESULT PresentBackend() = 0;
    virtual HRESULT WaitForGPU() = 0;
    virtual std::wstring GetAdapterName() = 0;
    virtual bool GetAdapterVideoMemory(DWORDLONG& used, DWORDLONG& total) = 0;
    virtual void SetPipeline(Pipeline pipeline) = 0;
    virtual char* Screenshot() = 0;
    virtual void SetBackgroundBlur(float sigma) = 0;
    virtual void ApplyBlur() = 0;
    virtual void DrawPianoRollStripBackground() = 0;
    virtual void DrawPianoRollStrip() = 0;

protected:
    virtual std::tuple<HRESULT, const char*> CreateWindowDependentObjects(HWND hWnd) = 0;
    virtual void SetupCommandList() = 0;
    virtual bool UploadBackgroundBitmap() = 0;
    virtual HRESULT CreateBlurResources() = 0;
    virtual void DestroyBlurResources() = 0;
    virtual void ReleaseDeviceResources() = 0;
    virtual void ImGuiBackendNewFrame() = 0;
    virtual void ImGuiBackendShutdown() = 0;
    virtual void ImGuiBackendCreateDeviceObjects() = 0;
    void UpdateImGuiSettings(); // shared font/style rebuild; ends in the backend hook

public:
    // --- Shared (API-agnostic) implementation ---------------------------------
    HRESULT ResetDeviceIfNeeded();
    HRESULT RecoverDevice(HWND hWnd, bool bLimitFPS);
    HRESULT Present(); // PresentPrelude (throttle/minimize) + PresentBackend()
    bool DeviceLost() const { return m_bDeviceLost; }
    int GetBufferWidth() const { return m_iBufferWidth; }
    int GetBufferHeight() const { return m_iBufferHeight; }

    HRESULT BeginText();
    HRESULT DrawTextW( const WCHAR *sText, FontSize fsFont, LPRECT rcPos, DWORD dwFormat, DWORD dwColor, INT iChars = -1 );
    HRESULT DrawTextA( const CHAR *sText, FontSize fsFont, LPRECT rcPos, DWORD dwFormat, DWORD dwColor, INT iChars = -1 );
    HRESULT EndText();
    HRESULT DrawRect( float x, float y, float cx, float cy, DWORD color );
    HRESULT DrawRect( float x, float y, float cx, float cy,
                      DWORD c1, DWORD c2, DWORD c3, DWORD c4 );
    HRESULT DrawSkew( float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, DWORD color );
    HRESULT DrawSkew( float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4,
                       DWORD c1, DWORD c2, DWORD c3, DWORD c4 );
    HRESULT RenderBatch(bool bWithDepth = false);

    bool GetLimitFPS() const { return m_bLimitFPS; }
    HRESULT SetLimitFPS( bool bLimitFPS );

    RootConstants& GetRootConstants() { return m_RootConstants; };
    FixedSizeConstants& GetFixedSizeConstants() { return m_FixedConstants; };
    TrackColor* GetTrackColors() { return m_TrackColors; };
    void MarkTrackColorsDirty(size_t track) {
        if (track >= MaxTrackColors) track = MaxTrackColors - 1;
        m_uTrackColorsDirtyBegin = min(m_uTrackColorsDirtyBegin, track);
        m_uTrackColorsDirtyEnd = max(m_uTrackColorsDirtyEnd, track + 1);
    }

    inline void PushNoteData(NoteData data) { m_vNotesIntermediate.push_back(data); };
    inline void PushPianoRollStripNoteData(NoteData data) { m_vPianoRollStripNotesIntermediate.push_back(data); };
    float GetDualRollTimeSpan(float normalTimeSpan, float normalRollPixels) const;
    size_t GetRenderedNotesCount() { return m_vNotesIntermediate.size(); };
    void SplitRect() { m_iRectSplit = (int)m_vRectsIntermediate.size(); }

    bool LoadBackgroundBitmap(std::wstring path);

    void ImGuiStartFrame();
    void RenderImGuiFrame();
    ImDrawList* GetDrawList() { return m_pDrawList; }
    void SetPlaybackPosition(float pos) { m_fPlaybackPosition = pos; }
    void SetPianoRollView(int startNote, int endNote, float playheadFraction) {
        m_iPianoRollStartNote = startNote;
        m_iPianoRollEndNote = endNote;
        m_fPianoRollPlayhead = playheadFraction;
    }
    void DrawPianoRollStripKeyboard();
    void SetStripKeyboardColors(DWORD white, DWORD whiteDark, DWORD whiteVeryDark,
                                DWORD sharp, DWORD sharpDark, DWORD sharpVeryDark,
                                DWORD background, DWORD backgroundDark) {
        m_stripKBWhite = white; m_stripKBWhiteDark = whiteDark; m_stripKBWhiteVeryDark = whiteVeryDark;
        m_stripKBSharp = sharp; m_stripKBSharpDark = sharpDark; m_stripKBSharpVeryDark = sharpVeryDark;
        m_stripKBBackground = background; m_stripKBBackgroundDark = backgroundDark;
    }
    void SetStripPressedKeys(const DWORD colors[128], const DWORD ribbonColors[128]) {
        memcpy(m_stripPressedKeys, colors, sizeof(m_stripPressedKeys));
        memcpy(m_stripRibbonColors, ribbonColors, sizeof(m_stripRibbonColors));
    }
    ImTextureID GetBlurTextureID() const { return m_BlurTextureID; }
    void SetRibbonArea(float x, float y, float cx, float cy) {
        m_fRibbonX = x; m_fRibbonY = y; m_fRibbonCX = cx; m_fRibbonCY = cy;
    }

    // --- Image buffer (pre-rendered note chunks) -----------------------------
    // Pre-renders fixed-size chunks of the note roll into offscreen textures
    // and draws them as scrolling textured quads, replacing per-note vertex
    // dispatch with a handful of quad draws in high note-density playback.
    static constexpr unsigned ChunkPoolSize = 64;
    static constexpr long long ImageBufferInvalidChunk = (std::numeric_limits<long long>::max)();

    struct ChunkBuildRequest {
        long long chunk;
        unsigned noteOffset;
        unsigned noteCount;
    };
    struct ChunkQuad {
        long long chunk;
        float yTop;
        float yBottom;
    };

    void ImageBufferBeginFrame(); // cache-key/validity bookkeeping; clears per-frame lists
    bool ImageBufferCanRender() const { return m_bImageBufferCanRender; }
    bool ImageBufferChunkCached(long long chunk) const;
    unsigned ImageBufferGetCachedCount() const;
    void ImageBufferRenderChunk(long long chunk, const NoteData* notes, unsigned noteCount);
    void ImageBufferDrawChunk(long long chunk, float yTop, float yBottom);
    void ImageBufferSetEventCount(unsigned long long count) { m_ullImageBufferEventCount = count; }
    void ImageBufferNotifyFixedChanged() { m_uImageBufferFixedStamp++; }
    void ImageBufferNotifyTrackColorsChanged() { m_uImageBufferTrackColorStamp++; }

    // --- Shared UI/state, public for the game states to poke at --------------
    bool m_bShowPreferences = false;
    bool m_bShowAbout = false;
    bool m_bShowSetResolution = false;
    bool m_bShowTrackSettings = false;
    bool m_bShowRenderDialog = false;

    float m_fCorruption = 0.0f;
    bool m_bCorruptorRamp = false; // ramp corruption 0% -> slider value over the song
    float m_fLastCorruption = 0.0f; // last effective corruption (for the Fun menu readout)
    unsigned m_iLagIntensity = 1; // Fun menu: 1 = no lag, 2/3/4 = NPS-based FPS throttling tiers
    long long m_llLagNPS = 0; // current notes-per-second, fed by GameState for the lag throttler
    void SetLagNPS(long long nps) { m_llLagNPS = nps; }
    bool m_bOverclockArtifacts = false; // Fun menu: warp geometry harder as FPS falls below 60
    double m_dLagFPS = 0.0; // EMA-smoothed FPS, feeds the artifact intensity

    bool m_bMTMode = false; // MIDITrail view mode (Fun menu)
    float m_fMTTilt = 3.0f; // MIDITrail camera tilt

    int m_iPianoRollStartNote = 0;
    int m_iPianoRollEndNote = 127;
    float m_fPianoRollPlayhead = 0.0f;

protected:
    // Shared constants (both backends use the same layout/timings)
    static constexpr unsigned FrameCount = 2;
    static constexpr unsigned RectsPerPass = 10000; // Relatively low limit, but not many rects are supposed to be rendered anyway
    static constexpr unsigned MaxNotesPerPass = 5000000;
    static constexpr unsigned PianoRollStripNoteOffset = MaxNotesPerPass;
    // window; rendering them all neither helps visually (they fill the strip)
    static constexpr unsigned StripNoteBudget = 5000000;
    static constexpr unsigned NoteBufferCapacity = MaxNotesPerPass * 2;
    static constexpr unsigned IndexBufferCount = max(RectsPerPass, MaxNotesPerPass) * 6;
    // Fixed constants + track colors share one upload region so the note
    // pipeline needs a single t1/t2/t3 SRV set in both APIs.
    static constexpr unsigned GenericUploadSize = sizeof(FixedSizeConstants) + MaxTrackColors * 16 * sizeof(TrackColor);

    static ComPtr<IWICImagingFactory> s_pWICFactory;

    int m_iBufferWidth = 0;
    int m_iBufferHeight = 0;
    bool m_bLimitFPS = false;
    bool m_bDeviceLost = false;
    bool m_bAllowTearing = true;
    bool m_bUnlimitedNotes = true;
    unsigned m_NotesPerPass = MaxNotesPerPass;

    HWND m_hWnd = NULL;

    RootConstants m_RootConstants = {};
    FixedSizeConstants m_FixedConstants = {};
    FixedSizeConstants m_OldFixedConstants = {};
    TrackColor m_TrackColors[MaxTrackColors * 16] = {};
    size_t m_uTrackColorsDirtyBegin = SIZE_MAX;
    size_t m_uTrackColorsDirtyEnd = 0;

    std::vector<RectVertex> m_vRectsIntermediate;
    std::vector<NoteData> m_vNotesIntermediate;
    std::vector<NoteData> m_vPianoRollStripNotesIntermediate;
    int m_iRectSplit = -1;

    // Image buffer shared state (backend keeps the GPU resources)
    struct ChunkCacheEntry {
        long long chunk = ImageBufferInvalidChunk;
        unsigned lastUsed = 0;
    };
    ChunkCacheEntry m_ChunkCache[ChunkPoolSize] = {};
    ChunkCacheKey m_ImageBufferKey;
    bool m_bImageBufferCanRender = false;
    unsigned m_uImageBufferFixedStamp = 0;
    unsigned m_uImageBufferTrackColorStamp = 0;
    unsigned long long m_ullImageBufferEventCount = 0;
    unsigned m_uImageBufferFrame = 0;
    std::vector<NoteData> m_vChunkNotes;
    std::vector<ChunkBuildRequest> m_vChunkBuilds;
    std::vector<ChunkQuad> m_vChunkQuads;
    int ImageBufferGetChunkSlot(long long chunk) const;
    int ImageBufferAllocateSlot();
    void ImageBufferMarkBaked(int slot, long long chunk);
    void ImageBufferBuildChunkFixed(FixedSizeConstants& out) const;

    ImDrawList* m_pDrawList = nullptr;
    float m_fLastUIScale = 1.0f;
    std::wstring m_sLastFont;
    bool m_bShowToolbar = true;
    int m_resWidth = 0;
    int m_resHeight = 0;
    float m_fPlaybackPosition = 0.0f;

    // Blur/bloom output exposed to ImGui as a texture id.
    ImTextureID m_BlurTextureID = 0;

    std::vector<char> m_vScreenshotOutput;

    float m_fRibbonX = 0, m_fRibbonY = 0, m_fRibbonCX = 0, m_fRibbonCY = 0;

    DWORD m_stripKBWhite = 0, m_stripKBWhiteDark = 0, m_stripKBWhiteVeryDark = 0;
    DWORD m_stripKBSharp = 0, m_stripKBSharpDark = 0, m_stripKBSharpVeryDark = 0;
    DWORD m_stripKBBackground = 0, m_stripKBBackgroundDark = 0;
    DWORD m_stripPressedKeys[128] = {};
    DWORD m_stripRibbonColors[128] = {};

    ComPtr<IWICBitmapSource> m_pUnscaledBackground;
    float m_fBGBlurSigma = 0.0f;

    // Shared per-frame helpers (backends call into these)
    void UpdateWarpConstants(); // corruption ramp + warp clock (ClearAndBeginScene prelude)
    bool PresentPrelude();      // FPS EMA, lag throttle, minimize gate; true = skip frame
    double m_dPresentWaitMs = 0.0; // per-frame swapchain wait time, fed by PresentBackend
};

class D3D12Renderer : public Renderer {
public:
    D3D12Renderer();
    ~D3D12Renderer();

    std::tuple<HRESULT, const char*> Init(HWND hWnd, bool bLimitFPS) override;
    HRESULT ResetDevice() override;
    HRESULT ClearAndBeginScene(DWORD color) override;
    HRESULT EndScene(bool draw_bg = false) override;
    HRESULT PresentBackend() override;
    HRESULT WaitForGPU() override;
    std::wstring GetAdapterName() override;
    bool GetAdapterVideoMemory(DWORDLONG& used, DWORDLONG& total) override;
    const wchar_t* GetModeName() const override { return L"DirectX 12"; }
    void SetPipeline(Pipeline pipeline) override;
    char* Screenshot() override;
    void SetBackgroundBlur(float sigma) override;
    void ApplyBlur() override;
    void DrawPianoRollStripBackground() override;
    void DrawPianoRollStrip() override;

protected:
    std::tuple<HRESULT, const char*> CreateWindowDependentObjects(HWND hWnd) override;
    void SetupCommandList() override;
    bool UploadBackgroundBitmap() override;
    HRESULT CreateBlurResources() override;
    void DestroyBlurResources() override;
    void ReleaseDeviceResources() override;
    void ImGuiBackendNewFrame() override;
    void ImGuiBackendShutdown() override;
    void ImGuiBackendCreateDeviceObjects() override;

private:
    ComPtr<IDXGIFactory2> m_pFactory;
    ComPtr<IDXGIAdapter2> m_pAdapter;
    ComPtr<ID3D12Device> m_pDevice;
    ComPtr<ID3D12CommandQueue> m_pCommandQueue;
    ComPtr<IDXGISwapChain3> m_pSwapChain;
    ComPtr<ID3D12DescriptorHeap> m_pRTVDescriptorHeap;
    UINT m_uRTVDescriptorSize = 0;
    ComPtr<ID3D12DescriptorHeap> m_pDSVDescriptorHeap;
    UINT m_uDSVDescriptorSize = 0;
    ComPtr<ID3D12DescriptorHeap> m_pSRVDescriptorHeap;
    UINT m_uSRVDescriptorSize = 0;
    ComPtr<ID3D12DescriptorHeap> m_pImGuiSRVDescriptorHeap;
    ComPtr<ID3D12DescriptorHeap> m_pTextureSRVDescriptorHeap;
    UINT m_uTextureSRVDescriptorSize = 0;
    ComPtr<ID3D12Resource> m_pRenderTargets[FrameCount];
    ComPtr<ID3D12Resource> m_pDepthBuffer;
    ComPtr<ID3D12CommandAllocator> m_pCommandAllocator[FrameCount];
    ComPtr<ID3D12RootSignature> m_pRectRootSignature;
    ComPtr<ID3D12PipelineState> m_pRectPipelineState;
    ComPtr<ID3D12RootSignature> m_pNoteRootSignature;
    ComPtr<ID3D12PipelineState> m_pNotePipelineState;
ComPtr<ID3D12RootSignature> m_pBackgroundRootSignature;
    ComPtr<ID3D12PipelineState> m_pBackgroundPipelineState;
    ComPtr<ID3D12RootSignature> m_pChunkQuadRootSignature;
    ComPtr<ID3D12PipelineState> m_pChunkQuadPipelineState;
    ComPtr<ID3D12RootSignature> m_pBloomRootSignature;
    ComPtr<ID3D12PipelineState> m_pBloomPipelineState;
    ComPtr<ID3D12RootSignature> m_pVignetteRootSignature;
    ComPtr<ID3D12PipelineState> m_pVignettePipelineState;
    ComPtr<ID3D12GraphicsCommandList> m_pCommandList;
    ComPtr<ID3D12Fence> m_pFence;
    HANDLE m_hFenceEvent = NULL;
    ComPtr<ID3D12Resource> m_pIndexBuffer;
    D3D12_INDEX_BUFFER_VIEW m_IndexBufferView;
    ComPtr<ID3D12Resource> m_pVertexBuffers[FrameCount];
    D3D12_VERTEX_BUFFER_VIEW m_VertexBufferViews[FrameCount];
    ComPtr<ID3D12Resource> m_pNoteBuffers[FrameCount];
    ComPtr<ID3D12Resource> m_pGenericUpload;
    ComPtr<ID3D12Resource> m_pFixedBuffer;
    ComPtr<ID3D12Resource> m_pTrackColorBuffer;

    // Image buffer (pre-rendered note chunks)
    ComPtr<ID3D12DescriptorHeap> m_pChunkRTVHeap;
    UINT m_uChunkRTVSize = 0;
    ComPtr<ID3D12DescriptorHeap> m_pChunkSRVHeap; // shader-visible
    ComPtr<ID3D12DescriptorHeap> m_pChunkDSVHeap;
    ComPtr<ID3D12Resource> m_pChunkTextures[ChunkPoolSize];
    D3D12_CPU_DESCRIPTOR_HANDLE m_ChunkRTVCPU[ChunkPoolSize] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_ChunkSRVCPU[ChunkPoolSize] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_ChunkSRVGPU[ChunkPoolSize] = {};
    D3D12_RESOURCE_STATES m_ChunkStates[ChunkPoolSize] = {};
    ComPtr<ID3D12Resource> m_pChunkDepth;
    D3D12_CPU_DESCRIPTOR_HANDLE m_ChunkDSVCPU = {};
    int m_iChunkWidth = 0, m_iChunkHeight = 0;
    ComPtr<ID3D12Resource> m_pChunkFixedBuffer; // chunk-relative fixed constants (t1 during bake)
    ComPtr<ID3D12Resource> m_pChunkFixedUpload;
    HRESULT EnsureChunkResources(int slot, int W, int H);
    HRESULT RenderImageBuffer();

    UINT m_uFrameIndex = 0;
    UINT64 m_pFenceValues[FrameCount] = {};

    ComPtr<ID3D12Resource> m_pScreenshotStaging;
    UINT64 m_ullScreenshotPitch;

    ComPtr<ID3D12Resource> m_pTextureUpload;
    ComPtr<ID3D12Resource> m_pTextureBuffer;

    ComPtr<ID3D12DescriptorHeap> m_pBGBlurHeap;
    ComPtr<ID3D12Resource> m_pBGBufferA;
    ComPtr<ID3D12Resource> m_pBGBufferB;
    D3D12_CPU_DESCRIPTOR_HANDLE m_BGBgTexSRVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BGBgTexSRVGPU = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_BGBgAUAVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BGBgAUAVGPU = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_BGBgASRVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BGBgASRVGPU = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_BGBgBUAVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BGBgBUAVGPU = {};

    ComPtr<ID3D12RootSignature> m_pBlurRootSignature;
    ComPtr<ID3D12PipelineState> m_pBlurPipelineState;
    ComPtr<ID3D12PipelineState> m_pCompositePipelineState;
    ComPtr<ID3D12Resource> m_pSceneCopy;
    ComPtr<ID3D12Resource> m_pBlurTemp;
    ComPtr<ID3D12Resource> m_pBlurOutput;
    D3D12_CPU_DESCRIPTOR_HANDLE m_BlurSceneSRVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BlurSceneSRVGPU = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_BlurTempSRVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BlurTempSRVGPU = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_BlurTempUAVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BlurTempUAVGPU = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_BlurOutputSRVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BlurOutputSRVGPU = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_BlurOutputUAVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BlurOutputUAVGPU = {};
    D3D12_RESOURCE_STATES m_BlurTempState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES m_BlurOutputState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES m_SceneCopyState = D3D12_RESOURCE_STATE_COPY_DEST;

    ComPtr<ID3D12Resource> m_pBloomHalf;        // half-res RGBA16F bright-pass extract
    ComPtr<ID3D12Resource> m_pBloomBlurTemp;     // half-res RGBA16F horizontal blur intermediate
    ComPtr<ID3D12Resource> m_pBloomBlurResult;   // half-res RGBA16F final bloom blur output
    D3D12_CPU_DESCRIPTOR_HANDLE m_BloomHalfSRVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BloomHalfSRVGPU = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_BloomHalfUAVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BloomHalfUAVGPU = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_BloomBlurTempSRVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BloomBlurTempSRVGPU = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_BloomBlurTempUAVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BloomBlurTempUAVGPU = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_BloomBlurResultSRVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BloomBlurResultSRVGPU = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_BloomBlurResultUAVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BloomBlurResultUAVGPU = {};
    D3D12_RESOURCE_STATES m_BloomHalfState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES m_BloomBlurTempState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES m_BloomBlurResultState = D3D12_RESOURCE_STATE_COMMON;
    int m_iBloomHalfWidth = 0, m_iBloomHalfHeight = 0;
    ComPtr<ID3D12PipelineState> m_pBloomExtractPipelineState;
    ComPtr<ID3D12RootSignature> m_pBloomExtractRootSignature;
};