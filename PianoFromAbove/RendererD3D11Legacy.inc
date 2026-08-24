/*************************************************************************************************
*
* File: RendererD3D11.cpp
*
* Description: Direct3D 11 renderer backend. Mirrors D3D12Renderer (Renderer.cpp)
*              with the shared batching/UI logic in RendererBase.cpp.
*
* Copyright (c) 2010 Brian Pantano. All rights reserved.
*
*************************************************************************************************/
#include <Windows.h>
#include "RectPixelShader11.h"
#include "RectVertexShader11.h"
#include "NotePixelShader11.h"
#include "NoteVertexShader11.h"
#include "BackgroundPixelShader11.h"
#include "BackgroundVertexShader11.h"
#include "ChunkQuadPixelShader11.h"
#include "ChunkQuadVertexShader11.h"
#include "BlurShader11.h"
#include "BloomExtractShader11.h"
#include "BloomPixelShader11.h"
#include "VignettePixelShader11.h"
#include "Globals.h"
#include "Renderer.h"
#include "RendererD3D11.h"
#include <d3d11_4.h>

#include <d3dcompiler.h>
#include <chrono>
#include "Config.h"
#include "MainProcs.h"
#include "imgui/imgui_impl_dx11.h"

D3D11Renderer::D3D11Renderer() {}

D3D11Renderer::~D3D11Renderer() {
    if (m_pContext4 && m_pFence && m_hFenceEvent) {
        m_pContext4->Signal(m_pFence.Get(), ++m_uFenceValue);
        m_pFence->SetEventOnCompletion(m_uFenceValue, m_hFenceEvent);
        WaitForSingleObject(m_hFenceEvent, 3000);
    }

    DestroyBlurResources();

    if (m_hFenceEvent)
        CloseHandle(m_hFenceEvent);
}

std::tuple<HRESULT, const char*> D3D11Renderer::Init(HWND hWnd, bool bLimitFPS) {
    HRESULT res;

#ifdef _DEBUG
    res = CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&m_pFactory));
#else
    res = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_pFactory));
#endif
    if (FAILED(res))
        return std::make_tuple(res, "CreateDXGIFactory2");

    m_hWnd = hWnd;
    m_bLimitFPS = bLimitFPS;
    {
        char buf[64];
        sprintf_s(buf, "init(d3d11):limitfps=%d", (int)bLimitFPS);
        HeartbeatLog(buf);
    }

    IDXGIAdapter1* adapter = nullptr;
    if (!g_bForceWARP) {
        for (UINT i = 0; m_pFactory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC2 desc = {};
            ComPtr<IDXGIAdapter2> adapter2;
            if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&adapter2))))
                continue;
            if (FAILED(adapter2->GetDesc2(&desc)))
                continue;
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue;

            D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
            flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
            res = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                                    &m_pDevice, nullptr, &m_pContext);
#ifdef _DEBUG
            if (FAILED(res)) {
                res = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                        levels, 2, D3D11_SDK_VERSION, &m_pDevice, nullptr, &m_pContext);
            }
