/*************************************************************************************************
*
* File: RendererD3D11.h
*
* Description: Direct3D 11 renderer backend (fallback for systems without D3D12).
*
* Copyright (c) 2010 Brian Pantano. All rights reserved.
*
*************************************************************************************************/
#pragma once

#include "Renderer.h"

#include <d3d11.h>
#include <d3d11_3.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class D3D11Renderer : public Renderer {
public:
    D3D11Renderer();
    ~D3D11Renderer();

    std::tuple<HRESULT, const char*> Init(HWND hWnd, bool bLimitFPS) override;
    HRESULT ResetDevice() override;
    HRESULT ClearAndBeginScene(DWORD color) override;
    HRESULT EndScene(bool draw_bg = false) override;
    HRESULT PresentBackend() override;
    HRESULT WaitForGPU() override;
    std::wstring GetAdapterName() override;
    const wchar_t* GetModeName() const override { return L"DirectX 11"; }
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
    // Matches the DX11 shaders' cbuffer layout (common.hlsli minus the fMT
    // pair, which lives at the tail of the C++ RootConstants struct).
    struct D3D11RootConstants {
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
    };
    static_assert(sizeof(D3D11RootConstants) == 116);

    void UploadPerFrameConstants();

    ComPtr<IDXGIFactory2> m_pFactory;
    ComPtr<IDXGIAdapter2> m_pAdapter;
    ComPtr<ID3D11Device> m_pDevice;
    ComPtr<ID3D11DeviceContext> m_pContext;
    ComPtr<ID3D11DeviceContext4> m_pContext4;
    ComPtr<IDXGISwapChain3> m_pSwapChain;
    ComPtr<ID3D11Fence> m_pFence;
    HANDLE m_hFenceEvent = NULL;
    UINT64 m_uFenceValue = 0;
    UINT m_uFrameIndex = 0;

    ComPtr<ID3D11RenderTargetView> m_pRTVs[FrameCount];
    ComPtr<ID3D11Texture2D> m_pDepthBuffer;
    ComPtr<ID3D11DepthStencilView> m_pDSV;

    ComPtr<ID3D11VertexShader> m_pRectVS;
    ComPtr<ID3D11PixelShader> m_pRectPS;
    ComPtr<ID3D11VertexShader> m_pNoteVS;
    ComPtr<ID3D11PixelShader> m_pNotePS;
    ComPtr<ID3D11VertexShader> m_pBackgroundVS;
    ComPtr<ID3D11PixelShader> m_pBackgroundPS;
    ComPtr<ID3D11ComputeShader> m_pBlurCS;
    ComPtr<ID3D11ComputeShader> m_pBloomExtractCS;
    ComPtr<ID3D11PixelShader> m_pBloomPS;

    ComPtr<ID3D11InputLayout> m_pRectInputLayout;
    ComPtr<ID3D11Buffer> m_pIndexBuffer;

    ComPtr<ID3D11Buffer> m_pPerFrameConstants;
    ComPtr<ID3D11Buffer> m_pBackgroundConstants;
    ComPtr<ID3D11Buffer> m_pBlurConstants;
    ComPtr<ID3D11Buffer> m_pBloomExtractConstants;
    ComPtr<ID3D11Buffer> m_pBloomConstants;

    ComPtr<ID3D11Buffer> m_pFixedBuffer;
    ComPtr<ID3D11ShaderResourceView> m_pFixedSRV;
    ComPtr<ID3D11Buffer> m_pTrackColorBuffer;
    ComPtr<ID3D11ShaderResourceView> m_pTrackColorSRV;
    ComPtr<ID3D11Buffer> m_pNoteBuffer;      // dynamic, NoteBufferCapacity elements
    ComPtr<ID3D11ShaderResourceView> m_pNoteSRV;
    ComPtr<ID3D11ShaderResourceView> m_pStripNoteSRV; // starts at PianoRollStripNoteOffset
    ComPtr<ID3D11Buffer> m_pRectVertexBuffer; // dynamic, RectsPerPass * 4 vertices

    ComPtr<ID3D11BlendState> m_pBlendInverted;
    ComPtr<ID3D11BlendState> m_pBlendBloom;
    ComPtr<ID3D11RasterizerState> m_pRasterizer;
    ComPtr<ID3D11DepthStencilState> m_pDepthDisabled;
    ComPtr<ID3D11DepthStencilState> m_pDepthEnabled;
    ComPtr<ID3D11SamplerState> m_pSamplerPointWrap;
    ComPtr<ID3D11SamplerState> m_pSamplerLinearClamp;

    D3D11_VIEWPORT m_Viewport = {};
    D3D11_RECT m_FullScissor = {};

    // Background image
    ComPtr<ID3D11Texture2D> m_pBackgroundTexture;
    ComPtr<ID3D11ShaderResourceView> m_pBackgroundSRV;

    // Scene blur resources
    ComPtr<ID3D11Texture2D> m_pSceneCopy;
    ComPtr<ID3D11ShaderResourceView> m_pSceneCopySRV;
    ComPtr<ID3D11Texture2D> m_pBlurTemp;
    ComPtr<ID3D11ShaderResourceView> m_pBlurTempSRV;
    ComPtr<ID3D11UnorderedAccessView> m_pBlurTempUAV;
    ComPtr<ID3D11Texture2D> m_pBlurOutput;
    ComPtr<ID3D11ShaderResourceView> m_pBlurOutputSRV;
    ComPtr<ID3D11UnorderedAccessView> m_pBlurOutputUAV;

    // Background blur buffers
    ComPtr<ID3D11Texture2D> m_pBGBufferA;
    ComPtr<ID3D11ShaderResourceView> m_pBGBufferASRV;
    ComPtr<ID3D11UnorderedAccessView> m_pBGBufferAUAV;
    ComPtr<ID3D11Texture2D> m_pBGBufferB;
    ComPtr<ID3D11ShaderResourceView> m_pBGBufferBSRV;
    ComPtr<ID3D11UnorderedAccessView> m_pBGBufferBUAV;

    // Bloom resources
    ComPtr<ID3D11Texture2D> m_pBloomHalf;
    ComPtr<ID3D11ShaderResourceView> m_pBloomHalfSRV;
    ComPtr<ID3D11UnorderedAccessView> m_pBloomHalfUAV;
    ComPtr<ID3D11Texture2D> m_pBloomBlurTemp;
    ComPtr<ID3D11ShaderResourceView> m_pBloomBlurTempSRV;
    ComPtr<ID3D11UnorderedAccessView> m_pBloomBlurTempUAV;
    ComPtr<ID3D11Texture2D> m_pBloomBlurResult;
    ComPtr<ID3D11ShaderResourceView> m_pBloomBlurResultSRV;
    ComPtr<ID3D11UnorderedAccessView> m_pBloomBlurResultUAV;
    int m_iBloomHalfWidth = 0, m_iBloomHalfHeight = 0;

    // Screenshot
    ComPtr<ID3D11Texture2D> m_pScreenshotStaging;
};
