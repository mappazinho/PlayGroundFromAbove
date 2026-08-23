#include "d3dx12/d3dx12.h"
#ifdef _DEBUG
#include <dxgidebug.h>
#include <mmsystem.h>
#endif
#include "RectPixelShader.h"
#include "RectVertexShader.h"
#include "NotePixelShader.h"
#include "NoteVertexShader.h"
#include "BackgroundPixelShader.h"
#include "BackgroundVertexShader.h"
#include "ChunkQuadPixelShader.h"
#include "ChunkQuadVertexShader.h"
#include "Globals.h"
#include "Renderer.h"
#include <d3dcompiler.h>
#include <ShlObj.h>
#include <psapi.h>
#include "resource.h"
#include "Config.h"
#include "MIDI.h"
#include "MIDIPreRenderPlayer.h"
#include "BASSMIDI.h"
#include "GameState.h"
#include "MainProcs.h"
#include "imgui/imgui_impl_dx12.h"
#include "ImageBufferMultipass.h"
#include <unordered_map>

static void MPD3D12OMSetRenderTargets(Renderer*, ID3D12Device*, ID3D12GraphicsCommandList*, int, int,
    UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
static void MPD3D12ClearRenderTargetView(Renderer*, ID3D12GraphicsCommandList*,
    D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*);
static void MPD3D12ClearDepthStencilView(Renderer*, ID3D12GraphicsCommandList*,
    D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);

// The legacy backend still performs all normal D3D12 work. These narrow hooks
// substitute the persistent slot/depth state only while a multipass chunk pass
// is active.
#define ImageBufferAllocateSlot() ImageBufferMPBeginBackend(this)
#define ImageBufferMarkBaked(slot, chunk) do { \
    if (ImageBufferMPRequestFinalize(this)) Renderer::ImageBufferMarkBaked((slot), (chunk)); \
    ImageBufferMPEndBackend(this); \
} while (0)
#define OMSetRenderTargets(...) GetType(), MPD3D12OMSetRenderTargets(this, m_pDevice.Get(), m_pCommandList.Get(), \
    (int)round(m_RootConstants.notes_cx), (int)round(m_RootConstants.notes_cy), __VA_ARGS__)
#define ClearRenderTargetView(...) GetType(), MPD3D12ClearRenderTargetView(this, m_pCommandList.Get(), __VA_ARGS__)
#define ClearDepthStencilView(...) GetType(), MPD3D12ClearDepthStencilView(this, m_pCommandList.Get(), __VA_ARGS__)

#include "RendererD3D12Legacy.inc"

#undef ClearDepthStencilView
#undef ClearRenderTargetView
#undef OMSetRenderTargets
#undef ImageBufferMarkBaked
#undef ImageBufferAllocateSlot

struct MPD3D12DepthPool {
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT descriptorSize = 0;
    ComPtr<ID3D12Resource> depth[Renderer::ChunkPoolSize];
    D3D12_CPU_DESCRIPTOR_HANDLE dsv[Renderer::ChunkPoolSize] = {};
    int width[Renderer::ChunkPoolSize] = {};
    int height[Renderer::ChunkPoolSize] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE activeDSV = {};
};

static std::unordered_map<Renderer*, MPD3D12DepthPool>& MPD3D12Pools()
{
    static std::unordered_map<Renderer*, MPD3D12DepthPool> pools;
    return pools;
}

static D3D12_CPU_DESCRIPTOR_HANDLE MPD3D12EnsureDepth(Renderer* renderer, ID3D12Device* device,
                                                      int slot, int W, int H)
{
    D3D12_CPU_DESCRIPTOR_HANDLE none = {};
    if (!device || slot < 0 || slot >= (int)Renderer::ChunkPoolSize || W <= 0 || H <= 0)
        return none;

    auto& pool = MPD3D12Pools()[renderer];
    if (!pool.heap) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDesc.NumDescriptors = Renderer::ChunkPoolSize;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&pool.heap))))
            return none;
        pool.descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        D3D12_CPU_DESCRIPTOR_HANDLE start = pool.heap->GetCPUDescriptorHandleForHeapStart();
        for (unsigned i = 0; i < Renderer::ChunkPoolSize; ++i) {
            pool.dsv[i] = start;
            pool.dsv[i].ptr += (SIZE_T)i * pool.descriptorSize;
        }
    }

    if (pool.depth[slot] && pool.width[slot] == W && pool.height[slot] == H)
        return pool.dsv[slot];

    pool.depth[slot].Reset();
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = (UINT64)W;
    desc.Height = (UINT)H;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear = {};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;
    clear.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&pool.depth[slot]));
    if (FAILED(hr))
        return none;

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    device->CreateDepthStencilView(pool.depth[slot].Get(), &dsvDesc, pool.dsv[slot]);
    pool.width[slot] = W;
    pool.height[slot] = H;
    return pool.dsv[slot];
}

static void MPD3D12OMSetRenderTargets(Renderer* renderer, ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
    int W, int H, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL singleRange,
    const D3D12_CPU_DESCRIPTOR_HANDLE* originalDSV)
{
    if (!cmd)
        return;
    if (!ImageBufferMPBackendActive(renderer)) {
        cmd->OMSetRenderTargets(count, rtvs, singleRange, originalDSV);
        return;
    }

    auto& pool = MPD3D12Pools()[renderer];
    pool.activeDSV = MPD3D12EnsureDepth(renderer, device, ImageBufferMPRequestSlot(renderer), W, H);
    if (pool.activeDSV.ptr)
        cmd->OMSetRenderTargets(count, rtvs, singleRange, &pool.activeDSV);
    else
        cmd->OMSetRenderTargets(count, rtvs, singleRange, originalDSV);
}

static void MPD3D12ClearRenderTargetView(Renderer* renderer, ID3D12GraphicsCommandList* cmd,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT* color, UINT rectCount, const D3D12_RECT* rects)
{
    if (!cmd)
        return;
    if (ImageBufferMPBackendActive(renderer) && !ImageBufferMPRequestClear(renderer))
        return; // continuation pass: preserve the color accumulated earlier
    cmd->ClearRenderTargetView(rtv, color, rectCount, rects);
}

static void MPD3D12ClearDepthStencilView(Renderer* renderer, ID3D12GraphicsCommandList* cmd,
    D3D12_CPU_DESCRIPTOR_HANDLE originalDSV, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil,
    UINT rectCount, const D3D12_RECT* rects)
{
    if (!cmd)
        return;
    if (!ImageBufferMPBackendActive(renderer)) {
        cmd->ClearDepthStencilView(originalDSV, flags, depth, stencil, rectCount, rects);
        return;
    }
    if (!ImageBufferMPRequestClear(renderer))
        return; // continuation pass: preserve depth so LESS ordering spans passes

    auto& pool = MPD3D12Pools()[renderer];
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = pool.activeDSV.ptr ? pool.activeDSV : originalDSV;
    cmd->ClearDepthStencilView(dsv, flags, depth, stencil, rectCount, rects);
}