#endif
            if (SUCCEEDED(res)) {
                m_pAdapter = adapter2;
                break;
            }
        }
    }
    if (m_pDevice == nullptr) {
        // No usable hardware device (WARP forced after a failed hardware init, or
        // no suitable adapter at all): fall back to the WARP software rasterizer
        // so the app can still boot on legacy/RDP/virtual machines with broken or
        // missing WDDM support.
        HeartbeatLog("init(d3d11):nodevice - trying WARP");
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        res = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                levels, 2, D3D11_SDK_VERSION, &m_pDevice, nullptr, &m_pContext);
        if (FAILED(res))
            return std::make_tuple(res, "D3D11CreateDevice (WARP)");
        m_bAllowTearing = false;
        HeartbeatLog("init(d3d11):warp");
    }

    ComPtr<ID3D11Device5> device5;
    res = m_pDevice->QueryInterface(IID_PPV_ARGS(&device5));
    if (FAILED(res))
        return std::make_tuple(res, "ID3D11Device5");
    res = m_pContext->QueryInterface(IID_PPV_ARGS(&m_pContext4));
    if (FAILED(res))
        return std::make_tuple(res, "ID3D11DeviceContext4");

    {
        std::wstring wsAdapter = GetAdapterName();
        char buf[256];
        sprintf_s(buf, "init(d3d11):adapter=%ls", wsAdapter.c_str());
        HeartbeatLog(buf);
    }

    res = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_pFence));
    if (FAILED(res))
        return std::make_tuple(res, "CreateFence");
    m_hFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    m_uFenceValue = 0;

    // Create rect shaders
    res = m_pDevice->CreateVertexShader(g_pRectVertexShader11, sizeof(g_pRectVertexShader11), nullptr, &m_pRectVS);
    if (FAILED(res))
        return std::make_tuple(res, "CreateVertexShader (rect)");
    res = m_pDevice->CreatePixelShader(g_pRectPixelShader11, sizeof(g_pRectPixelShader11), nullptr, &m_pRectPS);
    if (FAILED(res))
        return std::make_tuple(res, "CreatePixelShader (rect)");

    // Create note shaders
    res = m_pDevice->CreateVertexShader(g_pNoteVertexShader11, sizeof(g_pNoteVertexShader11), nullptr, &m_pNoteVS);
    if (FAILED(res))
        return std::make_tuple(res, "CreateVertexShader (note)");
    res = m_pDevice->CreatePixelShader(g_pNotePixelShader11, sizeof(g_pNotePixelShader11), nullptr, &m_pNotePS);
    if (FAILED(res))
        return std::make_tuple(res, "CreatePixelShader (note)");

    // Create background shaders
    res = m_pDevice->CreateVertexShader(g_pBackgroundVertexShader11, sizeof(g_pBackgroundVertexShader11), nullptr, &m_pBackgroundVS);
    if (FAILED(res))
        return std::make_tuple(res, "CreateVertexShader (background)");
    res = m_pDevice->CreatePixelShader(g_pBackgroundPixelShader11, sizeof(g_pBackgroundPixelShader11), nullptr, &m_pBackgroundPS);
    if (FAILED(res))
        return std::make_tuple(res, "CreatePixelShader (background)");

    // Create image buffer chunk quad shaders
    res = m_pDevice->CreateVertexShader(g_pChunkQuadVertexShader11, sizeof(g_pChunkQuadVertexShader11), nullptr, &m_pChunkQuadVS);
    if (FAILED(res))
        return std::make_tuple(res, "CreateVertexShader (chunk quad)");
    res = m_pDevice->CreatePixelShader(g_pChunkQuadPixelShader11, sizeof(g_pChunkQuadPixelShader11), nullptr, &m_pChunkQuadPS);
    if (FAILED(res))
        return std::make_tuple(res, "CreatePixelShader (chunk quad)");

    // Rect input layout (POSITION float2 + COLOR B8G8R8A8_UNORM)
    D3D11_INPUT_ELEMENT_DESC rect_vertex_input[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    res = m_pDevice->CreateInputLayout(rect_vertex_input, _countof(rect_vertex_input),
                                       g_pRectVertexShader11, sizeof(g_pRectVertexShader11), &m_pRectInputLayout);
    if (FAILED(res))
        return std::make_tuple(res, "CreateInputLayout (rect)");

    // Blend states: PFA inverts blending (0 = opaque, 255 = transparent)
    D3D11_BLEND_DESC blend_desc = {};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    res = m_pDevice->CreateBlendState(&blend_desc, &m_pBlendInverted);
    if (FAILED(res))
        return std::make_tuple(res, "CreateBlendState (inverted)");

    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_BLEND_FACTOR;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    res = m_pDevice->CreateBlendState(&blend_desc, &m_pBlendBloom);
    if (FAILED(res))
        return std::make_tuple(res, "CreateBlendState (bloom)");

    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_COLOR;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    res = m_pDevice->CreateBlendState(&blend_desc, &m_pBlendVignette);
    if (FAILED(res))
        return std::make_tuple(res, "CreateBlendState (vignette)");

    // Rasterizer: back-face culling, depth clip, scissor enabled (mirrors the
    // D3D12 default rasterizer + per-draw RSSetScissorRects)
    D3D11_RASTERIZER_DESC rs_desc = {};
    rs_desc.FillMode = D3D11_FILL_SOLID;
    rs_desc.CullMode = D3D11_CULL_BACK;
    rs_desc.FrontCounterClockwise = FALSE;
    rs_desc.DepthBias = 0;
    rs_desc.DepthBiasClamp = 0.0f;
    rs_desc.SlopeScaledDepthBias = 0.0f;
    rs_desc.DepthClipEnable = TRUE;
    rs_desc.ScissorEnable = TRUE;
    rs_desc.MultisampleEnable = FALSE;
    rs_desc.AntialiasedLineEnable = FALSE;
    res = m_pDevice->CreateRasterizerState(&rs_desc, &m_pRasterizer);
    if (FAILED(res))
        return std::make_tuple(res, "CreateRasterizerState");

    // Depth stencil states
    D3D11_DEPTH_STENCIL_DESC ds_desc = {};
    ds_desc.DepthEnable = FALSE;
    ds_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ds_desc.DepthFunc = D3D11_COMPARISON_LESS;
    res = m_pDevice->CreateDepthStencilState(&ds_desc, &m_pDepthDisabled);
    if (FAILED(res))
        return std::make_tuple(res, "CreateDepthStencilState (disabled)");
    ds_desc.DepthEnable = TRUE;
    ds_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    res = m_pDevice->CreateDepthStencilState(&ds_desc, &m_pDepthEnabled);
    if (FAILED(res))
        return std::make_tuple(res, "CreateDepthStencilState (enabled)");

    // Samplers
    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    res = m_pDevice->CreateSamplerState(&sampler_desc, &m_pSamplerPointWrap);
    if (FAILED(res))
        return std::make_tuple(res, "CreateSamplerState (point wrap)");
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    res = m_pDevice->CreateSamplerState(&sampler_desc, &m_pSamplerLinearClamp);
    if (FAILED(res))
        return std::make_tuple(res, "CreateSamplerState (linear clamp)");

    // Constant buffers
    D3D11_BUFFER_DESC cb_desc = {};
    cb_desc.Usage = D3D11_USAGE_DEFAULT;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.ByteWidth = 128; // proj + 14 floats, 16-byte aligned
    res = m_pDevice->CreateBuffer(&cb_desc, nullptr, &m_pPerFrameConstants);
    if (FAILED(res))
        return std::make_tuple(res, "CreateBuffer (per-frame constants)");
    cb_desc.ByteWidth = 16;
    res = m_pDevice->CreateBuffer(&cb_desc, nullptr, &m_pBackgroundConstants);
    if (FAILED(res))
        return std::make_tuple(res, "CreateBuffer (background constants)");
    res = m_pDevice->CreateBuffer(&cb_desc, nullptr, &m_pBlurConstants);
    if (FAILED(res))
        return std::make_tuple(res, "CreateBuffer (blur constants)");
    res = m_pDevice->CreateBuffer(&cb_desc, nullptr, &m_pBloomExtractConstants);
    if (FAILED(res))
        return std::make_tuple(res, "CreateBuffer (bloom extract constants)");
    cb_desc.ByteWidth = 32;
    res = m_pDevice->CreateBuffer(&cb_desc, nullptr, &m_pBloomConstants);
    if (FAILED(res))
        return std::make_tuple(res, "CreateBuffer (bloom constants)");
    cb_desc.ByteWidth = 16;
    res = m_pDevice->CreateBuffer(&cb_desc, nullptr, &m_pVignetteConstants);
    if (FAILED(res))
        return std::make_tuple(res, "CreateBuffer (vignette constants)");
    cb_desc.ByteWidth = 128; // D3D11RootConstants layout
    res = m_pDevice->CreateBuffer(&cb_desc, nullptr, &m_pChunkConstants);
    if (FAILED(res))
        return std::make_tuple(res, "CreateBuffer (chunk constants)");
    cb_desc.ByteWidth = 80; // proj + quadPos + quadSize
    res = m_pDevice->CreateBuffer(&cb_desc, nullptr, &m_pChunkQuadConstants);
    if (FAILED(res))
        return std::make_tuple(res, "CreateBuffer (chunk quad constants)");

    // Fixed size data buffer (t1)
    D3D11_BUFFER_DESC fixed_desc = {};
    fixed_desc.Usage = D3D11_USAGE_DEFAULT;
    fixed_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    fixed_desc.ByteWidth = sizeof(FixedSizeConstants);
    fixed_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    fixed_desc.StructureByteStride = sizeof(FixedSizeConstants);
    res = m_pDevice->CreateBuffer(&fixed_desc, nullptr, &m_pFixedBuffer);
    if (FAILED(res))
        return std::make_tuple(res, "CreateBuffer (fixed)");
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = DXGI_FORMAT_UNKNOWN;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    srv_desc.BufferEx.FirstElement = 0;
    srv_desc.BufferEx.NumElements = 1;
    srv_desc.BufferEx.Flags = 0;
    res = m_pDevice->CreateShaderResourceView(m_pFixedBuffer.Get(), &srv_desc, &m_pFixedSRV);
    if (FAILED(res))
        return std::make_tuple(res, "CreateShaderResourceView (fixed)");

    // Chunk-relative fixed size data buffer (t1 during image buffer bakes)
    res = m_pDevice->CreateBuffer(&fixed_desc, nullptr, &m_pChunkFixedBuffer);
    if (FAILED(res))
        return std::make_tuple(res, "CreateBuffer (chunk fixed)");
    srv_desc.BufferEx.FirstElement = 0;
    srv_desc.BufferEx.NumElements = 1;
    res = m_pDevice->CreateShaderResourceView(m_pChunkFixedBuffer.Get(), &srv_desc, &m_pChunkFixedSRV);
    if (FAILED(res))
        return std::make_tuple(res, "CreateShaderResourceView (chunk fixed)");

    // Track color buffer (t2)
    D3D11_BUFFER_DESC tc_desc = {};
    tc_desc.Usage = D3D11_USAGE_DEFAULT;
    tc_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    tc_desc.ByteWidth = MaxTrackColors * 16 * sizeof(TrackColor);
    tc_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    tc_desc.StructureByteStride = sizeof(TrackColor);
    res = m_pDevice->CreateBuffer(&tc_desc, nullptr, &m_pTrackColorBuffer);
    if (FAILED(res))
        return std::make_tuple(res, "CreateBuffer (track colors)");
    srv_desc.BufferEx.FirstElement = 0;
    srv_desc.BufferEx.NumElements = MaxTrackColors * 16;
    res = m_pDevice->CreateShaderResourceView(m_pTrackColorBuffer.Get(), &srv_desc, &m_pTrackColorSRV);
    if (FAILED(res))
        return std::make_tuple(res, "CreateShaderResourceView (track colors)");

    // Dynamic note buffer (t3): [0, MaxNotesPerPass) main, [PianoRollStripNoteOffset, ..) strip
    D3D11_BUFFER_DESC note_desc = {};
    note_desc.Usage = D3D11_USAGE_DYNAMIC;
    note_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    note_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    note_desc.ByteWidth = NoteBufferCapacity * sizeof(NoteData);
    note_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    note_desc.StructureByteStride = sizeof(NoteData);
    res = m_pDevice->CreateBuffer(&note_desc, nullptr, &m_pNoteBuffer);
    if (FAILED(res))
        return std::make_tuple(res, "CreateBuffer (notes)");
    srv_desc.BufferEx.FirstElement = 0;
    srv_desc.BufferEx.NumElements = MaxNotesPerPass;
    res = m_pDevice->CreateShaderResourceView(m_pNoteBuffer.Get(), &srv_desc, &m_pNoteSRV);
    if (FAILED(res))
        return std::make_tuple(res, "CreateShaderResourceView (notes)");
    srv_desc.BufferEx.FirstElement = PianoRollStripNoteOffset;
    srv_desc.BufferEx.NumElements = NoteBufferCapacity - PianoRollStripNoteOffset;
    res = m_pDevice->CreateShaderResourceView(m_pNoteBuffer.Get(), &srv_desc, &m_pStripNoteSRV);
    if (FAILED(res))
        return std::make_tuple(res, "CreateShaderResourceView (strip notes)");

    // Dynamic rect vertex buffer
    D3D11_BUFFER_DESC vb_desc = {};
    vb_desc.Usage = D3D11_USAGE_DYNAMIC;
    vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb_desc.ByteWidth = RectsPerPass * 4 * sizeof(RectVertex);
    res = m_pDevice->CreateBuffer(&vb_desc, nullptr, &m_pRectVertexBuffer);
    if (FAILED(res))
        return std::make_tuple(res, "CreateBuffer (rect vertices)");

    // Index buffer (6 indices per quad, R32_UINT)
    std::vector<uint32_t> index_buffer_vec;
    index_buffer_vec.resize(IndexBufferCount);
    for (uint32_t i = 0; i < IndexBufferCount / 6; i++) {
        index_buffer_vec[i * 6] = i * 4;
        index_buffer_vec[i * 6 + 1] = i * 4 + 1;
        index_buffer_vec[i * 6 + 2] = i * 4 + 2;
        index_buffer_vec[i * 6 + 3] = i * 4;
        index_buffer_vec[i * 6 + 4] = i * 4 + 2;
        index_buffer_vec[i * 6 + 5] = i * 4 + 3;
    }
    D3D11_BUFFER_DESC ib_desc = {};
    ib_desc.Usage = D3D11_USAGE_DEFAULT;
    ib_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ib_desc.ByteWidth = IndexBufferCount * sizeof(uint32_t);
    D3D11_SUBRESOURCE_DATA ib_data = { index_buffer_vec.data(), 0, 0 };
    res = m_pDevice->CreateBuffer(&ib_desc, &ib_data, &m_pIndexBuffer);
    if (FAILED(res))
        return std::make_tuple(res, "CreateBuffer (index buffer)");

    m_uFenceValue++;
    m_uFrameIndex = 0;

    auto res2 = CreateWindowDependentObjects(hWnd);
    if (FAILED(std::get<0>(res2)))
        return res2;

    // Initialize ImGui. Backend init must precede UpdateImGuiSettings: the
    // latter calls ImGuiBackendCreateDeviceObjects via ImGui_ImplDX11, which
    // dereferences the backend data set by ImGui_ImplDX11_Init.
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(hWnd);
    ImGui_ImplDX11_Init(m_pDevice.Get(), m_pContext.Get());

    // Update ImGui settings
    UpdateImGuiSettings();

    if (m_pDrawList) {
        delete m_pDrawList;
    }
    m_pDrawList = new ImDrawList(ImGui::GetDrawListSharedData());

    HRESULT blurRes = CreateBlurResources();
    if (FAILED(blurRes))
        return std::make_tuple(blurRes, "CreateBlurResources");
    HeartbeatLog("init(d3d11):done");

    return std::make_tuple(S_OK, "");
}

