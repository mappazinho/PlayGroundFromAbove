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
#include <wincodec.h>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx12.h"
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

class D3D12Renderer {
public:
    enum FontSize { Small, SmallBold, SmallComic, Medium, Large };

    D3D12Renderer();
    ~D3D12Renderer();

    std::tuple<HRESULT, const char*> Init( HWND hWnd, bool bLimitFPS );
    HRESULT ResetDeviceIfNeeded();
    HRESULT ResetDevice();
    HRESULT ClearAndBeginScene( DWORD color );
    HRESULT EndScene(bool draw_bg = false);
    HRESULT Present();
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

    // GPU resource and resume rendering; returns the Init result.
    HRESULT RecoverDevice(HWND hWnd, bool bLimitFPS);
    bool DeviceLost() const { return m_bDeviceLost; }

    int GetBufferWidth() const { return m_iBufferWidth; }
    int GetBufferHeight() const { return m_iBufferHeight; }

    HRESULT WaitForGPU();
    std::wstring GetAdapterName();
    void SetPipeline(Pipeline pipeline);

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

    char* Screenshot();
    bool LoadBackgroundBitmap(std::wstring path);
    void SetBackgroundBlur(float sigma);

    void ImGuiStartFrame();
    void RenderImGuiFrame();
    ImDrawList* GetDrawList() { return m_pDrawList; }
    void SetPlaybackPosition(float pos) { m_fPlaybackPosition = pos; }
    void SetPianoRollView(int startNote, int endNote, float playheadFraction) {
        m_iPianoRollStartNote = startNote;
        m_iPianoRollEndNote = endNote;
        m_fPianoRollPlayhead = playheadFraction;
    }
    void ApplyBlur();
    void DrawPianoRollStripBackground();
    void DrawPianoRollStrip();
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

    bool m_bShowPreferences = false;
    bool m_bShowAbout = false;
    bool m_bShowSetResolution = false;
    bool m_bShowTrackSettings = false;

    float m_fCorruption = 0.0f;
    bool m_bCorruptorRamp = false; // ramp corruption 0% -> slider value over the song
    float m_fLastCorruption = 0.0f; // last effective corruption (for the Fun menu readout)

    int m_iPianoRollStartNote = 0;
    int m_iPianoRollEndNote = 127;
    float m_fPianoRollPlayhead = 0.0f;

private:
    std::tuple<HRESULT, const char*> CreateWindowDependentObjects(HWND hWnd);
    void SetupCommandList();
    bool UploadBackgroundBitmap();
    void UpdateImGuiSettings();
    HRESULT CreateBlurResources();
    void DestroyBlurResources();
    void ReleaseDeviceResources();

    static constexpr unsigned FrameCount = 2;
    static constexpr unsigned RectsPerPass = 10000; // Relatively low limit, but not many rects are supposed to be rendered anyway
    static constexpr unsigned MaxNotesPerPass = 5000000;
    static constexpr unsigned PianoRollStripNoteOffset = MaxNotesPerPass;
    // window; rendering them all neither helps visually (they fill the strip)
    static constexpr unsigned StripNoteBudget = 5000000;
    static constexpr unsigned NoteBufferCapacity = MaxNotesPerPass * 2;
    static constexpr unsigned IndexBufferCount = max(RectsPerPass, MaxNotesPerPass) * 6;
    static constexpr unsigned GenericUploadSize = sizeof(FixedSizeConstants) + MaxTrackColors * 16 * sizeof(TrackColor);

    static ComPtr<IWICImagingFactory> s_pWICFactory;

    int m_iBufferWidth = 0;
    int m_iBufferHeight = 0;
    bool m_bLimitFPS = false;
    bool m_bDeviceLost = false;
    bool m_bUnlimitedNotes = true;
    unsigned m_NotesPerPass = MaxNotesPerPass;

    HWND m_hWnd = NULL;
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
    ComPtr<ID3D12RootSignature> m_pBloomRootSignature;
    ComPtr<ID3D12PipelineState> m_pBloomPipelineState;
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
    RootConstants m_RootConstants = {};
    FixedSizeConstants m_FixedConstants = {};
    FixedSizeConstants m_OldFixedConstants = {};
    TrackColor m_TrackColors[MaxTrackColors * 16] = {};
    size_t m_uTrackColorsDirtyBegin = SIZE_MAX;
    size_t m_uTrackColorsDirtyEnd = 0;

    UINT m_uFrameIndex = 0;
    UINT64 m_pFenceValues[FrameCount] = {};

    ComPtr<ID3D12Resource> m_pScreenshotStaging;
    std::vector<char> m_vScreenshotOutput;
    UINT64 m_ullScreenshotPitch;

    ComPtr<ID3D12Resource> m_pTextureUpload;
    ComPtr<ID3D12Resource> m_pTextureBuffer;
    ComPtr<IWICBitmapSource> m_pUnscaledBackground;

    ComPtr<ID3D12DescriptorHeap> m_pBGBlurHeap;
    ComPtr<ID3D12Resource> m_pBGBufferA;
    ComPtr<ID3D12Resource> m_pBGBufferB;
    float m_fBGBlurSigma = 0.0f;
    D3D12_CPU_DESCRIPTOR_HANDLE m_BGBgTexSRVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BGBgTexSRVGPU = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_BGBgAUAVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BGBgAUAVGPU = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_BGBgASRVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BGBgASRVGPU = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_BGBgBUAVCPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_BGBgBUAVGPU = {};

    std::vector<RectVertex> m_vRectsIntermediate;
    std::vector<NoteData> m_vNotesIntermediate;
    std::vector<NoteData> m_vPianoRollStripNotesIntermediate;
    int m_iRectSplit = -1;

    ImDrawList* m_pDrawList = nullptr;
    float m_fLastUIScale = 1.0f;
    std::wstring m_sLastFont;
    bool m_bShowToolbar = true;
    int m_resWidth = 0;
    int m_resHeight = 0;
    float m_fPlaybackPosition = 0.0f;

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
    ImTextureID m_BlurTextureID = 0;
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

    float m_fRibbonX = 0, m_fRibbonY = 0, m_fRibbonCX = 0, m_fRibbonCY = 0;

    DWORD m_stripKBWhite = 0, m_stripKBWhiteDark = 0, m_stripKBWhiteVeryDark = 0;
    DWORD m_stripKBSharp = 0, m_stripKBSharpDark = 0, m_stripKBSharpVeryDark = 0;
    DWORD m_stripKBBackground = 0, m_stripKBBackgroundDark = 0;
    DWORD m_stripPressedKeys[128] = {};
    DWORD m_stripRibbonColors[128] = {};
};
