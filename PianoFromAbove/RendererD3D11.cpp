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
#include "ImageBufferMultipass.h"
#include <unordered_map>

static void MPD3D11OMSetRenderTargets(Renderer*, ID3D11Device*, ID3D11DeviceContext*, int, int,
    UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
static void MPD3D11ClearRenderTargetView(Renderer*, ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*);
static void MPD3D11ClearDepthStencilView(Renderer*, ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);

#define ImageBufferAllocateSlot() ImageBufferMPBeginBackend(this)
#define ImageBufferMarkBaked(slot, chunk) do { \
    if (ImageBufferMPRequestFinalize(this)) Renderer::ImageBufferMarkBaked((slot), (chunk)); \
    ImageBufferMPEndBackend(this); \
} while (0)
#define OMSetRenderTargets(...) GetType(), MPD3D11OMSetRenderTargets(this, m_pDevice.Get(), m_pContext.Get(), \
    (int)round(m_RootConstants.notes_cx), (int)round(m_RootConstants.notes_cy), __VA_ARGS__)
#define ClearRenderTargetView(...) GetType(), MPD3D11ClearRenderTargetView(this, m_pContext.Get(), __VA_ARGS__)
#define ClearDepthStencilView(...) GetType(), MPD3D11ClearDepthStencilView(this, m_pContext.Get(), __VA_ARGS__)

#include "RendererD3D11Legacy.inc"

#undef ClearDepthStencilView
#undef ClearRenderTargetView
#undef OMSetRenderTargets
#undef ImageBufferMarkBaked
#undef ImageBufferAllocateSlot

struct MPD3D11DepthPool {
    ID3D11Device* deviceTag = nullptr;
    ComPtr<ID3D11Texture2D> depth[Renderer::ChunkPoolSize];
    ComPtr<ID3D11DepthStencilView> dsv[Renderer::ChunkPoolSize];
    int width[Renderer::ChunkPoolSize] = {};
    int height[Renderer::ChunkPoolSize] = {};
    ID3D11DepthStencilView* activeDSV = nullptr;
};

static std::unordered_map<Renderer*, MPD3D11DepthPool>& MPD3D11Pools()
{
    static std::unordered_map<Renderer*, MPD3D11DepthPool> pools;
    return pools;
}

static ID3D11DepthStencilView* MPD3D11EnsureDepth(Renderer* renderer, ID3D11Device* device,
                                                  int slot, int W, int H)
{
    if (!device || slot < 0 || slot >= (int)Renderer::ChunkPoolSize || W <= 0 || H <= 0)
        return nullptr;

    auto& pool = MPD3D11Pools()[renderer];
    if (pool.deviceTag != device) {
        pool = MPD3D11DepthPool{};
        pool.deviceTag = device;
    }
    if (pool.depth[slot] && pool.dsv[slot] && pool.width[slot] == W && pool.height[slot] == H)
        return pool.dsv[slot].Get();

    pool.depth[slot].Reset();
    pool.dsv[slot].Reset();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = (UINT)W;
    desc.Height = (UINT)H;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    HRESULT hr = device->CreateTexture2D(&desc, nullptr, pool.depth[slot].ReleaseAndGetAddressOf());
    if (FAILED(hr))
        return nullptr;
    hr = device->CreateDepthStencilView(pool.depth[slot].Get(), nullptr, pool.dsv[slot].ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        pool.depth[slot].Reset();
        return nullptr;
    }

    pool.width[slot] = W;
    pool.height[slot] = H;
    return pool.dsv[slot].Get();
}

static void MPD3D11OMSetRenderTargets(Renderer* renderer, ID3D11Device* device, ID3D11DeviceContext* context,
    int W, int H, UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* originalDSV)
{
    if (!context)
        return;
    if (!ImageBufferMPBackendActive(renderer)) {
        context->OMSetRenderTargets(count, rtvs, originalDSV);
        return;
    }

    auto& pool = MPD3D11Pools()[renderer];
    pool.activeDSV = MPD3D11EnsureDepth(renderer, device, ImageBufferMPRequestSlot(renderer), W, H);
    context->OMSetRenderTargets(count, rtvs, pool.activeDSV ? pool.activeDSV : originalDSV);
}

static void MPD3D11ClearRenderTargetView(Renderer* renderer, ID3D11DeviceContext* context,
    ID3D11RenderTargetView* rtv, const FLOAT* color)
{
    if (!context)
        return;
    if (ImageBufferMPBackendActive(renderer) && !ImageBufferMPRequestClear(renderer))
        return;
    context->ClearRenderTargetView(rtv, color);
}

static void MPD3D11ClearDepthStencilView(Renderer* renderer, ID3D11DeviceContext* context,
    ID3D11DepthStencilView* originalDSV, UINT flags, FLOAT depth, UINT8 stencil)
{
    if (!context)
        return;
    if (!ImageBufferMPBackendActive(renderer)) {
        context->ClearDepthStencilView(originalDSV, flags, depth, stencil);
        return;
    }
    if (!ImageBufferMPRequestClear(renderer))
        return;

    auto& pool = MPD3D11Pools()[renderer];
    context->ClearDepthStencilView(pool.activeDSV ? pool.activeDSV : originalDSV, flags, depth, stencil);
}