std::tuple<HRESULT, const char*> D3D11Renderer::CreateWindowDependentObjects(HWND hWnd) {
    HRESULT res;
    if (m_pSwapChain) {
        HeartbeatLog("resize(d3d11):gpuwait");
        if (FAILED(WaitForGPU()))
            return std::make_tuple(E_FAIL, "WaitForGPU (device lost)");

        for (uint32_t i = 0; i < FrameCount; i++)
            m_pRTVs[i].Reset();
        m_pDepthBuffer.Reset();
        m_pDSV.Reset();
        m_pScreenshotStaging.Reset();
        m_pBackgroundTexture.Reset();
        m_pBackgroundSRV.Reset();

        HeartbeatLog("resize(d3d11):resizebuffers");
        res = m_pSwapChain->ResizeBuffers(FrameCount, 0, 0, DXGI_FORMAT_B8G8R8A8_UNORM,
                                          m_bAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        if (FAILED(res)) {
            if (res == DXGI_ERROR_DEVICE_REMOVED || res == DXGI_ERROR_DEVICE_RESET) {
                m_bDeviceLost = true;
                HeartbeatLog("resize(d3d11):deviceremoved");
            }
            return std::make_tuple(res, "ResizeBuffers");
        }
        HeartbeatLog("resize(d3d11):afterresize");
    } else {
        DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {
            .Width = 0,
            .Height = 0,
            .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
            .Stereo = FALSE,
            .SampleDesc = {
                .Count = 1,
            },
            .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
            .BufferCount = FrameCount,
            .Scaling = g_bForceWARP ? DXGI_SCALING_STRETCH : DXGI_SCALING_NONE,
            .SwapEffect = g_bForceWARP ? DXGI_SWAP_EFFECT_DISCARD : DXGI_SWAP_EFFECT_FLIP_DISCARD,
            .AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
            .Flags = (UINT)(m_bAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0),
        };
        ComPtr<IDXGISwapChain1> temp_swapchain;
        res = m_pFactory->CreateSwapChainForHwnd(m_pDevice.Get(), hWnd, &swap_chain_desc, nullptr, nullptr, &temp_swapchain);
        if (FAILED(res)) {
            // Flip-model swapchains (FLIP_DISCARD) require a WDDM 1.2+ driver (FL 11.1
            // path) and fail with E_ACCESSDENIED on WDDM 1.x drivers, RDP sessions,
            // virtual GPUs, etc. Fall back to the bitblt model, which works everywhere.
            // (DXGI_SCALING_NONE is flip-model-only, so switch to STRETCH too.)
            char ebuf[96];
            sprintf_s(ebuf, "swapchain(d3d11):flipfailed hr=0x%08X - trying bitblt", (unsigned)res);
            HeartbeatLog(ebuf);
            m_bAllowTearing = false;
            swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            swap_chain_desc.Scaling = DXGI_SCALING_STRETCH;
            swap_chain_desc.Flags = 0;
            res = m_pFactory->CreateSwapChainForHwnd(m_pDevice.Get(), hWnd, &swap_chain_desc, nullptr, nullptr, &temp_swapchain);
        }
        if (FAILED(res)) {
            char ebuf[96];
            sprintf_s(ebuf, "swapchain(d3d11):bitbltfailed hr=0x%08X", (unsigned)res);
            HeartbeatLog(ebuf);
            return std::make_tuple(res, "CreateSwapChainForHwnd");
        }
        res = temp_swapchain->QueryInterface(IID_PPV_ARGS(&m_pSwapChain));
        if (FAILED(res))
            return std::make_tuple(res, "IDXGISwapChain1 -> IDXGISwapChain3");
    }

    DXGI_SWAP_CHAIN_DESC1 actual_swap_desc = {};
    res = m_pSwapChain->GetDesc1(&actual_swap_desc);
    if (FAILED(res))
        return std::make_tuple(res, "GetDesc1");
    m_iBufferWidth = actual_swap_desc.Width;
    m_iBufferHeight = actual_swap_desc.Height;

    m_pFactory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);

    // Render target views. A flip-model swap chain only exposes its current
    // back buffer until the first present; the other RTVs are created lazily
    // in PresentBackend once the remaining buffers are allocated.
    m_uFrameIndex = m_pSwapChain->GetCurrentBackBufferIndex();
    {
        ComPtr<ID3D11Texture2D> backbuffer;
        res = m_pSwapChain->GetBuffer(m_uFrameIndex, IID_PPV_ARGS(&backbuffer));
        if (FAILED(res))
            return std::make_tuple(res, "GetBuffer");
        res = m_pDevice->CreateRenderTargetView(backbuffer.Get(), nullptr, &m_pRTVs[m_uFrameIndex]);
        if (FAILED(res))
            return std::make_tuple(res, "CreateRenderTargetView");
    }

    // Depth buffer
    D3D11_TEXTURE2D_DESC depth_desc = {};
    depth_desc.Width = m_iBufferWidth;
    depth_desc.Height = m_iBufferHeight;
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    res = m_pDevice->CreateTexture2D(&depth_desc, nullptr, &m_pDepthBuffer);
    if (FAILED(res))
        return std::make_tuple(res, "CreateTexture2D (depth buffer)");
    res = m_pDevice->CreateDepthStencilView(m_pDepthBuffer.Get(), nullptr, &m_pDSV);
    if (FAILED(res))
        return std::make_tuple(res, "CreateDepthStencilView");

    D3D11_VIEWPORT viewport = {
        .TopLeftX = 0,
        .TopLeftY = 0,
        .Width = (float)m_iBufferWidth,
        .Height = (float)m_iBufferHeight,
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };
    m_Viewport = viewport;
    m_FullScissor = { 0, 0, m_iBufferWidth, m_iBufferHeight };

    // Screenshot staging texture (CPU readable, same format as the backbuffer)
    D3D11_TEXTURE2D_DESC staging_desc = {};
    staging_desc.Width = m_iBufferWidth;
    staging_desc.Height = m_iBufferHeight;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    res = m_pDevice->CreateTexture2D(&staging_desc, nullptr, &m_pScreenshotStaging);
    if (FAILED(res))
        return std::make_tuple(res, "CreateTexture2D (screenshot staging)");

    m_vScreenshotOutput.resize(m_iBufferWidth * m_iBufferHeight * 4);

    // Background texture
    D3D11_TEXTURE2D_DESC bg_tex_desc = {};
    bg_tex_desc.Width = m_iBufferWidth;
    bg_tex_desc.Height = m_iBufferHeight;
    bg_tex_desc.MipLevels = 1;
    bg_tex_desc.ArraySize = 1;
    bg_tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    bg_tex_desc.SampleDesc.Count = 1;
    bg_tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    res = m_pDevice->CreateTexture2D(&bg_tex_desc, nullptr, &m_pBackgroundTexture);
    if (FAILED(res))
        return std::make_tuple(res, "CreateTexture2D (background)");
    D3D11_SHADER_RESOURCE_VIEW_DESC bg_srv_desc = {};
    bg_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    bg_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    bg_srv_desc.Texture2D.MipLevels = 1;
    res = m_pDevice->CreateShaderResourceView(m_pBackgroundTexture.Get(), &bg_srv_desc, &m_pBackgroundSRV);
    if (FAILED(res))
        return std::make_tuple(res, "CreateShaderResourceView (background)");

    // Scale and upload background image
    UploadBackgroundBitmap();

    // Set the orthographic projection (same as the D3D12 backend)
    float L = 0;
    float R = m_iBufferWidth;
    float T = 0;
    float B = m_iBufferHeight;
    float mvp[4][4] = {
        { 2.0f/(R-L),   0.0f,           0.0f,       0.0f },
        { 0.0f,         2.0f/(T-B),     0.0f,       0.0f },
        { 0.0f,         0.0f,           0.5f,       0.0f },
        { (R+L)/(L-R),  (T+B)/(B-T),    0.5f,       1.0f },
    };
    memcpy(m_RootConstants.proj, mvp, sizeof(mvp));

    return std::make_tuple(S_OK, "");
}

HRESULT D3D11Renderer::ResetDevice() {
    HeartbeatLog("reset(d3d11):start");
    DestroyBlurResources();
    HeartbeatLog("reset(d3d11):destroyblur");
    auto res = CreateWindowDependentObjects(m_hWnd);
    HeartbeatLog("reset(d3d11):windowdep");
    if (FAILED(std::get<0>(res)))
        return std::get<0>(res);
    HeartbeatLog("reset(d3d11):blur-create-start");
    if (FAILED(CreateBlurResources())) {
        HeartbeatLog("reset(d3d11):createblurfail");
        return E_FAIL;
    }
    HeartbeatLog("reset(d3d11):done");

    if (m_pUnscaledBackground) {
        HeartbeatLog("bg(d3d11):reapply-start");
        if (m_fBGBlurSigma >= 0.5f && m_pBlurCS && m_pBGBufferA)
            SetBackgroundBlur(m_fBGBlurSigma);
        else
            UploadBackgroundBitmap();
        HeartbeatLog("bg(d3d11):reapply-done");
    }

    return S_OK;
}

void D3D11Renderer::ReleaseDeviceResources() {
    HeartbeatLog("release(d3d11):start");
    DestroyBlurResources();

    for (uint32_t i = 0; i < FrameCount; i++)
        m_pRTVs[i].Reset();
    m_pSwapChain.Reset();
    m_pDepthBuffer.Reset();
    m_pDSV.Reset();
    m_pIndexBuffer.Reset();
    m_pNoteBuffer.Reset();
    m_pNoteSRV.Reset();
    m_pStripNoteSRV.Reset();
    m_pRectVertexBuffer.Reset();
    m_pFixedBuffer.Reset();
    m_pFixedSRV.Reset();
    m_pTrackColorBuffer.Reset();
    m_pTrackColorSRV.Reset();
    m_pScreenshotStaging.Reset();
    m_pBackgroundTexture.Reset();
    m_pBackgroundSRV.Reset();

    m_pRectVS.Reset();
    m_pRectPS.Reset();
    m_pNoteVS.Reset();
    m_pNotePS.Reset();
    m_pBackgroundVS.Reset();
    m_pBackgroundPS.Reset();
    m_pChunkQuadVS.Reset();
    m_pChunkQuadPS.Reset();
    for (unsigned i = 0; i < ChunkPoolSize; i++) {
        m_pChunkTextures[i].Reset();
        m_pChunkRTVs[i].Reset();
        m_pChunkSRVs[i].Reset();
    }
    m_pChunkDepth.Reset();
    m_pChunkDSV.Reset();
    m_pChunkConstants.Reset();
    m_pChunkFixedBuffer.Reset();
    m_pChunkFixedSRV.Reset();
    m_pChunkQuadConstants.Reset();
    m_iChunkWidth = 0;
    m_iChunkHeight = 0;
    for (auto& entry : m_ChunkCache)
        entry.chunk = ImageBufferInvalidChunk;
    m_pBlurCS.Reset();
    m_pBloomExtractCS.Reset();
    m_pBloomPS.Reset();
    m_pRectInputLayout.Reset();
    m_pPerFrameConstants.Reset();
    m_pBackgroundConstants.Reset();
    m_pBlurConstants.Reset();
    m_pBloomExtractConstants.Reset();
    m_pBloomConstants.Reset();
    m_pBlendInverted.Reset();
    m_pBlendBloom.Reset();
    m_pRasterizer.Reset();
    m_pDepthDisabled.Reset();
    m_pDepthEnabled.Reset();
    m_pSamplerPointWrap.Reset();
    m_pSamplerLinearClamp.Reset();

    m_pContext.Reset();
    m_pContext4.Reset();
    m_pFence.Reset();
    if (m_hFenceEvent) {
        CloseHandle(m_hFenceEvent);
        m_hFenceEvent = NULL;
    }
    m_pDevice.Reset();
    m_uFrameIndex = 0;
    m_iBufferWidth = 0;
    m_iBufferHeight = 0;
    HeartbeatLog("release(d3d11):done");
}

HRESULT D3D11Renderer::ClearAndBeginScene(DWORD color) {
    m_vRectsIntermediate.clear();
    m_vNotesIntermediate.clear();
    m_vPianoRollStripNotesIntermediate.clear();
    m_iRectSplit = -1;

    UpdateWarpConstants();

    // Upload fixed size constants if they changed
    if (memcmp(&m_FixedConstants, &m_OldFixedConstants, sizeof(FixedSizeConstants))) {
        memcpy(&m_OldFixedConstants, &m_FixedConstants, sizeof(FixedSizeConstants));
        ImageBufferNotifyFixedChanged();
        m_pContext->UpdateSubresource(m_pFixedBuffer.Get(), 0, nullptr, &m_FixedConstants, 0, 0);
    }

    // Upload dirty track colors
    if (m_uTrackColorsDirtyBegin < m_uTrackColorsDirtyEnd) {
        const size_t beginBytes = m_uTrackColorsDirtyBegin * 16 * sizeof(TrackColor);
        const size_t bytes = (m_uTrackColorsDirtyEnd - m_uTrackColorsDirtyBegin) * 16 * sizeof(TrackColor);
        m_uTrackColorsDirtyBegin = SIZE_MAX;
        m_uTrackColorsDirtyEnd = 0;
        ImageBufferNotifyTrackColorsChanged();
        D3D11_BOX box = {
            (UINT)beginBytes, 0, 0,
            (UINT)(beginBytes + bytes), 1, 1,
        };
        m_pContext->UpdateSubresource(m_pTrackColorBuffer.Get(), 0, &box, (const uint8_t*)m_TrackColors + beginBytes, (UINT)bytes, 0);
    }

    // Fixed/color stamps now describe the data that will actually be baked.
    ImageBufferBeginFrame();

    SetPipeline(Pipeline::Rect);
    SetupCommandList();

    float float_color[4] = { (float)((color >> 16) & 0xFF) / 255.0f, (float)((color >> 8) & 0xFF) / 255.0f, (float)(color & 0xFF) / 255.0f, 1.0f };
    m_pContext->ClearDepthStencilView(m_pDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    m_pContext->ClearRenderTargetView(m_pRTVs[m_uFrameIndex].Get(), float_color);

    return S_OK;
}

HRESULT D3D11Renderer::EndScene(bool draw_bg) {
    ImGui::Render();
    ImGui::GetDrawData()->AddDrawList(m_pDrawList);

    if (draw_bg) {
        SetPipeline(Pipeline::Background);
        const BackgroundConstants bgConstants = {
            .fadeStart = 0.0f,
            .fadeEnd = 0.0f,
            .fadeEnabled = 0.0f,
            .padding = Config::GetConfig().GetVizSettings().fBGOpacity,
        };
        m_pContext->UpdateSubresource(m_pBackgroundConstants.Get(), 0, nullptr, &bgConstants, 0, 0);
        m_pContext->Draw(3, 0);
        SetPipeline(Pipeline::Rect);
    }

    HRESULT res = S_OK;
    auto rect_count = min(m_vRectsIntermediate.size(), RectsPerPass * 4);
    auto rect_split = min(m_iRectSplit < 0 ? rect_count : m_iRectSplit, RectsPerPass * 4);
    if (!m_vRectsIntermediate.empty()) {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        res = m_pContext->Map(m_pRectVertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(res))
            return res;
        memcpy(mapped.pData, m_vRectsIntermediate.data(), rect_count * sizeof(RectVertex));
        m_pContext->Unmap(m_pRectVertexBuffer.Get(), 0);

        UINT stride = sizeof(RectVertex);
        UINT offset = 0;
        m_pContext->IASetVertexBuffers(0, 1, m_pRectVertexBuffer.GetAddressOf(), &stride, &offset);
        m_pContext->IASetIndexBuffer(m_pIndexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        m_pContext->DrawIndexed((UINT)(rect_split / 4 * 6), 0, 0);
    }

    D3D11_MAPPED_SUBRESOURCE note_map = {};
    if (ImageBufferCanRender() && (!m_vChunkBuilds.empty() || !m_vChunkQuads.empty())) {
        res = RenderImageBuffer();
        if (FAILED(res))
            return res;
    }
    // Not else-if: fallback notes (image-buffer budget overflow) must still
    // draw through this path in the same frame.
    if (!m_vNotesIntermediate.empty()) {
        const size_t total = m_vNotesIntermediate.size();
        const size_t noteLimit = m_bUnlimitedNotes
            ? total
            : min(total, max((size_t)100, (size_t)m_NotesPerPass));
        const size_t startIdx = total - noteLimit;
        const size_t batchSize = MaxNotesPerPass;

        for (size_t i = startIdx; i < total; i += batchSize) {
            if (i == startIdx)
                SetPipeline(Pipeline::Note);

            auto remaining = total - i;
            auto note_count = min(remaining, batchSize);
            res = m_pContext->Map(m_pNoteBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &note_map);
            if (FAILED(res))
                return res;
            memcpy(note_map.pData, &m_vNotesIntermediate[i], note_count * sizeof(NoteData));
            m_pContext->Unmap(m_pNoteBuffer.Get(), 0);

            m_pContext->DrawIndexed((UINT)(note_count * 6), 0, 0);
        }
    }

    if (rect_count > rect_split) {
        SetPipeline(Pipeline::Rect);
        UINT stride = sizeof(RectVertex);
        UINT offset = 0;
        m_pContext->IASetVertexBuffers(0, 1, m_pRectVertexBuffer.GetAddressOf(), &stride, &offset);
        m_pContext->IASetIndexBuffer(m_pIndexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        m_pContext->DrawIndexed((UINT)((rect_count - rect_split) / 4 * 6), (UINT)(rect_split / 4 * 6), 0);
    }

    if (!g_bDisableBlur)
        ApplyBlur();

    const auto& bloomViz = Config::GetConfig().GetVizSettings();
    // "Disable all blurs" covers UI/background/roll blur only. Bloom is
    // an independent post effect and must remain controlled solely by bBloom.
    if (bloomViz.bBloom && m_pBloomPS && m_pBloomExtractCS && bloomViz.fBloomIntensity > 0.0f && m_pBloomHalf) {
        // Extract bright parts into the half-res buffer
        m_pContext->CSSetShader(m_pBloomExtractCS.Get(), nullptr, 0);
        m_pContext->CSSetConstantBuffers(0, 1, m_pBloomExtractConstants.GetAddressOf());
        float extractCB[4] = { bloomViz.fBloomBrightness * 0.4f, 0.1f, 2.5f, 0.0f };
        m_pContext->UpdateSubresource(m_pBloomExtractConstants.Get(), 0, nullptr, extractCB, 0, 0);
        ID3D11ShaderResourceView* sceneSRV = m_pSceneCopySRV.Get();
        ID3D11UnorderedAccessView* halfUAV = m_pBloomHalfUAV.Get();
        m_pContext->CSSetShaderResources(0, 1, &sceneSRV);
        m_pContext->CSSetUnorderedAccessViews(0, 1, &halfUAV, 0);
        UINT extract_gx = (m_iBloomHalfWidth + 15) / 16;
        UINT extract_gy = (m_iBloomHalfHeight + 15) / 16;
        m_pContext->Dispatch(extract_gx, extract_gy, 1);

        ID3D11ShaderResourceView* nullSRV = nullptr;
        ID3D11UnorderedAccessView* nullUAV = nullptr;
        m_pContext->CSSetShaderResources(0, 1, &nullSRV);
        m_pContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, 0);

        // Horizontal blur
        m_pContext->CSSetShader(m_pBlurCS.Get(), nullptr, 0);
        m_pContext->CSSetConstantBuffers(0, 1, m_pBlurConstants.GetAddressOf());
        const int sigma = max(1, (int)(bloomViz.fBloomSpread + 0.5f));
        UINT blurH[4] = { 0, (UINT)sigma, 0, 0 };
        m_pContext->UpdateSubresource(m_pBlurConstants.Get(), 0, nullptr, blurH, 0, 0);
        ID3D11ShaderResourceView* halfSRV = m_pBloomHalfSRV.Get();
        ID3D11UnorderedAccessView* tempUAV = m_pBloomBlurTempUAV.Get();
        m_pContext->CSSetShaderResources(0, 1, &halfSRV);
        m_pContext->CSSetUnorderedAccessViews(0, 1, &tempUAV, 0);
        UINT blur_gx = (m_iBloomHalfWidth + 255) / 256;
        m_pContext->Dispatch(blur_gx, m_iBloomHalfHeight, 1);
        m_pContext->CSSetShaderResources(0, 1, &nullSRV);
        m_pContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, 0);

        // Vertical blur
        UINT blurV[4] = { 1, (UINT)sigma, 0, 0 };
        m_pContext->UpdateSubresource(m_pBlurConstants.Get(), 0, nullptr, blurV, 0, 0);
        ID3D11ShaderResourceView* tempSRV = m_pBloomBlurTempSRV.Get();
        ID3D11UnorderedAccessView* resultUAV = m_pBloomBlurResultUAV.Get();
        m_pContext->CSSetShaderResources(0, 1, &tempSRV);
        m_pContext->CSSetUnorderedAccessViews(0, 1, &resultUAV, 0);
        UINT blur_gy = (m_iBloomHalfHeight + 255) / 256;
        m_pContext->Dispatch(m_iBloomHalfWidth, blur_gy, 1);
        m_pContext->CSSetShaderResources(0, 1, &nullSRV);
        m_pContext->CSSetUnorderedAccessViews(0, 1, &nullUAV, 0);

        // Composite bloom over the scene with the fullscreen triangle
        m_pContext->VSSetShader(m_pBackgroundVS.Get(), nullptr, 0);
        m_pContext->PSSetShader(m_pBloomPS.Get(), nullptr, 0);
        m_pContext->IASetInputLayout(nullptr);
        m_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT stride0 = 0, offset0 = 0;
        m_pContext->IASetVertexBuffers(0, 0, nullptr, &stride0, &offset0);
        m_pContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
        m_pContext->RSSetState(m_pRasterizer.Get());
        m_pContext->OMSetBlendState(m_pBlendBloom.Get(), nullptr, 0xFFFFFFFF);
        m_pContext->OMSetDepthStencilState(m_pDepthDisabled.Get(), 0);
        m_pContext->PSSetConstantBuffers(0, 1, m_pBloomConstants.GetAddressOf());
        ID3D11ShaderResourceView* bloomResultSRV = m_pBloomBlurResultSRV.Get();
        m_pContext->PSSetShaderResources(0, 1, &bloomResultSRV);
        m_pContext->PSSetSamplers(0, 1, m_pSamplerLinearClamp.GetAddressOf());

        float compositeCB[8] = { max(0.0f, bloomViz.fBloomSaturation), 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f };
        m_pContext->UpdateSubresource(m_pBloomConstants.Get(), 0, nullptr, compositeCB, 0, 0);
        float intensity = min(bloomViz.fBloomIntensity, 1.0f);
        float bf1[4] = { intensity, intensity, intensity, intensity };
        m_pContext->OMSetBlendState(m_pBlendBloom.Get(), bf1, 0xFFFFFFFF);
        m_pContext->Draw(3, 0);

        if (bloomViz.fRibbonBloomHeight > 0.0f && (bloomViz.fRibbonBloomIntensity * bloomViz.fRibbonBloomBrightness) > 0.0f && m_fRibbonCX > 0 && m_fRibbonCY > 0) {
            const int strips = max(1, min(bloomViz.iRibbonBloomSteps, 100));
            const float fadePad = bloomViz.fRibbonBloomHeight;
            const float ribBrightness = max(0.0f, bloomViz.fRibbonBloomIntensity * bloomViz.fRibbonBloomBrightness);
            const float totalH = m_fRibbonCY + fadePad * 2.0f;
            const float stripH = totalH / strips;

            float ribbonTint[3] = { 1.0f, 1.0f, 1.0f };
            if (bloomViz.bRibbonCustomColor) {
                ribbonTint[0] = ((bloomViz.dwRibbonBaseColor >> 16) & 0xFF) / 255.0f;
                ribbonTint[1] = ((bloomViz.dwRibbonBaseColor >> 8) & 0xFF) / 255.0f;
                ribbonTint[2] = (bloomViz.dwRibbonBaseColor & 0xFF) / 255.0f;
            }
            float ribbonCB[8] = { max(0.0f, bloomViz.fBloomSaturation), ribBrightness, 0.0f, 0.0f, ribbonTint[0], ribbonTint[1], ribbonTint[2], 0.0f };
            m_pContext->UpdateSubresource(m_pBloomConstants.Get(), 0, nullptr, ribbonCB, 0, 0);

            for (int s = 0; s < strips; s++) {
                float t = (s + 0.5f) / (float)strips;
                float alpha = 0.5f * (1.0f + cosf(3.14159265f * (2.0f * t - 1.0f)));
                if (alpha < 0.005f) continue;
                float bf[4] = { alpha, alpha, alpha, alpha };
                m_pContext->OMSetBlendState(m_pBlendBloom.Get(), bf, 0xFFFFFFFF);
                float stripY = m_fRibbonY - fadePad + s * stripH;
                D3D11_RECT scissor = {
                    (LONG)max(m_fRibbonX - fadePad * 2.0f, 0.0f),
                    (LONG)max(stripY, 0.0f),
                    (LONG)min(m_fRibbonX + m_fRibbonCX + fadePad * 2.0f, (float)m_iBufferWidth),
                    (LONG)min(stripY + stripH, (float)m_iBufferHeight),
                };
                m_pContext->RSSetScissorRects(1, &scissor);
                m_pContext->Draw(3, 0);
            }
            m_pContext->RSSetScissorRects(1, &m_FullScissor);
            float bfRestore[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            m_pContext->OMSetBlendState(m_pBlendBloom.Get(), bfRestore, 0xFFFFFFFF);
        }
    }

    DrawPianoRollStripBackground();
    DrawPianoRollStrip();

    const auto& vignetteViz = Config::GetConfig().GetVizSettings();
    if (vignetteViz.bVignette && vignetteViz.fVignetteIntensity > 0.0f && m_pVignettePS && m_pBlendVignette && m_pVignetteConstants) {
        m_pContext->VSSetShader(m_pBackgroundVS.Get(), nullptr, 0);
        m_pContext->PSSetShader(m_pVignettePS.Get(), nullptr, 0);
        m_pContext->IASetInputLayout(nullptr);
        m_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT stride0 = 0, offset0 = 0;
        m_pContext->IASetVertexBuffers(0, 0, nullptr, &stride0, &offset0);
        m_pContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
        m_pContext->RSSetState(m_pRasterizer.Get());
        m_pContext->OMSetBlendState(m_pBlendVignette.Get(), nullptr, 0xFFFFFFFF);
        m_pContext->OMSetDepthStencilState(m_pDepthDisabled.Get(), 0);
        m_pContext->PSSetConstantBuffers(0, 1, m_pVignetteConstants.GetAddressOf());
        m_pContext->RSSetScissorRects(1, &m_FullScissor);
        float vg[4] = { vignetteViz.fVignetteIntensity, (float)m_iBufferWidth / (float)max(1, m_iBufferHeight), vignetteViz.fVignetteWidth, 0.4f };
        m_pContext->UpdateSubresource(m_pVignetteConstants.Get(), 0, nullptr, vg, 0, 0);
        m_pContext->Draw(3, 0);
    }

    if (!Config::GetConfig().GetVizSettings().bDisableUI) {
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    return S_OK;
}

HRESULT D3D11Renderer::PresentBackend() {
    HRESULT res;
    if (m_bLimitFPS)
        res = m_pSwapChain->Present(1, 0);
    else {
        res = m_pSwapChain->Present(0, m_bAllowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    }
    if (res == DXGI_ERROR_DEVICE_REMOVED || res == DXGI_ERROR_DEVICE_RESET) {
        m_bDeviceLost = true;
        HeartbeatLog("present(d3d11):deviceremoved");
        return res;
    }
    if (FAILED(res))
        return res;

    const UINT64 cur_fence_value = m_uFenceValue;
    m_pContext4->Signal(m_pFence.Get(), m_uFenceValue);

    m_uFrameIndex = m_pSwapChain->GetCurrentBackBufferIndex();

    // A flip-model swap chain only guarantees the current back buffer right
    // after creation/resize; fill in the remaining RTVs now that the other
    // buffers are allocated.
    for (uint32_t i = 0; i < FrameCount; i++) {
        if (!m_pRTVs[i]) {
            ComPtr<ID3D11Texture2D> backbuffer;
            if (SUCCEEDED(m_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&backbuffer))))
                m_pDevice->CreateRenderTargetView(backbuffer.Get(), nullptr, &m_pRTVs[i]);
        }
    }

    auto tWaitStart = std::chrono::steady_clock::now();
    if (m_pFence->GetCompletedValue() < m_uFenceValue) {
        ResetEvent(m_hFenceEvent);
        res = m_pFence->SetEventOnCompletion(m_uFenceValue, m_hFenceEvent);
        if (FAILED(res))
            return res;

        while (WaitForSingleObjectEx(m_hFenceEvent, 1000, FALSE) == WAIT_TIMEOUT) {
            if (g_bGfxDestroyed)
                break;
        }
    }
    m_dPresentWaitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tWaitStart).count();

    m_uFenceValue = cur_fence_value + 1;
    return S_OK;
}

HRESULT D3D11Renderer::WaitForGPU() {
    if (g_bSkipGPUWait) {
        static bool logged = false;
        if (!logged) { HeartbeatLog("waitgpu(d3d11):skipped"); logged = true; }
        return S_OK;
    }

    m_pContext4->Signal(m_pFence.Get(), ++m_uFenceValue);

    if (m_pFence->GetCompletedValue() < m_uFenceValue) {
        ResetEvent(m_hFenceEvent);
        HRESULT res = m_pFence->SetEventOnCompletion(m_uFenceValue, m_hFenceEvent);
        if (FAILED(res))
            return res;
        while (WaitForSingleObjectEx(m_hFenceEvent, 1000, FALSE) == WAIT_TIMEOUT) {
            HRESULT reason = m_pDevice->GetDeviceRemovedReason();
            if (FAILED(reason)) {
                m_bDeviceLost = true;
                HeartbeatLog("waitgpu(d3d11):deviceremoved");
                return reason;
            }
            if (g_bGfxDestroyed)
                return E_ABORT;
        }
    }

    return S_OK;
}

std::wstring D3D11Renderer::GetAdapterName() {
    if (g_bForceWARP && !m_pAdapter)
        return L"WARP (software)";
    if (m_pAdapter) {
        DXGI_ADAPTER_DESC2 desc = {};
        if (FAILED(m_pAdapter->GetDesc2(&desc)))
            return L"GetDesc2 failed";
        return desc.Description;
    }
    return L"None";
}

bool D3D11Renderer::GetAdapterVideoMemory(DWORDLONG& used, DWORDLONG& total) {
    used = 0;
    total = 0;
    ComPtr<IDXGIAdapter3> adapter3;
    if (!m_pAdapter || FAILED(m_pAdapter.As(&adapter3)))
        return false;
    DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
    if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        return false;
    used = info.CurrentUsage;
    total = info.Budget;
    return true;
}

void D3D11Renderer::UploadPerFrameConstants() {
    // The shaders only declare proj + the 14 floats (120 bytes); the fMT pair
    // at the tail of RootConstants is ignored by them.
    D3D11RootConstants constants = {};
    memcpy(&constants, &m_RootConstants, min(sizeof(constants), sizeof(m_RootConstants)));
    m_pContext->UpdateSubresource(m_pPerFrameConstants.Get(), 0, nullptr, &constants, 0, 0);
}

void D3D11Renderer::SetPipeline(Pipeline pipeline) {
    switch (pipeline) {
    case Pipeline::Rect:
        m_pContext->VSSetShader(m_pRectVS.Get(), nullptr, 0);
        m_pContext->PSSetShader(m_pRectPS.Get(), nullptr, 0);
        m_pContext->IASetInputLayout(m_pRectInputLayout.Get());
        m_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_pContext->RSSetState(m_pRasterizer.Get());
        m_pContext->OMSetBlendState(m_pBlendInverted.Get(), nullptr, 0xFFFFFFFF);
        m_pContext->OMSetDepthStencilState(m_pDepthDisabled.Get(), 0);
        m_pContext->VSSetConstantBuffers(0, 1, m_pPerFrameConstants.GetAddressOf());
        UploadPerFrameConstants();
        break;
    case Pipeline::Note:
        m_pContext->VSSetShader(m_pNoteVS.Get(), nullptr, 0);
        m_pContext->PSSetShader(m_pNotePS.Get(), nullptr, 0);
        m_pContext->IASetInputLayout(nullptr);
        m_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_pContext->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
        m_pContext->IASetIndexBuffer(m_pIndexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        m_pContext->RSSetState(m_pRasterizer.Get());
        m_pContext->OMSetBlendState(m_pBlendInverted.Get(), nullptr, 0xFFFFFFFF);
        m_pContext->OMSetDepthStencilState(m_pDepthEnabled.Get(), 0);
        m_pContext->VSSetConstantBuffers(0, 1, m_pPerFrameConstants.GetAddressOf());
        m_pContext->PSSetConstantBuffers(0, 1, m_pPerFrameConstants.GetAddressOf());
        UploadPerFrameConstants();
        {
            // The shader expects fixed:t1, colors:t2, note_data:t3 (matches the
            // D3D12 root signature); slot t0 is unused.
            ID3D11ShaderResourceView* bindSRVs[] = { nullptr, m_pFixedSRV.Get(), m_pTrackColorSRV.Get(), m_pNoteSRV.Get() };
            m_pContext->VSSetShaderResources(0, 4, bindSRVs);
        }
        break;
    case Pipeline::Background:
        m_pContext->VSSetShader(m_pBackgroundVS.Get(), nullptr, 0);
        m_pContext->PSSetShader(m_pBackgroundPS.Get(), nullptr, 0);
        m_pContext->IASetInputLayout(nullptr);
        m_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_pContext->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
        m_pContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
        m_pContext->RSSetState(m_pRasterizer.Get());
        m_pContext->OMSetBlendState(m_pBlendInverted.Get(), nullptr, 0xFFFFFFFF);
        m_pContext->OMSetDepthStencilState(m_pDepthDisabled.Get(), 0);
        m_pContext->PSSetConstantBuffers(0, 1, m_pBackgroundConstants.GetAddressOf());
        ID3D11ShaderResourceView* bindSRVs[] = { m_pBackgroundSRV.Get() };
        m_pContext->PSSetShaderResources(0, 1, bindSRVs);
        m_pContext->PSSetSamplers(0, 1, m_pSamplerPointWrap.GetAddressOf());
        const BackgroundConstants backgroundConstants = {};
        m_pContext->UpdateSubresource(m_pBackgroundConstants.Get(), 0, nullptr, &backgroundConstants, 0, 0);
        break;
    }
}

void D3D11Renderer::SetupCommandList() {
    m_pContext->RSSetViewports(1, &m_Viewport);
    m_pContext->RSSetScissorRects(1, &m_FullScissor);
    ID3D11RenderTargetView* rtvs[1] = { m_pRTVs[m_uFrameIndex].Get() };
    m_pContext->OMSetRenderTargets(1, rtvs, m_pDSV.Get());
}

char* D3D11Renderer::Screenshot() {
    ID3D11Resource* backbuffer = nullptr;
    m_pRTVs[m_uFrameIndex]->GetResource(&backbuffer);

    m_pContext->CopyResource(m_pScreenshotStaging.Get(), backbuffer);
    backbuffer->Release();

    if (FAILED(WaitForGPU()))
        return nullptr;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_pContext->Map(m_pScreenshotStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return nullptr;
    for (int y = 0; y < m_iBufferHeight; y++)
        memcpy(&m_vScreenshotOutput[y * m_iBufferWidth * 4], (const char*)mapped.pData + y * mapped.RowPitch, m_iBufferWidth * 4);
    m_pContext->Unmap(m_pScreenshotStaging.Get(), 0);

    return m_vScreenshotOutput.data();
}

bool D3D11Renderer::UploadBackgroundBitmap() {
    if (!m_pUnscaledBackground)
        return false;

    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(s_pWICFactory->CreateBitmapScaler(&scaler)))
        return false;

    if (FAILED(scaler->Initialize(m_pUnscaledBackground.Get(), m_iBufferWidth, m_iBufferHeight, WICBitmapInterpolationModeHighQualityCubic)))
        return false;

    std::vector<BYTE> scaled;
    scaled.resize(m_iBufferWidth * m_iBufferHeight * 4);
    if (FAILED(scaler->CopyPixels(NULL, m_iBufferWidth * 4, scaled.size(), scaled.data())))
        return false;

    WaitForGPU();

    // Unbind the background texture so the update doesn't hit a bound resource
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    m_pContext->PSSetShaderResources(0, 2, nullSRVs);
    m_pContext->VSSetShaderResources(0, 2, nullSRVs);

    m_pContext->UpdateSubresource(m_pBackgroundTexture.Get(), 0, nullptr, scaled.data(), m_iBufferWidth * 4, 0);

    return true;
}

void D3D11Renderer::SetBackgroundBlur(float sigma) {
    m_fBGBlurSigma = sigma;
    if (!m_pUnscaledBackground)
        return;

    UploadBackgroundBitmap();

    if (sigma < 0.5f || !m_pBlurCS || !m_pBGBufferA)
        return;

    const UINT iSigma = (UINT)(sigma + 0.5f);

    WaitForGPU();

    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
    m_pContext->PSSetShaderResources(0, 2, nullSRVs);
    m_pContext->VSSetShaderResources(0, 2, nullSRVs);
    m_pContext->CSSetShaderResources(0, 2, nullSRVs);

    m_pContext->CSSetShader(m_pBlurCS.Get(), nullptr, 0);
    m_pContext->CSSetConstantBuffers(0, 1, m_pBlurConstants.GetAddressOf());

    UINT blur_consts[4] = { 0, iSigma, 0, 0 };
    m_pContext->UpdateSubresource(m_pBlurConstants.Get(), 0, nullptr, blur_consts, 0, 0);
    {
        ID3D11ShaderResourceView* bgSRV = m_pBackgroundSRV.Get();
        ID3D11UnorderedAccessView* bgAUAV = m_pBGBufferAUAV.Get();
        m_pContext->CSSetShaderResources(0, 1, &bgSRV);
        m_pContext->CSSetUnorderedAccessViews(0, 1, &bgAUAV, 0);
    }
    m_pContext->Dispatch((m_iBufferWidth + 255) / 256, m_iBufferHeight, 1);
    m_pContext->CSSetShaderResources(0, 1, nullSRVs);
    m_pContext->CSSetUnorderedAccessViews(0, 1, nullUAVs, 0);

    blur_consts[0] = 1;
    m_pContext->UpdateSubresource(m_pBlurConstants.Get(), 0, nullptr, blur_consts, 0, 0);
    {
        ID3D11ShaderResourceView* bgASRV = m_pBGBufferASRV.Get();
        ID3D11UnorderedAccessView* bgBUAV = m_pBGBufferBUAV.Get();
        m_pContext->CSSetShaderResources(0, 1, &bgASRV);
        m_pContext->CSSetUnorderedAccessViews(0, 1, &bgBUAV, 0);
    }
    m_pContext->Dispatch(m_iBufferWidth, (m_iBufferHeight + 255) / 256, 1);
    m_pContext->CSSetShaderResources(0, 1, nullSRVs);
    m_pContext->CSSetUnorderedAccessViews(0, 1, nullUAVs, 0);

    m_pContext->CopyResource(m_pBackgroundTexture.Get(), m_pBGBufferB.Get());

    WaitForGPU();
}

HRESULT D3D11Renderer::CreateBlurResources() {
    if (m_pBlurCS)
        DestroyBlurResources();

    HRESULT res;

    res = m_pDevice->CreateComputeShader(g_pBlurShader11, sizeof(g_pBlurShader11), nullptr, &m_pBlurCS);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    res = m_pDevice->CreateComputeShader(g_pBloomExtractShader11, sizeof(g_pBloomExtractShader11), nullptr, &m_pBloomExtractCS);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    res = m_pDevice->CreatePixelShader(g_pBloomPixelShader11, sizeof(g_pBloomPixelShader11), nullptr, &m_pBloomPS);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    res = m_pDevice->CreatePixelShader(g_pVignettePixelShader11, sizeof(g_pVignettePixelShader11), nullptr, &m_pVignettePS);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }

    // Scene copy / blur textures (full-res B8G8R8A8)
    ComPtr<ID3D11Texture2D> tex = nullptr;
    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = m_iBufferWidth;
    tex_desc.Height = m_iBufferHeight;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    res = m_pDevice->CreateTexture2D(&tex_desc, nullptr, &tex);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    m_pSceneCopy = tex;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    res = m_pDevice->CreateShaderResourceView(m_pSceneCopy.Get(), &srv_desc, &m_pSceneCopySRV);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }

    tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    // UAVs on B8G8R8A8_UNORM require FL 11.1; blur work textures use R8G8B8A8
    // (FL 11.0-safe) so older GPUs like the Intel HD 4400 can create them.
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    res = m_pDevice->CreateTexture2D(&tex_desc, nullptr, &m_pBlurTemp);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    res = m_pDevice->CreateShaderResourceView(m_pBlurTemp.Get(), &srv_desc, &m_pBlurTempSRV);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    res = m_pDevice->CreateUnorderedAccessView(m_pBlurTemp.Get(), nullptr, &m_pBlurTempUAV);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }

    res = m_pDevice->CreateTexture2D(&tex_desc, nullptr, &m_pBlurOutput);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    res = m_pDevice->CreateShaderResourceView(m_pBlurOutput.Get(), &srv_desc, &m_pBlurOutputSRV);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    res = m_pDevice->CreateUnorderedAccessView(m_pBlurOutput.Get(), nullptr, &m_pBlurOutputUAV);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    m_BlurTextureID = (ImTextureID)m_pBlurOutputSRV.Get();
    tex.Reset();

    // Background blur buffers (R8G8B8A8)
    D3D11_TEXTURE2D_DESC bg_tex_desc = {};
    bg_tex_desc.Width = m_iBufferWidth;
    bg_tex_desc.Height = m_iBufferHeight;
    bg_tex_desc.MipLevels = 1;
    bg_tex_desc.ArraySize = 1;
    bg_tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    bg_tex_desc.SampleDesc.Count = 1;
    bg_tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    res = m_pDevice->CreateTexture2D(&bg_tex_desc, nullptr, &m_pBGBufferA);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    res = m_pDevice->CreateTexture2D(&bg_tex_desc, nullptr, &m_pBGBufferB);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    D3D11_SHADER_RESOURCE_VIEW_DESC bg_srv_desc = {};
    bg_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    bg_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    bg_srv_desc.Texture2D.MipLevels = 1;
    res = m_pDevice->CreateShaderResourceView(m_pBGBufferA.Get(), &bg_srv_desc, &m_pBGBufferASRV);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    res = m_pDevice->CreateShaderResourceView(m_pBGBufferB.Get(), &bg_srv_desc, &m_pBGBufferBSRV);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    res = m_pDevice->CreateUnorderedAccessView(m_pBGBufferA.Get(), nullptr, &m_pBGBufferAUAV);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    res = m_pDevice->CreateUnorderedAccessView(m_pBGBufferB.Get(), nullptr, &m_pBGBufferBUAV);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }

    // Bloom buffers (half-res R16G16B16A16_FLOAT)
    m_iBloomHalfWidth = max(1, m_iBufferWidth / 2);
    m_iBloomHalfHeight = max(1, m_iBufferHeight / 2);
    D3D11_TEXTURE2D_DESC bloom_tex_desc = {};
    bloom_tex_desc.Width = m_iBloomHalfWidth;
    bloom_tex_desc.Height = m_iBloomHalfHeight;
    bloom_tex_desc.MipLevels = 1;
    bloom_tex_desc.ArraySize = 1;
    bloom_tex_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    bloom_tex_desc.SampleDesc.Count = 1;
    bloom_tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    res = m_pDevice->CreateTexture2D(&bloom_tex_desc, nullptr, &m_pBloomHalf);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    res = m_pDevice->CreateTexture2D(&bloom_tex_desc, nullptr, &m_pBloomBlurTemp);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    res = m_pDevice->CreateTexture2D(&bloom_tex_desc, nullptr, &m_pBloomBlurResult);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    D3D11_SHADER_RESOURCE_VIEW_DESC bloom_srv_desc = {};
    bloom_srv_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    bloom_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    bloom_srv_desc.Texture2D.MipLevels = 1;
    res = m_pDevice->CreateShaderResourceView(m_pBloomHalf.Get(), &bloom_srv_desc, &m_pBloomHalfSRV);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    res = m_pDevice->CreateShaderResourceView(m_pBloomBlurTemp.Get(), &bloom_srv_desc, &m_pBloomBlurTempSRV);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    res = m_pDevice->CreateShaderResourceView(m_pBloomBlurResult.Get(), &bloom_srv_desc, &m_pBloomBlurResultSRV);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    res = m_pDevice->CreateUnorderedAccessView(m_pBloomHalf.Get(), nullptr, &m_pBloomHalfUAV);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    res = m_pDevice->CreateUnorderedAccessView(m_pBloomBlurTemp.Get(), nullptr, &m_pBloomBlurTempUAV);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }
    res = m_pDevice->CreateUnorderedAccessView(m_pBloomBlurResult.Get(), nullptr, &m_pBloomBlurResultUAV);
    if (FAILED(res)) { char eb[160]; sprintf_s(eb, "blurfail:d3d11 L%d hr=0x%08X", __LINE__, (unsigned)res); HeartbeatLog(eb); return res; }

    return S_OK;
}

void D3D11Renderer::DestroyBlurResources() {
    m_pBlurCS.Reset();
    m_pBloomExtractCS.Reset();
    m_pBloomPS.Reset();
    m_pVignettePS.Reset();
    m_pSceneCopy.Reset();
    m_pSceneCopySRV.Reset();
    m_pBlurTemp.Reset();
    m_pBlurTempSRV.Reset();
    m_pBlurTempUAV.Reset();
    m_pBlurOutput.Reset();
    m_pBlurOutputSRV.Reset();
    m_pBlurOutputUAV.Reset();
    m_pBGBufferA.Reset();
    m_pBGBufferASRV.Reset();
    m_pBGBufferAUAV.Reset();
    m_pBGBufferB.Reset();
    m_pBGBufferBSRV.Reset();
    m_pBGBufferBUAV.Reset();
    m_pBloomHalf.Reset();
    m_pBloomHalfSRV.Reset();
    m_pBloomHalfUAV.Reset();
    m_pBloomBlurTemp.Reset();
    m_pBloomBlurTempSRV.Reset();
    m_pBloomBlurTempUAV.Reset();
    m_pBloomBlurResult.Reset();
    m_pBloomBlurResultSRV.Reset();
    m_pBloomBlurResultUAV.Reset();
    m_BlurTextureID = 0;
}

void D3D11Renderer::ApplyBlur() {
    if (g_bShowLoading || !m_pBlurCS || !m_pSceneCopy || !m_pBlurTemp || !m_pBlurOutput)
        return;

    ID3D11Resource* backbuffer = nullptr;
    m_pRTVs[m_uFrameIndex]->GetResource(&backbuffer);

    m_pContext->CopyResource(m_pSceneCopy.Get(), backbuffer);
    backbuffer->Release();

    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
    m_pContext->PSSetShaderResources(0, 2, nullSRVs);
    m_pContext->VSSetShaderResources(0, 2, nullSRVs);
    m_pContext->CSSetShaderResources(0, 2, nullSRVs);

    m_pContext->CSSetShader(m_pBlurCS.Get(), nullptr, 0);
    m_pContext->CSSetConstantBuffers(0, 1, m_pBlurConstants.GetAddressOf());

    const VizSettings& blurViz = Config::GetConfig().GetVizSettings();
    const int sigma = blurViz.bBloom ? max(1, (int)(blurViz.fBloomSpread + 0.5f)) : 10;
    UINT blur_consts[4] = { 0, (UINT)sigma, 0, 0 };
    m_pContext->UpdateSubresource(m_pBlurConstants.Get(), 0, nullptr, blur_consts, 0, 0);
    {
        ID3D11ShaderResourceView* sceneSRV = m_pSceneCopySRV.Get();
        ID3D11UnorderedAccessView* tempUAV = m_pBlurTempUAV.Get();
        m_pContext->CSSetShaderResources(0, 1, &sceneSRV);
        m_pContext->CSSetUnorderedAccessViews(0, 1, &tempUAV, 0);
    }
    UINT groups_x = (m_iBufferWidth + 255) / 256;
    m_pContext->Dispatch(groups_x, m_iBufferHeight, 1);
    m_pContext->CSSetShaderResources(0, 1, nullSRVs);
    m_pContext->CSSetUnorderedAccessViews(0, 1, nullUAVs, 0);

    blur_consts[0] = 1;
    m_pContext->UpdateSubresource(m_pBlurConstants.Get(), 0, nullptr, blur_consts, 0, 0);
    {
        ID3D11ShaderResourceView* tempSRV = m_pBlurTempSRV.Get();
        ID3D11UnorderedAccessView* outputUAV = m_pBlurOutputUAV.Get();
        m_pContext->CSSetShaderResources(0, 1, &tempSRV);
        m_pContext->CSSetUnorderedAccessViews(0, 1, &outputUAV, 0);
    }
    UINT groups_y = (m_iBufferHeight + 255) / 256;
    m_pContext->Dispatch(m_iBufferWidth, groups_y, 1);
    m_pContext->CSSetShaderResources(0, 1, nullSRVs);
    m_pContext->CSSetUnorderedAccessViews(0, 1, nullUAVs, 0);
}

void D3D11Renderer::DrawPianoRollStripBackground() {
    const auto& stripViz = Config::GetConfig().GetVizSettings();
    if (g_bDisableBlur || (!stripViz.bDualPianoRoll && !stripViz.bHorizontalPianoRoll) || !m_pBlurOutput)
        return;

    if (stripViz.bHorizontalPianoRoll) {
        const D3D11_RECT fullScissor = { 0, 0, m_iBufferWidth, m_iBufferHeight };
        const BackgroundConstants fullConstants = { .fadeStart = 0.0f, .fadeEnd = 0.0f, .fadeEnabled = 0.0f, .padding = 1.0f };
        SetPipeline(Pipeline::Background);
        m_pContext->UpdateSubresource(m_pBackgroundConstants.Get(), 0, nullptr, &fullConstants, 0, 0);
        ID3D11ShaderResourceView* blurSRV = m_pBlurOutputSRV.Get();
        m_pContext->PSSetShaderResources(0, 1, &blurSRV);
        m_pContext->RSSetScissorRects(1, &fullScissor);
        m_pContext->Draw(3, 0);
        return;
    }
    const float menuBarHeight = 20.0f;
    const float toolbarHeight = 35.0f;
    const float stripTop = menuBarHeight + toolbarHeight;
    const float stripH = max(190.0f, min((float)m_iBufferHeight * 0.45f, (float)m_iBufferHeight * 0.28f));
    const float stripBottom = stripTop + stripH;
    const float fadeBottom = min((float)m_iBufferHeight, stripBottom + 40.0f);
    const float fadeTop = max(0.0f, stripTop - 40.0f);
    const D3D11_RECT stripScissor = {
        0,
        (LONG)floor(stripTop),
        m_iBufferWidth,
        min(m_iBufferHeight, (LONG)ceil(fadeBottom)),
    };
    const D3D11_RECT topScissor = {
        0,
        (LONG)floor(fadeTop),
        m_iBufferWidth,
        (LONG)ceil(stripTop),
    };

    const BackgroundConstants backgroundConstants = {
        .fadeStart = stripBottom,
        .fadeEnd = fadeBottom,
        .fadeEnabled = 1.0f,
        .padding = 1.0f, // fade mode 1: alpha = smoothstep -> blur in the strip, fading out toward fade_end (bottom edge)
    };
    SetPipeline(Pipeline::Background);
    m_pContext->UpdateSubresource(m_pBackgroundConstants.Get(), 0, nullptr, &backgroundConstants, 0, 0);
    ID3D11ShaderResourceView* blurSRV = m_pBlurOutputSRV.Get();
    m_pContext->PSSetShaderResources(0, 1, &blurSRV);
    m_pContext->RSSetScissorRects(1, &stripScissor);
    m_pContext->Draw(3, 0);

    // Top-edge fade, symmetric to the bottom one: the blurred background fades
    // out over the 40 px band above the strip.
    const BackgroundConstants topConstants = {
        .fadeStart = fadeTop,
        .fadeEnd = stripTop,
        .fadeEnabled = 1.0f,
        .padding = 0.0f, // fade mode 0: alpha = 1 - smoothstep -> sharp above, blur fading in toward fade_start (top edge)
    };
    m_pContext->UpdateSubresource(m_pBackgroundConstants.Get(), 0, nullptr, &topConstants, 0, 0);
    m_pContext->PSSetShaderResources(0, 1, &blurSRV);
    m_pContext->RSSetScissorRects(1, &topScissor);
    m_pContext->Draw(3, 0);
    m_pContext->RSSetScissorRects(1, &m_FullScissor);
}

void D3D11Renderer::DrawPianoRollStrip() {
    const auto& stripViz = Config::GetConfig().GetVizSettings();
    const bool horizontal = stripViz.bHorizontalPianoRoll;
    if (!stripViz.bDualPianoRoll && !horizontal)
        return;

    const std::vector<NoteData>& stripNotes = !m_vPianoRollStripNotesIntermediate.empty()
        ? m_vPianoRollStripNotesIntermediate
        : m_vNotesIntermediate;
    if (stripNotes.empty())
        return;

    const size_t stripCount = m_bUnlimitedNotes
        ? stripNotes.size()
        : min(stripNotes.size(), (size_t)m_NotesPerPass);
    if (stripCount == 0)
        return;

    const size_t stride = (stripCount + StripNoteBudget - 1) / StripNoteBudget;
    const size_t sampledCount = (stripCount + stride - 1) / stride;

    const float menuBarHeight = 20.0f;
    const float toolbarHeight = 35.0f;
    const float stripTop = horizontal ? 0.0f : menuBarHeight + toolbarHeight;

    m_pContext->ClearDepthStencilView(m_pDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    const float stripH = horizontal ? (float)m_iBufferHeight : max(190.0f, min((float)m_iBufferHeight * 0.45f, (float)m_iBufferHeight * 0.28f));
    const float stripW = (float)m_iBufferWidth;
    const int nKeys = 128;
    static const int sSharpsBelow[12] = { 0, 0, 1, 1, 2, 2, 2, 3, 3, 4, 4, 5 };
    const int nWhiteKeys = nKeys - (nKeys / 12) * 5 - sSharpsBelow[nKeys % 12];
    const float keyH = stripH / (float)nKeys;
    const float whiteH = stripH / (float)nWhiteKeys;

    RootConstants savedRoot = m_RootConstants;
    m_RootConstants.deflate = max(1.0f, min(3.0f, round(keyH * 0.15f / 2.0f)));
    m_RootConstants.notes_y = stripTop;
    m_RootConstants.notes_cy = stripW;
    m_RootConstants.white_cx = whiteH;
    m_RootConstants.timespan = stripH;
    m_RootConstants.stripMode = 1.0f;
    m_RootConstants.stripTimeSpan = GetDualRollTimeSpan(savedRoot.timespan, savedRoot.notes_cy);

    // The note buffer is still mapped (single DISCARD per frame); write the
    // decimated strip notes after the main batches in the same region.
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_pContext->Map(m_pNoteBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;
    NoteData* notes = (NoteData*)mapped.pData;
    for (size_t s = 0; s < sampledCount; s++)
        notes[PianoRollStripNoteOffset + s] = stripNotes[s * stride];
    m_pContext->Unmap(m_pNoteBuffer.Get(), 0);

    SetPipeline(Pipeline::Note);
    // The strip SRV starts at PianoRollStripNoteOffset
    ID3D11ShaderResourceView* bindSRVs[] = { m_pFixedSRV.Get(), m_pTrackColorSRV.Get(), m_pStripNoteSRV.Get() };
    m_pContext->VSSetShaderResources(0, 3, bindSRVs);
    m_pContext->DrawIndexed((UINT)(sampledCount * 6), 0, 0);

    m_RootConstants = savedRoot;
    SetPipeline(Pipeline::Note);
}

HRESULT D3D11Renderer::EnsureChunkResources(int slot, int W, int H) {
    if (W <= 0 || H <= 0)
        return S_OK;
    if (m_pChunkTextures[slot]) {
        D3D11_TEXTURE2D_DESC desc;
        m_pChunkTextures[slot]->GetDesc(&desc);
        if (desc.Width == (UINT)W && desc.Height == (UINT)H)
            return S_OK;
        m_pChunkTextures[slot].Reset();
        m_pChunkRTVs[slot].Reset();
        m_pChunkSRVs[slot].Reset();
    }

    D3D11_TEXTURE2D_DESC tex_desc = {};
    tex_desc.Width = (UINT)W;
    tex_desc.Height = (UINT)H;
    tex_desc.MipLevels = 1;
    tex_desc.ArraySize = 1;
    tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    tex_desc.SampleDesc = { 1, 0 };
    tex_desc.Usage = D3D11_USAGE_DEFAULT;
    tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT res = m_pDevice->CreateTexture2D(&tex_desc, nullptr, &m_pChunkTextures[slot]);
    if (FAILED(res))
        return res;
    res = m_pDevice->CreateRenderTargetView(m_pChunkTextures[slot].Get(), nullptr, &m_pChunkRTVs[slot]);
    if (FAILED(res))
        return res;
    res = m_pDevice->CreateShaderResourceView(m_pChunkTextures[slot].Get(), nullptr, &m_pChunkSRVs[slot]);
    if (FAILED(res))
        return res;

    // Shared chunk depth buffer (all chunks share the same dims while cached)
    if (!m_pChunkDepth || m_iChunkWidth != W || m_iChunkHeight != H) {
        m_pChunkDepth.Reset();
        m_pChunkDSV.Reset();
        D3D11_TEXTURE2D_DESC depth_desc = {};
        depth_desc.Width = (UINT)W;
        depth_desc.Height = (UINT)H;
        depth_desc.MipLevels = 1;
        depth_desc.ArraySize = 1;
        depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
        depth_desc.SampleDesc = { 1, 0 };
        depth_desc.Usage = D3D11_USAGE_DEFAULT;
        depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        res = m_pDevice->CreateTexture2D(&depth_desc, nullptr, &m_pChunkDepth);
        if (FAILED(res))
            return res;
        res = m_pDevice->CreateDepthStencilView(m_pChunkDepth.Get(), nullptr, &m_pChunkDSV);
        if (FAILED(res))
            return res;
        m_iChunkWidth = W;
        m_iChunkHeight = H;
    }

    return S_OK;
}

HRESULT D3D11Renderer::RenderImageBuffer() {
    const int W = (int)round(m_RootConstants.notes_cx);
    const int H = (int)round(m_RootConstants.notes_cy);
    if (W <= 0 || H <= 0)
        return S_OK;

    for (const auto& req : m_vChunkBuilds) {
        const size_t offset = (size_t)req.noteOffset;
        const size_t count = (size_t)req.noteCount;
        if (offset > m_vChunkNotes.size() || count > m_vChunkNotes.size() - offset || count > MaxNotesPerPass) {
            char buf[192];
            sprintf_s(buf, "imgbuf:reject-range chunk=%lld off=%zu count=%zu total=%zu cap=%u", req.chunk, offset, count, m_vChunkNotes.size(), (unsigned)MaxNotesPerPass);
            HeartbeatLog(buf);
            ImageBufferInvalidate();
            return S_OK;
        }
    }

    // Chunk-relative root constants: [0,W]x[0,H] maps to NDC, no warp.
    D3D11RootConstants chunkRoot = {};
    memcpy(&chunkRoot, &m_RootConstants, min(sizeof(chunkRoot), sizeof(m_RootConstants)));
    chunkRoot.proj[0][0] = 2.0f / (float)W;  chunkRoot.proj[0][1] = 0; chunkRoot.proj[0][2] = 0; chunkRoot.proj[0][3] = 0;
    chunkRoot.proj[1][0] = 0; chunkRoot.proj[1][1] = -2.0f / (float)H; chunkRoot.proj[1][2] = 0; chunkRoot.proj[1][3] = 0;
    chunkRoot.proj[2][0] = 0; chunkRoot.proj[2][1] = 0; chunkRoot.proj[2][2] = 0.5f; chunkRoot.proj[2][3] = 0;
    chunkRoot.proj[3][0] = -1; chunkRoot.proj[3][1] = 1; chunkRoot.proj[3][2] = 0.5f; chunkRoot.proj[3][3] = 1;
    chunkRoot.notes_y = 0;
    chunkRoot.notes_cy = (float)H;
    chunkRoot.notes_x = 0;
    chunkRoot.notes_cx = (float)W;
    chunkRoot.stripMode = 0;
    chunkRoot.fWarp = 0;
    chunkRoot.fWarpTime = 0;
    chunkRoot.fWarpSeedX = 0;
    chunkRoot.fWarpSeedY = 0;

    FixedSizeConstants chunkFixed;
    ImageBufferBuildChunkFixed(chunkFixed);

    for (const auto& req : m_vChunkBuilds) {
        if (ImageBufferGetChunkSlot(req.chunk) >= 0)
            continue; // already baked this frame

        const int slot = ImageBufferAllocateSlot();
        if (slot < 0)
            break; // pool exhausted; drop the rest

        HRESULT res = EnsureChunkResources(slot, W, H);
        if (FAILED(res))
            return res;

        m_pContext->OMSetRenderTargets(1, m_pChunkRTVs[slot].GetAddressOf(), m_pChunkDSV.Get());
        D3D11_VIEWPORT chunk_viewport = { 0, 0, (float)W, (float)H, 0, 1 };
        m_pContext->RSSetViewports(1, &chunk_viewport);
        D3D11_RECT chunk_scissor = { 0, 0, W, H };
        m_pContext->RSSetScissorRects(1, &chunk_scissor);

        float clear_color[4] = { 0, 0, 0, 1 }; // alpha 1 = transparent under the inverted blend
        m_pContext->ClearRenderTargetView(m_pChunkRTVs[slot].Get(), clear_color);
        m_pContext->ClearDepthStencilView(m_pChunkDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

        SetPipeline(Pipeline::Note);
        m_pContext->UpdateSubresource(m_pChunkConstants.Get(), 0, nullptr, &chunkRoot, 0, 0);
        m_pContext->UpdateSubresource(m_pChunkFixedBuffer.Get(), 0, nullptr, &chunkFixed, 0, 0);
        m_pContext->VSSetConstantBuffers(0, 1, m_pChunkConstants.GetAddressOf());
        m_pContext->PSSetConstantBuffers(0, 1, m_pChunkConstants.GetAddressOf());

        // Chunk notes go into [0, count) of the note buffer (free in image buffer mode)
        if (req.noteCount > 0) {
            D3D11_MAPPED_SUBRESOURCE note_map = {};
            res = m_pContext->Map(m_pNoteBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &note_map);
            if (FAILED(res))
                return res;
            memcpy(note_map.pData, &m_vChunkNotes[req.noteOffset], (size_t)req.noteCount * sizeof(NoteData));
            m_pContext->Unmap(m_pNoteBuffer.Get(), 0);

            ID3D11ShaderResourceView* bindSRVs[] = { nullptr, m_pChunkFixedSRV.Get(), m_pTrackColorSRV.Get(), m_pNoteSRV.Get() };
            m_pContext->VSSetShaderResources(0, 4, bindSRVs);
            m_pContext->DrawIndexed((UINT)req.noteCount * 6, 0, 0);
        }

        ImageBufferMarkBaked(slot, req.chunk);
    }

    // Restore the main render target / viewport / scissor
    m_pContext->OMSetRenderTargets(1, m_pRTVs[m_uFrameIndex].GetAddressOf(), m_pDSV.Get());
    m_pContext->RSSetViewports(1, &m_Viewport);
    m_pContext->RSSetScissorRects(1, &m_FullScissor);

    // Draw the chunk quads
    if (!m_vChunkQuads.empty()) {
        struct ChunkQuadConstants {
            float proj[4][4];
            float quadPos[2];
            float quadSize[2];
        };
        ChunkQuadConstants quadConstants = {};
        memcpy(quadConstants.proj, m_RootConstants.proj, sizeof(quadConstants.proj));

        const float notesX = m_RootConstants.notes_x;
        const float notesY = m_RootConstants.notes_y;
        const float notesCX = m_RootConstants.notes_cx;
        const float notesCY = m_RootConstants.notes_cy;
        const D3D11_RECT notes_scissor = {
            (LONG)round(notesX), (LONG)round(notesY),
            (LONG)round(notesX + notesCX), (LONG)round(notesY + notesCY),
        };

        m_pContext->VSSetShader(m_pChunkQuadVS.Get(), nullptr, 0);
        m_pContext->PSSetShader(m_pChunkQuadPS.Get(), nullptr, 0);
        m_pContext->IASetInputLayout(nullptr);
        m_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_pContext->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
        m_pContext->IASetIndexBuffer(m_pIndexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        m_pContext->RSSetState(m_pRasterizer.Get());
        m_pContext->OMSetBlendState(m_pBlendInverted.Get(), nullptr, 0xFFFFFFFF);
        m_pContext->OMSetDepthStencilState(m_pDepthDisabled.Get(), 0);
        m_pContext->VSSetConstantBuffers(0, 1, m_pChunkQuadConstants.GetAddressOf());

        for (const auto& quad : m_vChunkQuads) {
            const int slot = ImageBufferGetChunkSlot(quad.chunk);
            if (slot < 0)
                continue; // not baked this frame (shouldn't happen)
            quadConstants.quadPos[0] = notesX;
            quadConstants.quadPos[1] = quad.yTop;
            quadConstants.quadSize[0] = notesCX;
            quadConstants.quadSize[1] = max(0.0f, quad.yBottom - quad.yTop);
            m_pContext->RSSetScissorRects(1, &notes_scissor);
            m_pContext->UpdateSubresource(m_pChunkQuadConstants.Get(), 0, nullptr, &quadConstants, 0, 0);
            m_pContext->PSSetShaderResources(0, 1, m_pChunkSRVs[slot].GetAddressOf());
            m_pContext->DrawIndexed(6, 0, 0);
        }

        // Restore bindings for any later pass that relies on the defaults
        m_pContext->VSSetConstantBuffers(0, 1, m_pPerFrameConstants.GetAddressOf());
        m_pContext->PSSetShaderResources(0, 1, nullptr);
    }

    // Chunk quads clip to notes_scissor. Keyboard and border rectangles are
    // post-split draws rendered after this pass, so restore the full viewport.
    m_pContext->RSSetScissorRects(1, &m_FullScissor);
    return S_OK;
}

void D3D11Renderer::ImGuiBackendNewFrame() {
    ImGui_ImplDX11_NewFrame();
}

void D3D11Renderer::ImGuiBackendShutdown() {
    ImGui_ImplDX11_Shutdown();
}

void D3D11Renderer::ImGuiBackendCreateDeviceObjects() {
    ImGui_ImplDX11_CreateDeviceObjects();
}