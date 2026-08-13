/*************************************************************************************************
*
* File: Renderer.cpp
*
* Description: Implements the rendering objects. Just a wrapper to Direct3D.
*
* Copyright (c) 2010 Brian Pantano. All rights reserved.
*
*************************************************************************************************/
#include "d3dx12/d3dx12.h"
#ifdef _DEBUG
#include <dxgidebug.h>
#endif
#include "RectPixelShader.h"
#include "RectVertexShader.h"
#include "NotePixelShader.h"
#include "NoteVertexShader.h"
#include "BackgroundPixelShader.h"
#include "BackgroundVertexShader.h"
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

static HRESULT CompileBackgroundPS(ID3DBlob** ppBlob);

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

ComPtr<IWICImagingFactory> D3D12Renderer::s_pWICFactory;

D3D12Renderer::D3D12Renderer() {}

D3D12Renderer::~D3D12Renderer() {
    if (m_pCommandQueue && m_pFence && m_hFenceEvent) {
        m_pCommandQueue->Signal(m_pFence.Get(), ++m_pFenceValues[m_uFrameIndex]);
        ResetEvent(m_hFenceEvent);
        m_pFence->SetEventOnCompletion(m_pFenceValues[m_uFrameIndex], m_hFenceEvent);
        WaitForSingleObject(m_hFenceEvent, 3000);
    }

    DestroyBlurResources();

    if (m_hFenceEvent)
        CloseHandle(m_hFenceEvent);

#ifdef _DEBUG
    ComPtr<IDXGIDebug1> dxgi_debug = nullptr;
    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgi_debug))))
        dxgi_debug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_ALL);
#endif
}

std::tuple<HRESULT, const char*> D3D12Renderer::Init(HWND hWnd, bool bLimitFPS) {
    HRESULT res;
    // Create DXGI factory
#ifdef _DEBUG
    ID3D12Debug1* d3d12_debug = nullptr;
    res = D3D12GetDebugInterface(IID_PPV_ARGS(&d3d12_debug));
    if (FAILED(res))
        return std::make_tuple(res, "D3D12GetDebugInterface");
    d3d12_debug->EnableDebugLayer();
    d3d12_debug->SetEnableGPUBasedValidation(true);
    d3d12_debug->SetEnableSynchronizedCommandQueueValidation(true);
    d3d12_debug->Release();
#endif

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
        sprintf_s(buf, "init:limitfps=%d", (int)bLimitFPS);
        HeartbeatLog(buf);
    }
    IDXGIAdapter* adapter = nullptr;
    // std::vector<ComPtr<IDXGIAdapter1>> adapters;
    for (UINT i = 0; m_pFactory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        res = adapter->QueryInterface(IID_PPV_ARGS(&m_pAdapter));
        if (FAILED(res))
            continue;

        DXGI_ADAPTER_DESC2 desc = {};
        res = m_pAdapter->GetDesc2(&desc);
        if (FAILED(res))
            return std::make_tuple(res, "GetDesc2");

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        res = D3D12CreateDevice(m_pAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_pDevice));
        if (FAILED(res))
            continue;
            //return std::make_tuple(res, "D3D12CreateDevice");
        break;
    }
    if (m_pDevice == nullptr) {
        HeartbeatLog("init:nodevice");
        // During a TDR the driver can be unavailable for a few seconds; the
        if (!g_bInRecovery) {
            MessageBoxW(NULL, L"Couldn't find a suitable D3D12 device.", L"DirectX Error", MB_ICONERROR);
            exit(1);
        }
        return std::make_tuple(E_FAIL, "D3D12CreateDevice");
    }
    {
        // Which adapter actually got picked (rules out e.g. a virtual display
        // adapter being selected instead of the physical GPU).
        std::wstring wsAdapter = GetAdapterName();
        char buf[256];
        sprintf_s(buf, "init:adapter=%ls", wsAdapter.c_str());
        HeartbeatLog(buf);
    }

#ifdef _DEBUG
    ComPtr<ID3D12InfoQueue> info_queue;
    if (SUCCEEDED(m_pDevice->QueryInterface(IID_PPV_ARGS(&info_queue)))) {
        info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
    }
#endif

    D3D12_COMMAND_QUEUE_DESC queue_desc = {
        .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
        .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
        .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
        .NodeMask = 0,
    };
    res = m_pDevice->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&m_pCommandQueue));
    if (FAILED(res))
        return std::make_tuple(res, "CreateCommandQueue");

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = FrameCount,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        .NodeMask = 0,
    };
    res = m_pDevice->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&m_pRTVDescriptorHeap));
    if (FAILED(res))
        return std::make_tuple(res, "CreateDescriptorHeap (RTV)");
    m_uRTVDescriptorSize = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
        .NumDescriptors = 1,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
        .NodeMask = 0,
    };
    res = m_pDevice->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&m_pDSVDescriptorHeap));
    if (FAILED(res))
        return std::make_tuple(res, "CreateDescriptorHeap (DSV)");
    m_uDSVDescriptorSize = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = FrameCount + 3,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
        .NodeMask = 0,
    };
    res = m_pDevice->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&m_pSRVDescriptorHeap));
    if (FAILED(res))
        return std::make_tuple(res, "CreateDescriptorHeap (SRV)");
    m_uSRVDescriptorSize = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE srv_handle = m_pSRVDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < FrameCount; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {
            .Format = DXGI_FORMAT_UNKNOWN,
            .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
            .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
            .Buffer = {
                .FirstElement = 0,
                .NumElements = MaxNotesPerPass,
                .StructureByteStride = sizeof(NoteData),
                .Flags = D3D12_BUFFER_SRV_FLAG_NONE,
            }
        };

        m_pDevice->CreateShaderResourceView(m_pNoteBuffers[i].Get(), &srv_desc, srv_handle);
        srv_handle.ptr += m_uSRVDescriptorSize;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {
        .Format = DXGI_FORMAT_UNKNOWN,
        .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Buffer = {
            .FirstElement = 0,
            .NumElements = 1,
            .StructureByteStride = sizeof(FixedSizeConstants),
            .Flags = D3D12_BUFFER_SRV_FLAG_NONE,
        }
    };
    m_pDevice->CreateShaderResourceView(m_pGenericUpload.Get(), &srv_desc, srv_handle);
    srv_desc.Buffer.StructureByteStride = GenericUploadSize;
    srv_handle.ptr += m_uSRVDescriptorSize;
    m_pDevice->CreateShaderResourceView(m_pFixedBuffer.Get(), &srv_desc, srv_handle);
    srv_desc.Buffer.StructureByteStride = MaxTrackColors * 16 * sizeof(TrackColor);
    srv_handle.ptr += m_uSRVDescriptorSize;
    m_pDevice->CreateShaderResourceView(m_pTrackColorBuffer.Get(), &srv_desc, srv_handle);

    D3D12_DESCRIPTOR_HEAP_DESC imgui_srv_heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = 12,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
        .NodeMask = 0,
    };
    res = m_pDevice->CreateDescriptorHeap(&imgui_srv_heap_desc, IID_PPV_ARGS(&m_pImGuiSRVDescriptorHeap));
    if (FAILED(res))
        return std::make_tuple(res, "CreateDescriptorHeap (ImGui SRV)");

    D3D12_DESCRIPTOR_HEAP_DESC texture_srv_heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = 1,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
        .NodeMask = 0,
    };
    res = m_pDevice->CreateDescriptorHeap(&texture_srv_heap_desc, IID_PPV_ARGS(&m_pTextureSRVDescriptorHeap));
    if (FAILED(res))
        return std::make_tuple(res, "CreateDescriptorHeap (Texture SRV)");
    m_uTextureSRVDescriptorSize = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (uint32_t i = 0; i < FrameCount; i++) {
        res = m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_pCommandAllocator[i]));
        if (FAILED(res))
            return std::make_tuple(res, "CreateCommandAllocator (direct)");
    }

    // Create root signature
    ComPtr<ID3DBlob> rect_serialized;
    D3D12_ROOT_PARAMETER root_sig_params[] = {
        {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
            .Constants = {
                .ShaderRegister = 0,
                .RegisterSpace = 0,
                .Num32BitValues = sizeof(RootConstants) / 4,
            },
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL
        },
        // The rect shader doesn't actually use any of this, but I have to put it here because of Intel's shit iGPU drivers
        {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV,
            .Descriptor = {
                .ShaderRegister = 1,
                .RegisterSpace = 0,
            },
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX
        },
        {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV,
            .Descriptor = {
                .ShaderRegister = 2,
                .RegisterSpace = 0,
            },
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX
        },
        {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV,
            .Descriptor = {
                .ShaderRegister = 3,
                .RegisterSpace = 0,
            },
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX
        },
    };
    D3D12_ROOT_SIGNATURE_DESC root_sig_desc = {
        .NumParameters = _countof(root_sig_params),
        .pParameters = root_sig_params,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS,
    };
    res = D3D12SerializeRootSignature(&root_sig_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rect_serialized, nullptr);
    if (FAILED(res))
        return std::make_tuple(res, "D3D12SerializeRootSignature (rectangle)");
    res = m_pDevice->CreateRootSignature(0, rect_serialized->GetBufferPointer(), rect_serialized->GetBufferSize(), IID_PPV_ARGS(&m_pRectRootSignature));
    if (FAILED(res))
        return std::make_tuple(res, "CreateRootSignature (rectangle)");

    // Create rect pipeline
    D3D12_INPUT_ELEMENT_DESC rect_vertex_input[] = {
        {
            .SemanticName = "POSITION",
            .SemanticIndex = 0,
            .Format = DXGI_FORMAT_R32G32_FLOAT,
            .InputSlot = 0,
            .AlignedByteOffset = 0,
            .InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
            .InstanceDataStepRate = 0,
        },
        {
            .SemanticName = "COLOR",
            .SemanticIndex = 0,
            .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
            .InputSlot = 0,
            .AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT,
            .InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
            .InstanceDataStepRate = 0,
        },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC rect_pipeline_desc = {
        .pRootSignature = m_pRectRootSignature.Get(),
        .VS = {
            .pShaderBytecode = g_pRectVertexShader,
            .BytecodeLength = sizeof(g_pRectVertexShader),
        },
        .PS = {
            .pShaderBytecode = g_pRectPixelShader,
            .BytecodeLength = sizeof(g_pRectPixelShader),
        },
        .BlendState = {
            .AlphaToCoverageEnable = FALSE,
            .IndependentBlendEnable = FALSE,
            .RenderTarget = {
                {
                    // PFA is weird and inverts blending operations (0 is opaque, 255 is transparent)
                    .BlendEnable = TRUE,
                    .LogicOpEnable = FALSE,
                    .SrcBlend = D3D12_BLEND_INV_SRC_ALPHA,
                    .DestBlend = D3D12_BLEND_SRC_ALPHA,
                    .BlendOp = D3D12_BLEND_OP_ADD,
                    .SrcBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA,
                    .DestBlendAlpha = D3D12_BLEND_SRC_ALPHA,
                    .BlendOpAlpha = D3D12_BLEND_OP_ADD,
                    .LogicOp = D3D12_LOGIC_OP_NOOP,
                    .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
                }
            }
        },
        .SampleMask = UINT_MAX,
        .RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT),
        .DepthStencilState = {
            .DepthEnable = FALSE,
            .StencilEnable = FALSE,
        },
        .InputLayout = {
            .pInputElementDescs = rect_vertex_input,
            .NumElements = _countof(rect_vertex_input),
        },
        .IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED,
        .PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
        .NumRenderTargets = 1,
        .RTVFormats = {
            DXGI_FORMAT_B8G8R8A8_UNORM,
        },
        .DSVFormat = DXGI_FORMAT_D32_FLOAT,
        .SampleDesc = {
            .Count = 1
        },
    };
    res = m_pDevice->CreateGraphicsPipelineState(&rect_pipeline_desc, IID_PPV_ARGS(&m_pRectPipelineState));
    if (FAILED(res))
        return std::make_tuple(res, "CreateGraphicsPipelineState (rect)");
    
    // Create note root signature
    ComPtr<ID3DBlob> note_serialized;
    res = D3D12SerializeRootSignature(&root_sig_desc, D3D_ROOT_SIGNATURE_VERSION_1, &note_serialized, nullptr);
    if (FAILED(res))
        return std::make_tuple(res, "D3D12SerializeRootSignature (note)");
    res = m_pDevice->CreateRootSignature(0, note_serialized->GetBufferPointer(), note_serialized->GetBufferSize(), IID_PPV_ARGS(&m_pNoteRootSignature));
    if (FAILED(res))
        return std::make_tuple(res, "CreateRootSignature (note)");

    // Create note pipeline
    auto note_pipeline_desc = rect_pipeline_desc;
    note_pipeline_desc.pRootSignature = m_pNoteRootSignature.Get();
    note_pipeline_desc.VS = {
        .pShaderBytecode = g_pNoteVertexShader,
        .BytecodeLength = sizeof(g_pNoteVertexShader),
    };
    note_pipeline_desc.PS = {
        .pShaderBytecode = g_pNotePixelShader,
        .BytecodeLength = sizeof(g_pNotePixelShader),
    };
    note_pipeline_desc.DepthStencilState = {
        .DepthEnable = TRUE,
        .DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL,
        .DepthFunc = D3D12_COMPARISON_FUNC_LESS,
    };
    note_pipeline_desc.InputLayout = {
        .NumElements = 0,
    };
    res = m_pDevice->CreateGraphicsPipelineState(&note_pipeline_desc, IID_PPV_ARGS(&m_pNotePipelineState));
    if (FAILED(res))
        return std::make_tuple(res, "CreateGraphicsPipelineState (note)");

    // way; no separate strip PSO is created.

    D3D12_DESCRIPTOR_RANGE descriptor_ranges[] = {
        {
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 0,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = 0,
        }
    };
    ComPtr<ID3DBlob> background_serialized;
    D3D12_ROOT_PARAMETER background_root_sig_params[] = {
        {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
            .Constants = {
                .ShaderRegister = 0,
                .RegisterSpace = 0,
                .Num32BitValues = sizeof(BackgroundConstants) / 4,
            },
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
        },
        {
            .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
            .DescriptorTable = {
                .NumDescriptorRanges = 1,
                .pDescriptorRanges = descriptor_ranges,
            },
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
        },
    };
    D3D12_STATIC_SAMPLER_DESC background_root_sig_samplers[] = {
        {
            .Filter = D3D12_FILTER_MIN_MAG_MIP_POINT,
            .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
            .MipLODBias = 0,
            .MaxAnisotropy = 0,
            .ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
            .BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK,
            .MinLOD = 0,
            .MaxLOD = D3D12_FLOAT32_MAX,
            .ShaderRegister = 0,
            .RegisterSpace = 0,
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
        }
    };
    D3D12_ROOT_SIGNATURE_DESC background_root_sig_desc = {
        .NumParameters = _countof(background_root_sig_params),
        .pParameters = background_root_sig_params,
        .NumStaticSamplers = _countof(background_root_sig_samplers),
        .pStaticSamplers = background_root_sig_samplers,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS,
    };
    res = D3D12SerializeRootSignature(&background_root_sig_desc, D3D_ROOT_SIGNATURE_VERSION_1, &background_serialized, nullptr);
    if (FAILED(res))
        return std::make_tuple(res, "D3D12SerializeRootSignature (background)");
    res = m_pDevice->CreateRootSignature(0, background_serialized->GetBufferPointer(), background_serialized->GetBufferSize(), IID_PPV_ARGS(&m_pBackgroundRootSignature));
    if (FAILED(res))
        return std::make_tuple(res, "CreateRootSignature (background)");

    // Create background pipeline
    auto background_pipeline_desc = rect_pipeline_desc;
    background_pipeline_desc.pRootSignature = m_pBackgroundRootSignature.Get();
    background_pipeline_desc.VS = {
        .pShaderBytecode = g_pBackgroundVertexShader,
        .BytecodeLength = sizeof(g_pBackgroundVertexShader),
    };
    ComPtr<ID3DBlob> bg_ps_blob;
    if (SUCCEEDED(CompileBackgroundPS(&bg_ps_blob))) {
        background_pipeline_desc.PS = {
            .pShaderBytecode = bg_ps_blob->GetBufferPointer(),
            .BytecodeLength = bg_ps_blob->GetBufferSize(),
        };
    } else {
        background_pipeline_desc.PS = {
            .pShaderBytecode = g_pBackgroundPixelShader,
            .BytecodeLength = sizeof(g_pBackgroundPixelShader),
        };
    }
    background_pipeline_desc.InputLayout = {
        .NumElements = 0,
    };
    res = m_pDevice->CreateGraphicsPipelineState(&background_pipeline_desc, IID_PPV_ARGS(&m_pBackgroundPipelineState));
    if (FAILED(res))
        return std::make_tuple(res, "CreateGraphicsPipelineState (background)");

    //res = m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&m_pCommandList));
    //res = m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&m_pCommandList));
    res = m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_pCommandAllocator[m_uFrameIndex].Get(), nullptr, IID_PPV_ARGS(&m_pCommandList));
    if (FAILED(res))
        return std::make_tuple(res, "CreateCommandList");
    res = m_pCommandList->Close();
    if (FAILED(res))
        return std::make_tuple(res, "Closing command list");

    // Create synchronization fence
    res = m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_pFence));
    if (FAILED(res))
        return std::make_tuple(res, "CreateFence");
    m_pFenceValues[m_uFrameIndex]++;

    // Create synchronization fence event
    m_hFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Create generic upload buffer
    auto generic_upload_desc = CD3DX12_RESOURCE_DESC::Buffer(GenericUploadSize);
    auto upload_heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto default_heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    res = m_pDevice->CreateCommittedResource(
        &upload_heap,
        D3D12_HEAP_FLAG_NONE,
        &generic_upload_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_pGenericUpload)
    );
    if (FAILED(res))
        return std::make_tuple(res, "CreateCommittedResource (generic upload buffer)");

    // Create fixed size constants buffer
    auto fixed_desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(FixedSizeConstants));
    res = m_pDevice->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_NONE,
        &fixed_desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&m_pFixedBuffer)
    );
    if (FAILED(res))
        return std::make_tuple(res, "CreateCommittedResource (fixed buffer)");

    // Create track color buffer
    auto track_color_desc = CD3DX12_RESOURCE_DESC::Buffer(MaxTrackColors * 16 * sizeof(TrackColor));
    res = m_pDevice->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_NONE,
        &track_color_desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&m_pTrackColorBuffer)
    );
    if (FAILED(res))
        return std::make_tuple(res, "CreateCommittedResource (track color buffer)");

    // Create dynamic rect vertex buffers Each in-flight frame has its own vertex buffer
    auto vertex_buffer_desc = CD3DX12_RESOURCE_DESC::Buffer(RectsPerPass * 6 * sizeof(RectVertex));
    for (uint32_t i = 0; i < FrameCount; i++) {
        res = m_pDevice->CreateCommittedResource(
            &upload_heap,
            D3D12_HEAP_FLAG_NONE,
            &vertex_buffer_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_pVertexBuffers[i])
        );
        if (FAILED(res))
            return std::make_tuple(res, "CreateCommittedResource (vertex buffer)");
        m_pVertexBuffers[i]->SetName(L"Vertex buffer");
        m_VertexBufferViews[i].BufferLocation = m_pVertexBuffers[i]->GetGPUVirtualAddress();
        m_VertexBufferViews[i].SizeInBytes = vertex_buffer_desc.Width;
        m_VertexBufferViews[i].StrideInBytes = sizeof(RectVertex);
    }

    // One renderer-owned VBO per frame. The first half is the falling roll;
    auto note_buffer_desc = CD3DX12_RESOURCE_DESC::Buffer((UINT64)NoteBufferCapacity * sizeof(NoteData));
    for (uint32_t i = 0; i < FrameCount; i++) {
        res = m_pDevice->CreateCommittedResource(
            &upload_heap,
            D3D12_HEAP_FLAG_NONE,
            &note_buffer_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_pNoteBuffers[i])
        );
        if (FAILED(res))
            return std::make_tuple(res, "CreateCommittedResource (note buffer)");
        m_pNoteBuffers[i]->SetName(L"Note buffer");
    }

    // Create index buffer
    auto index_buffer_desc = CD3DX12_RESOURCE_DESC::Buffer(IndexBufferCount * sizeof(uint32_t));
    res = m_pDevice->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_NONE,
        &index_buffer_desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&m_pIndexBuffer)
    );
    if (FAILED(res))
        return std::make_tuple(res, "CreateCommittedResource (index buffer)");
    m_pIndexBuffer->SetName(L"Index buffer");
    m_IndexBufferView.BufferLocation = m_pIndexBuffer->GetGPUVirtualAddress();
    m_IndexBufferView.Format = DXGI_FORMAT_R32_UINT;
    m_IndexBufferView.SizeInBytes = index_buffer_desc.Width;

    // Create index upload buffer
    ComPtr<ID3D12Resource> index_buffer_upload = nullptr;
    res = m_pDevice->CreateCommittedResource(
        &upload_heap,
        D3D12_HEAP_FLAG_NONE,
        &index_buffer_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&index_buffer_upload)
    );
    if (FAILED(res))
        return std::make_tuple(res, "CreateCommittedResource (index upload buffer)");
    index_buffer_upload->SetName(L"Index upload buffer");

    // Generate index buffer data
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

    // Reset the command list
    m_pCommandAllocator[m_uFrameIndex]->Reset();
    m_pCommandList->Reset(m_pCommandAllocator[m_uFrameIndex].Get(), nullptr);

    // Upload index buffer to GPU
    D3D12_SUBRESOURCE_DATA index_buffer_data = {
        .pData = index_buffer_vec.data(),
        .RowPitch = (LONG_PTR)(index_buffer_vec.size() * sizeof(uint32_t)),
        .SlicePitch = (LONG_PTR)(index_buffer_vec.size() * sizeof(uint32_t)),
    };
    UpdateSubresources<1>(m_pCommandList.Get(), m_pIndexBuffer.Get(), index_buffer_upload.Get(), 0, 0, 1, &index_buffer_data);

    // Finalize index buffer
    auto index_buffer_barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pIndexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    m_pCommandList->ResourceBarrier(1, &index_buffer_barrier);

    // Close the command list
    res = m_pCommandList->Close();
    if (FAILED(res))
        return std::make_tuple(res, "Closing command list for initial buffer upload");

    // Execute the command list
    ID3D12CommandList* command_lists[] = { m_pCommandList.Get()};
    m_pCommandQueue->ExecuteCommandLists(1, command_lists);

    // Wait for everything to finish
    if (FAILED(WaitForGPU()))
        return std::make_tuple(res, "WaitForGPU");

    // Make the swap chain
    auto res2 = CreateWindowDependentObjects(hWnd);
    if (FAILED(std::get<0>(res2)))
        return res2;

    // Initialize ImGui
    auto imgui_heap = m_pImGuiSRVDescriptorHeap.Get();
    ImGui::CreateContext();

    // Update ImGui settings
    UpdateImGuiSettings();

    ImGui_ImplWin32_Init(hWnd);
    ImGui_ImplDX12_Init(m_pDevice.Get(), FrameCount, DXGI_FORMAT_B8G8R8A8_UNORM, imgui_heap, imgui_heap->GetCPUDescriptorHandleForHeapStart(), imgui_heap->GetGPUDescriptorHandleForHeapStart());

    if (m_pDrawList) {
        delete m_pDrawList;
    }
    m_pDrawList = new ImDrawList(ImGui::GetDrawListSharedData());

    if (FAILED(CreateBlurResources()))
        return std::make_tuple(E_FAIL, "CreateBlurResources");

    return std::make_tuple(S_OK, "");
}

std::tuple<HRESULT, const char*> D3D12Renderer::CreateWindowDependentObjects(HWND hWnd) {
    HRESULT res;
    if (m_pSwapChain) {
        HeartbeatLog("resize:gpuwait");
        if (FAILED(WaitForGPU()))
            // Device lost (TDR): skip ResizeBuffers on the dead device; the
            return std::make_tuple(E_FAIL, "WaitForGPU (device lost)");

        for (uint32_t i = 0; i < FrameCount; i++) {
            m_pRenderTargets[i].Reset();
            m_pFenceValues[i] = m_pFenceValues[m_uFrameIndex];
        }

        HeartbeatLog("resize:resizebuffers");
        // Resize the swap chain
        res = m_pSwapChain->ResizeBuffers(FrameCount, 0, 0, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
        if (FAILED(res)) {
            if (res == DXGI_ERROR_DEVICE_REMOVED || res == DXGI_ERROR_DEVICE_RESET) {
                m_bDeviceLost = true;
                HeartbeatLog("resize:deviceremoved");
            }
            return std::make_tuple(res, "ResizeBuffers");
        }
        HeartbeatLog("resize:afterresize");
    } else {
        // Create swap chain
        IDXGISwapChain1* temp_swapchain = nullptr;
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
            .Scaling = DXGI_SCALING_NONE,
            .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
            .AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
            .Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING,
        };
        res = m_pFactory->CreateSwapChainForHwnd(m_pCommandQueue.Get(), hWnd, &swap_chain_desc, nullptr, nullptr, &temp_swapchain);
        if (FAILED(res))
            return std::make_tuple(res, "CreateSwapChainForHwnd");
        res = temp_swapchain->QueryInterface(IID_PPV_ARGS(&m_pSwapChain));
        if (FAILED(res))
            return std::make_tuple(res, "IDXGISwapChain1 -> IDXGISwapChain3");
    }

    // Read backbuffer width and height TODO: Handle resizing
    DXGI_SWAP_CHAIN_DESC1 actual_swap_desc = {};
    res = m_pSwapChain->GetDesc1(&actual_swap_desc);
    if (FAILED(res))
        return std::make_tuple(res, "GetDesc1");
    m_iBufferWidth = actual_swap_desc.Width;
    m_iBufferHeight = actual_swap_desc.Height;

    // Disable ALT+ENTER TODO: Make fullscreen work
    m_pFactory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);

    // Create render target views
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = m_pRTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < FrameCount; i++) {
        res = m_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&m_pRenderTargets[i]));
        if (FAILED(res))
            return std::make_tuple(res, "GetBuffer");
        m_pDevice->CreateRenderTargetView(m_pRenderTargets[i].Get(), nullptr, rtv_handle);
        m_pRenderTargets[i]->SetName(L"Render target");
        rtv_handle.ptr += m_uRTVDescriptorSize;
    }

    // Create depth buffer view
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = m_pDSVDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    auto dsv_heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto dsv_res_desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_D32_FLOAT, m_iBufferWidth, m_iBufferHeight, 1, 0, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {
        .Format = DXGI_FORMAT_D32_FLOAT,
        .ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D,
        .Flags = D3D12_DSV_FLAG_NONE,
    };
    D3D12_CLEAR_VALUE clear_value = {
        .Format = DXGI_FORMAT_D32_FLOAT,
        .DepthStencil = {
            .Depth = 1.0f,
            .Stencil = 0,
        }
    };
    res = m_pDevice->CreateCommittedResource(
        &dsv_heap,
        D3D12_HEAP_FLAG_NONE,
        &dsv_res_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clear_value,
        IID_PPV_ARGS(&m_pDepthBuffer)
    );
    if (FAILED(res))
        return std::make_tuple(res, "CreateCommittedResource (depth buffer)");
    m_pDevice->CreateDepthStencilView(m_pDepthBuffer.Get(), &dsv_desc, dsv_handle);

    // Reset the current frame index
    m_uFrameIndex = m_pSwapChain->GetCurrentBackBufferIndex();

    // Get backbuffer pitch
    auto desc = m_pRenderTargets[m_uFrameIndex]->GetDesc();
    m_pDevice->GetCopyableFootprints(&desc, 0, 1, 0, nullptr, nullptr, &m_ullScreenshotPitch, nullptr);

    // Round up the pitch to a multiple of 256 Not sure if this is required, just got it from DirectXTK12 ScreenGrab.cpp
    m_ullScreenshotPitch = (m_ullScreenshotPitch + 255) & ~0xFFu;

    // Create screenshot staging buffer
    CD3DX12_HEAP_PROPERTIES readback_heap(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC screenshot_staging_desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
        .Alignment = 0,
        .Width = m_ullScreenshotPitch * m_iBufferHeight,
        .Height = 1,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_UNKNOWN,
        .SampleDesc = {
            .Count = 1,
        },
        .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    res = m_pDevice->CreateCommittedResource(
        &readback_heap,
        D3D12_HEAP_FLAG_NONE,
        &screenshot_staging_desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_pScreenshotStaging)
    );
    if (FAILED(res))
        return std::make_tuple(res, "CreateCommittedResource (screenshot staging buffer)");
    m_pScreenshotStaging->SetName(L"Screenshot staging buffer");

    // Resize screenshot target buffer
    m_vScreenshotOutput.resize(m_iBufferWidth * m_iBufferHeight * 4);

    // Create background image texture
    CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC texture_desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = (UINT64)m_iBufferWidth,
        .Height = (UINT)m_iBufferHeight,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = {
            .Count = 1,
        },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };
    res = m_pDevice->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_NONE,
        &texture_desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        nullptr,
        IID_PPV_ARGS(&m_pTextureBuffer)
    );
    if (FAILED(res))
        return std::make_tuple(res, "CreateCommittedResource (background texture)");
    m_pTextureBuffer->SetName(L"Background texture");

    // Create texture SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC texture_view_desc = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D {
            .MipLevels = 1,
        }
    };
    m_pDevice->CreateShaderResourceView(m_pTextureBuffer.Get(), &texture_view_desc, m_pTextureSRVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    // Create background image upload buffer
    UINT64 texture_upload_size = 0;
    m_pDevice->GetCopyableFootprints(&texture_desc, 0, 1, 0, NULL, NULL, NULL, &texture_upload_size);
    CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    auto texture_upload_desc = CD3DX12_RESOURCE_DESC::Buffer(texture_upload_size);
    res = m_pDevice->CreateCommittedResource(
        &upload_heap,
        D3D12_HEAP_FLAG_NONE,
        &texture_upload_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_pTextureUpload)
    );
    if (FAILED(res))
        return std::make_tuple(res, "CreateCommittedResource (background texture upload buffer)");
    m_pTextureUpload->SetName(L"Background texture upload buffer");

    // Scale and upload background image
    UploadBackgroundBitmap();

    // Set up root constants https://github.com/ocornut/imgui/blob/master/backends/imgui_impl_dx12.cpp#L99
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

HRESULT D3D12Renderer::ResetDeviceIfNeeded() {
    // TODO
    return S_OK;
}

HRESULT D3D12Renderer::ResetDevice() {
    HeartbeatLog("reset:start");
    DestroyBlurResources();
    HeartbeatLog("reset:destroyblur");
    auto res = CreateWindowDependentObjects(m_hWnd);
    HeartbeatLog("reset:windowdep");
    if (FAILED(std::get<0>(res)))
        return std::get<0>(res);
    HeartbeatLog("blur:create-start");
    if (FAILED(CreateBlurResources())) {
        HeartbeatLog("reset:createblurfail");
        return E_FAIL;
    }
    HeartbeatLog("reset:done");

    if (m_pUnscaledBackground)
    {
        HeartbeatLog("bg:reapply-start");
        if (m_fBGBlurSigma >= 0.5f && m_pBlurPipelineState && m_pBGBlurHeap)
            SetBackgroundBlur(m_fBGBlurSigma);
        else
            UploadBackgroundBitmap();
        HeartbeatLog("bg:reapply-done");
    }

    return S_OK;
}

void D3D12Renderer::ReleaseDeviceResources() {
    HeartbeatLog("release:start");
    DestroyBlurResources();

    for (uint32_t i = 0; i < FrameCount; i++)
        m_pRenderTargets[i].Reset();
    m_pSwapChain.Reset();
    m_pDepthBuffer.Reset();
    m_pCommandList.Reset();
    for (uint32_t i = 0; i < FrameCount; i++)
        m_pCommandAllocator[i].Reset();
    m_pIndexBuffer.Reset();
    for (uint32_t i = 0; i < FrameCount; i++)
        m_pVertexBuffers[i].Reset();
    for (uint32_t i = 0; i < FrameCount; i++)
        m_pNoteBuffers[i].Reset();
    m_pGenericUpload.Reset();
    m_pFixedBuffer.Reset();
    m_pTrackColorBuffer.Reset();
    m_pScreenshotStaging.Reset();
    m_pTextureUpload.Reset();
    m_pTextureBuffer.Reset();

    m_pRectRootSignature.Reset();
    m_pRectPipelineState.Reset();
    m_pNoteRootSignature.Reset();
    m_pNotePipelineState.Reset();
    m_pBackgroundRootSignature.Reset();
    m_pBackgroundPipelineState.Reset();
    m_pBloomRootSignature.Reset();
    m_pBloomPipelineState.Reset();
    m_pBloomExtractRootSignature.Reset();
    m_pBloomExtractPipelineState.Reset();
    m_pBlurRootSignature.Reset();
    m_pBlurPipelineState.Reset();
    m_pCompositePipelineState.Reset();
    m_pSceneCopy.Reset();
    m_pBlurTemp.Reset();
    m_pBlurOutput.Reset();
    m_pBloomHalf.Reset();
    m_pBloomBlurTemp.Reset();
    m_pBloomBlurResult.Reset();

    m_pRTVDescriptorHeap.Reset();
    m_pDSVDescriptorHeap.Reset();
    m_pSRVDescriptorHeap.Reset();
    m_pImGuiSRVDescriptorHeap.Reset();
    m_pTextureSRVDescriptorHeap.Reset();

    m_pFence.Reset();
    if (m_hFenceEvent) {
        CloseHandle(m_hFenceEvent);
        m_hFenceEvent = NULL;
    }
    m_pCommandQueue.Reset();
    m_pDevice.Reset();
    m_uFrameIndex = 0;
    for (uint32_t i = 0; i < FrameCount; i++)
        m_pFenceValues[i] = 0;
    m_iBufferWidth = 0;
    m_iBufferHeight = 0;
    m_BlurTempState = D3D12_RESOURCE_STATE_COMMON;
    m_BlurOutputState = D3D12_RESOURCE_STATE_COMMON;
    m_SceneCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
    m_BloomHalfState = D3D12_RESOURCE_STATE_COMMON;
    m_BloomBlurTempState = D3D12_RESOURCE_STATE_COMMON;
    m_BloomBlurResultState = D3D12_RESOURCE_STATE_COMMON;
    HeartbeatLog("release:done");
}

HRESULT D3D12Renderer::RecoverDevice(HWND hWnd, bool bLimitFPS) {
    HeartbeatLog("recover:start");
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ReleaseDeviceResources();
    m_bDeviceLost = false;
    g_bInRecovery = true;
    auto res = Init(hWnd, bLimitFPS);
    g_bInRecovery = false;
    HeartbeatLog("recover:done");
    return std::get<0>(res);
}

HRESULT D3D12Renderer::ClearAndBeginScene(DWORD color) {
    m_vRectsIntermediate.clear();
    m_vNotesIntermediate.clear();
    m_vPianoRollStripNotesIntermediate.clear();
    m_iRectSplit = -1;

    auto c_t0 = std::chrono::steady_clock::now();
    m_pCommandAllocator[m_uFrameIndex]->Reset();
    auto c_t1 = std::chrono::steady_clock::now();
    m_pCommandList->Reset(m_pCommandAllocator[m_uFrameIndex].Get(), m_pRectPipelineState.Get());
    auto c_t2 = std::chrono::steady_clock::now();

    SetPipeline(Pipeline::Rect);
    auto c_t3 = std::chrono::steady_clock::now();
    SetupCommandList();
    auto c_t4 = std::chrono::steady_clock::now();
    {
        static int s_clearLog = 0;
        static double cAlloc = 0, cList = 0, cPipe = 0, cSetup = 0, cRest = 0;
        auto c_t5 = std::chrono::steady_clock::now();
        cAlloc += std::chrono::duration<double, std::milli>(c_t1 - c_t0).count();
        cList += std::chrono::duration<double, std::milli>(c_t2 - c_t1).count();
        cPipe += std::chrono::duration<double, std::milli>(c_t3 - c_t2).count();
        cSetup += std::chrono::duration<double, std::milli>(c_t4 - c_t3).count();
        if ((s_clearLog++ & 127) == 127) {
            char buf[128];
            sprintf_s(buf, "c:alloc=%.2f list=%.2f pipe=%.2f setup=%.2f",
                cAlloc / 128.0, cList / 128.0, cPipe / 128.0, cSetup / 128.0);
            HeartbeatLog(buf);
            cAlloc = cList = cPipe = cSetup = 0;
        }
    }
    if (memcmp(&m_FixedConstants, &m_OldFixedConstants, sizeof(FixedSizeConstants))) {
        memcpy(&m_OldFixedConstants, &m_FixedConstants, sizeof(FixedSizeConstants));

        auto fixed_barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pFixedBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        m_pCommandList->ResourceBarrier(1, &fixed_barrier);

        D3D12_SUBRESOURCE_DATA fixed_upload_data = {
            .pData = &m_FixedConstants,
            .RowPitch = (LONG_PTR)(sizeof(m_FixedConstants)),
            .SlicePitch = (LONG_PTR)(sizeof(m_FixedConstants)),
        };
        UpdateSubresources(m_pCommandList.Get(), m_pFixedBuffer.Get(), m_pGenericUpload.Get(), 0, 0, 1, &fixed_upload_data);

        fixed_barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pFixedBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_pCommandList->ResourceBarrier(1, &fixed_barrier);
    }
    if (m_uTrackColorsDirtyBegin < m_uTrackColorsDirtyEnd) {
        const size_t beginBytes = m_uTrackColorsDirtyBegin * 16 * sizeof(TrackColor);
        const size_t bytes = (m_uTrackColorsDirtyEnd - m_uTrackColorsDirtyBegin) * 16 * sizeof(TrackColor);
        const UINT64 uploadOffset = sizeof(m_FixedConstants) + beginBytes;
        m_uTrackColorsDirtyBegin = SIZE_MAX;
        m_uTrackColorsDirtyEnd = 0;

        auto fixed_barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pTrackColorBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        m_pCommandList->ResourceBarrier(1, &fixed_barrier);

        void* pUpload = nullptr;
        m_pGenericUpload->Map(0, nullptr, &pUpload);
        memcpy((uint8_t*)pUpload + uploadOffset, (const uint8_t*)m_TrackColors + beginBytes, bytes);
        m_pGenericUpload->Unmap(0, nullptr);
        m_pCommandList->CopyBufferRegion(m_pTrackColorBuffer.Get(), beginBytes, m_pGenericUpload.Get(), uploadOffset, bytes);

        fixed_barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pTrackColorBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_pCommandList->ResourceBarrier(1, &fixed_barrier);
    }

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pRenderTargets[m_uFrameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_pCommandList->ResourceBarrier(1, &barrier);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(m_pRTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), m_uFrameIndex, m_uRTVDescriptorSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(m_pDSVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    float float_color[4] = { (float)((color >> 16) & 0xFF) / 255.0f, (float)((color >> 8) & 0xFF) / 255.0f, (float)(color & 0xFF) / 255.0f, 1.0f };
    m_pCommandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    m_pCommandList->ClearRenderTargetView(rtv, float_color, 0, nullptr);

    return S_OK;
}

HRESULT D3D12Renderer::EndScene(bool draw_bg) {
    auto s_t0 = std::chrono::steady_clock::now();
    ImGui::Render();
    ImGui::GetDrawData()->AddDrawList(m_pDrawList);
    auto s_t1 = std::chrono::steady_clock::now();

    if (draw_bg) {
        SetPipeline(Pipeline::Background);
        const BackgroundConstants bgConstants = {
            .fadeStart = 0.0f,
            .fadeEnd = 0.0f,
            .fadeEnabled = 0.0f,
            .padding = Config::GetConfig().GetVizSettings().fBGOpacity,
        };
        m_pCommandList->SetGraphicsRoot32BitConstants(0, sizeof(bgConstants) / 4, &bgConstants, 0);
        m_pCommandList->DrawIndexedInstanced(3, 1, 0, 0, 0);
        SetPipeline(Pipeline::Rect);
        m_pCommandList->IASetVertexBuffers(0, 1, &m_VertexBufferViews[m_uFrameIndex]);
    }

    HRESULT res = S_OK;
    auto rect_count = min(m_vRectsIntermediate.size(), RectsPerPass * 4);
    auto rect_split = min(m_iRectSplit < 0 ? rect_count : m_iRectSplit, RectsPerPass * 4);
    if (!m_vRectsIntermediate.empty()) {
        D3D12_RANGE rect_range = {
            .Begin = 0,
            .End = rect_count * sizeof(RectVertex),
        };
        RectVertex* vertices = nullptr;
        res = m_pVertexBuffers[m_uFrameIndex]->Map(0, &rect_range, (void**)&vertices);
        if (FAILED(res))
            return res;
        memcpy(vertices, m_vRectsIntermediate.data(), rect_count * sizeof(RectVertex));
        m_pVertexBuffers[m_uFrameIndex]->Unmap(0, &rect_range);

        m_pCommandList->DrawIndexedInstanced(rect_split / 4 * 6, 1, 0, 0, 0);
    }

    if (!m_vNotesIntermediate.empty()) {
        const size_t total = m_vNotesIntermediate.size();
        const size_t noteLimit = m_bUnlimitedNotes
            ? total
            : min(total, max((size_t)100, (size_t)m_NotesPerPass));
        const size_t startIdx = total - noteLimit;
        const size_t batchSize = MaxNotesPerPass;
        {
            static int s_noteLog = 0;
            if ((s_noteLog++ & 7) == 0) {
                char buf[128];
                sprintf_s(buf, "notes:pushed=%zu gpu=%zu batches=%zu", total, noteLimit,
                    (noteLimit + MaxNotesPerPass - 1) / MaxNotesPerPass);
                HeartbeatLog(buf);
            }
        }
        for (size_t i = startIdx; i < total; i += batchSize) {
            if (i == startIdx)
                SetPipeline(Pipeline::Note);

            auto remaining = total - i;
            auto note_count = min(remaining, batchSize);
            D3D12_RANGE note_range = {
                .Begin = 0,
                .End = note_count * sizeof(NoteData),
            };
            NoteData* notes = nullptr;
            res = m_pNoteBuffers[m_uFrameIndex]->Map(0, &note_range, (void**)&notes);
            if (FAILED(res))
                return res;
            memcpy(notes, &m_vNotesIntermediate[i], note_count * sizeof(NoteData));
            m_pNoteBuffers[m_uFrameIndex]->Unmap(0, &note_range);

            m_pCommandList->DrawIndexedInstanced(note_count * 6, 1, 0, 0, 0);

            if (remaining - note_count != 0) {
                res = m_pCommandList->Close();
                if (FAILED(res))
                    return res;

                ID3D12CommandList* command_lists[] = { m_pCommandList.Get() };
                m_pCommandQueue->ExecuteCommandLists(1, command_lists);

                res = WaitForGPU();
                if (FAILED(res))
                    return res;

                m_pCommandAllocator[m_uFrameIndex]->Reset();
                m_pCommandList->Reset(m_pCommandAllocator[m_uFrameIndex].Get(), m_pRectPipelineState.Get());

                SetPipeline(Pipeline::Note);
                SetupCommandList();
            }
        }
    }

    if (rect_count > rect_split) {
        SetPipeline(Pipeline::Rect);
        m_pCommandList->IASetVertexBuffers(0, 1, &m_VertexBufferViews[m_uFrameIndex]);
        m_pCommandList->DrawIndexedInstanced((rect_count - rect_split) / 4 * 6, 1, rect_split / 4 * 6, 0, 0);
    }
    auto s_t2 = std::chrono::steady_clock::now();

    if (!g_bDisableBlur)
        ApplyBlur();

    const auto& bloomViz = Config::GetConfig().GetVizSettings();
    if (bloomViz.bBloom && m_pBloomPipelineState && m_pBloomExtractPipelineState && bloomViz.fBloomIntensity > 0.0f && m_pBloomHalf) {
        ID3D12DescriptorHeap* heaps[] = { m_pImGuiSRVDescriptorHeap.Get() };
        m_pCommandList->SetDescriptorHeaps(_countof(heaps), heaps);

        if (m_SceneCopyState != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pSceneCopy.Get(), m_SceneCopyState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            m_pCommandList->ResourceBarrier(1, &barrier);
            m_SceneCopyState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }

        if (m_BloomHalfState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pBloomHalf.Get(), m_BloomHalfState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_pCommandList->ResourceBarrier(1, &barrier);
            m_BloomHalfState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        m_pCommandList->SetComputeRootSignature(m_pBloomExtractRootSignature.Get());
        m_pCommandList->SetPipelineState(m_pBloomExtractPipelineState.Get());

        float extractCB[4] = { bloomViz.fBloomBrightness * 0.4f, 0.1f, 2.5f, 0.0f };
        m_pCommandList->SetComputeRoot32BitConstants(0, 4, extractCB, 0);
        m_pCommandList->SetComputeRootDescriptorTable(1, m_BlurSceneSRVGPU);
        m_pCommandList->SetComputeRootDescriptorTable(2, m_BloomHalfUAVGPU);

        UINT extract_gx = (m_iBloomHalfWidth + 15) / 16;
        UINT extract_gy = (m_iBloomHalfHeight + 15) / 16;
        m_pCommandList->Dispatch(extract_gx, extract_gy, 1);

        {
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pBloomHalf.Get(), m_BloomHalfState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            m_pCommandList->ResourceBarrier(1, &barrier);
            m_BloomHalfState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        if (m_BloomBlurTempState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pBloomBlurTemp.Get(), m_BloomBlurTempState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_pCommandList->ResourceBarrier(1, &barrier);
            m_BloomBlurTempState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        m_pCommandList->SetComputeRootSignature(m_pBlurRootSignature.Get());
        m_pCommandList->SetPipelineState(m_pBlurPipelineState.Get());

        const int sigma = max(1, (int)(bloomViz.fBloomSpread + 0.5f));
        UINT blur_consts[4] = { 0, (UINT)sigma, 0, 0 }; // direction 0 = horizontal
        m_pCommandList->SetComputeRoot32BitConstants(0, 4, blur_consts, 0);
        m_pCommandList->SetComputeRootDescriptorTable(1, m_BloomHalfSRVGPU);
        m_pCommandList->SetComputeRootDescriptorTable(2, m_BloomBlurTempUAVGPU);

        UINT blur_gx = (m_iBloomHalfWidth + 255) / 256;
        m_pCommandList->Dispatch(blur_gx, m_iBloomHalfHeight, 1);

        {
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pBloomBlurTemp.Get(), m_BloomBlurTempState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            m_pCommandList->ResourceBarrier(1, &barrier);
            m_BloomBlurTempState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        }
        if (m_BloomBlurResultState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pBloomBlurResult.Get(), m_BloomBlurResultState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_pCommandList->ResourceBarrier(1, &barrier);
            m_BloomBlurResultState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }

        blur_consts[0] = 1; // direction 1 = vertical
        m_pCommandList->SetComputeRoot32BitConstants(0, 4, blur_consts, 0);
        m_pCommandList->SetComputeRootDescriptorTable(1, m_BloomBlurTempSRVGPU);
        m_pCommandList->SetComputeRootDescriptorTable(2, m_BloomBlurResultUAVGPU);

        UINT blur_gy = (m_iBloomHalfHeight + 255) / 256;
        m_pCommandList->Dispatch(m_iBloomHalfWidth, blur_gy, 1);

        {
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pBloomBlurResult.Get(), m_BloomBlurResultState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_pCommandList->ResourceBarrier(1, &barrier);
            m_BloomBlurResultState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        m_pCommandList->SetPipelineState(m_pBloomPipelineState.Get());
        m_pCommandList->SetGraphicsRootSignature(m_pBloomRootSignature.Get());
        m_pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_pCommandList->IASetVertexBuffers(0, 0, nullptr);
        m_pCommandList->IASetIndexBuffer(&m_IndexBufferView);

        m_pCommandList->SetGraphicsRootDescriptorTable(1, m_BloomBlurResultSRVGPU);
        float compositeCB[4] = { max(0.0f, bloomViz.fBloomSaturation), 1.0f, 0.0f, 0.0f };
        m_pCommandList->SetGraphicsRoot32BitConstants(0, 4, compositeCB, 0);
        float intensity = min(bloomViz.fBloomIntensity, 1.0f);
        float bf1[4] = { intensity, intensity, intensity, intensity };
        m_pCommandList->OMSetBlendFactor(bf1);
        m_pCommandList->DrawIndexedInstanced(3, 1, 0, 0, 0);

        if (bloomViz.fRibbonBloomHeight > 0.0f && (bloomViz.fRibbonBloomIntensity * bloomViz.fRibbonBloomBrightness) > 0.0f && m_fRibbonCX > 0 && m_fRibbonCY > 0) {
            const int strips = max(1, min(bloomViz.iRibbonBloomSteps, 100));
            const float fadePad = bloomViz.fRibbonBloomHeight;
            const float ribBrightness = max(0.0f, bloomViz.fRibbonBloomIntensity * bloomViz.fRibbonBloomBrightness);
            const float totalH = m_fRibbonCY + fadePad * 2.0f;
            const float stripH = totalH / strips;

            float ribbonCB[4] = { max(0.0f, bloomViz.fBloomSaturation), ribBrightness, 0.0f, 0.0f };
            m_pCommandList->SetGraphicsRoot32BitConstants(0, 4, ribbonCB, 0);

            for (int s = 0; s < strips; s++) {
                float t = (s + 0.5f) / (float)strips;
                float alpha = 0.5f * (1.0f + cosf(3.14159265f * (2.0f * t - 1.0f)));
                if (alpha < 0.005f) continue;
                float bf[4] = { alpha, alpha, alpha, alpha };
                m_pCommandList->OMSetBlendFactor(bf);
                float stripY = m_fRibbonY - fadePad + s * stripH;
                D3D12_RECT scissor = {
                    (LONG)max(m_fRibbonX - fadePad * 2.0f, 0.0f),
                    (LONG)max(stripY, 0.0f),
                    (LONG)min(m_fRibbonX + m_fRibbonCX + fadePad * 2.0f, (float)m_iBufferWidth),
                    (LONG)min(stripY + stripH, (float)m_iBufferHeight),
                };
                m_pCommandList->RSSetScissorRects(1, &scissor);
                m_pCommandList->DrawIndexedInstanced(3, 1, 0, 0, 0);
            }
            D3D12_RECT fullScissor = { 0, 0, m_iBufferWidth, m_iBufferHeight };
            m_pCommandList->RSSetScissorRects(1, &fullScissor);
            float bfRestore[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            m_pCommandList->OMSetBlendFactor(bfRestore);
        }

        if (m_SceneCopyState != D3D12_RESOURCE_STATE_COPY_DEST) {
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pSceneCopy.Get(), m_SceneCopyState, D3D12_RESOURCE_STATE_COPY_DEST);
            m_pCommandList->ResourceBarrier(1, &barrier);
            m_SceneCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
        }
    }

    DrawPianoRollStripBackground();
    DrawPianoRollStrip();
    auto s_t3 = std::chrono::steady_clock::now();

    if (!Config::GetConfig().GetVizSettings().bDisableUI) {
        ID3D12DescriptorHeap* heaps[] = { m_pImGuiSRVDescriptorHeap.Get() };
        m_pCommandList->SetDescriptorHeaps(_countof(heaps), heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_pCommandList.Get());
    }
    auto s_t4 = std::chrono::steady_clock::now();

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pRenderTargets[m_uFrameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_pCommandList->ResourceBarrier(1, &barrier);

    res = m_pCommandList->Close();
    if (FAILED(res))
        return res;

    HeartbeatLog("endscene:exec");
    ID3D12CommandList* command_lists[] = { m_pCommandList.Get() };
    m_pCommandQueue->ExecuteCommandLists(1, command_lists);

    {
        static int s_endLog = 0;
        static double eImgui = 0, eFlush = 0, eFx = 0, eStrip = 0, eDraw = 0;
        auto s_t5 = std::chrono::steady_clock::now();
        eImgui += std::chrono::duration<double, std::milli>(s_t1 - s_t0).count();
        eFlush += std::chrono::duration<double, std::milli>(s_t2 - s_t1).count();
        eFx += std::chrono::duration<double, std::milli>(s_t3 - s_t2).count();
        eStrip += std::chrono::duration<double, std::milli>(s_t4 - s_t3).count();
        eDraw += std::chrono::duration<double, std::milli>(s_t5 - s_t4).count();
        if ((s_endLog++ & 127) == 127) {
            char buf[128];
            sprintf_s(buf, "e:imgui=%.2f flush=%.2f fx=%.2f strip=%.2f draw+close=%.2f",
                eImgui / 128.0, eFlush / 128.0, eFx / 128.0, eStrip / 128.0, eDraw / 128.0);
            HeartbeatLog(buf);
            eImgui = eFlush = eFx = eStrip = eDraw = 0;
        }
    }

    return S_OK;
}

HRESULT D3D12Renderer::Present() {
    HRESULT res;
    HeartbeatLog("present:start");
    static int s_branch = -1;
    static int s_frames = 0;
    static double s_waitSum = 0.0;
    static long long s_fpsStart = 0;
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
    s_frames++;
    if (s_frames == 1 || s_frames == 128) {
        long long now = GetTickCount64();
        if (s_frames == 1) s_fpsStart = now;
        else {
            char buf[64];
            sprintf_s(buf, "present:fps=%d wait=%.2f", (int)llround(128000.0 / max(now - s_fpsStart, 1)),
                s_waitSum / (double)s_frames);
            HeartbeatLog(buf);
            s_waitSum = 0.0;
            s_frames = 0;
        }
    }
    if (!g_bDisableGates && IsIconic(g_hWnd)) {
        Sleep(16);
        res = S_OK;
    } else if (m_bLimitFPS)
        res = m_pSwapChain->Present(1, 0);
    else {
        res = m_pSwapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
    }
    if (res == DXGI_ERROR_DEVICE_REMOVED || res == DXGI_ERROR_DEVICE_RESET) {
        // The display driver was reset (TDR). The device is dead; do not run the
        m_bDeviceLost = true;
        HeartbeatLog("present:deviceremoved");
        return res;
    }
    if (FAILED(res))
        return res;

    const UINT64 cur_fence_value = m_pFenceValues[m_uFrameIndex];
    res = m_pCommandQueue->Signal(m_pFence.Get(), cur_fence_value);
    if (FAILED(res))
        return res;

    m_uFrameIndex = m_pSwapChain->GetCurrentBackBufferIndex();

    auto tWaitStart = std::chrono::steady_clock::now();
    if (m_pFence->GetCompletedValue() < m_pFenceValues[m_uFrameIndex]) {
        ResetEvent(m_hFenceEvent);
        res = m_pFence->SetEventOnCompletion(m_pFenceValues[m_uFrameIndex], m_hFenceEvent);
        if (FAILED(res))
            return res;

        HeartbeatLog("present:fencewait");

        while (WaitForSingleObjectEx(m_hFenceEvent, 1000, FALSE) == WAIT_TIMEOUT) {
            if (g_bGfxDestroyed)
                break;
        }
    }
    s_waitSum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tWaitStart).count();

    m_pFenceValues[m_uFrameIndex] = cur_fence_value + 1;
    return S_OK;
}

void D3D12Renderer::DrawPianoRollStripBackground() {
    if (g_bDisableBlur || !Config::GetConfig().GetVizSettings().bDualPianoRoll || !m_pBlurOutput)
        return;

    const float menuBarHeight = 20.0f;
    const float toolbarHeight = 35.0f;
    const float stripTop = menuBarHeight + toolbarHeight;
    const float stripH = max(190.0f, min((float)m_iBufferHeight * 0.45f, (float)m_iBufferHeight * 0.28f));
    const float stripBottom = stripTop + stripH;
    const float fadeBottom = min((float)m_iBufferHeight, stripBottom + 40.0f);
    const D3D12_RECT stripScissor = {
        0,
        (LONG)floor(stripTop),
        m_iBufferWidth,
        min(m_iBufferHeight, (LONG)ceil(fadeBottom)),
    };
    const D3D12_RECT fullScissor = { 0, 0, m_iBufferWidth, m_iBufferHeight };

    const BackgroundConstants backgroundConstants = {
        .fadeStart = stripBottom,
        .fadeEnd = fadeBottom,
        .fadeEnabled = 1.0f,
        .padding = 1.0f,
    };
    SetPipeline(Pipeline::Background);
    ID3D12DescriptorHeap* heaps[] = { m_pImGuiSRVDescriptorHeap.Get() };
    m_pCommandList->SetDescriptorHeaps(_countof(heaps), heaps);
    m_pCommandList->SetGraphicsRoot32BitConstants(0, sizeof(backgroundConstants) / 4, &backgroundConstants, 0);
    m_pCommandList->SetGraphicsRootDescriptorTable(1, m_BlurOutputSRVGPU);
    m_pCommandList->RSSetScissorRects(1, &stripScissor);
    m_pCommandList->DrawIndexedInstanced(3, 1, 0, 0, 0);
    m_pCommandList->RSSetScissorRects(1, &fullScissor);
}

float D3D12Renderer::GetDualRollTimeSpan(float normalTimeSpan, float normalRollPixels) const {
    const float normalPixels = max(normalRollPixels, 1.0f);
    const float stripPixels = max((float)m_iBufferWidth, 1.0f);
    return max(normalTimeSpan * 2.0f * stripPixels / normalPixels, 1.0f);
}

void D3D12Renderer::DrawPianoRollStrip() {
    if (!Config::GetConfig().GetVizSettings().bDualPianoRoll)
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
    const float stripTop = menuBarHeight + toolbarHeight;

    // Reposition the depth so the strip pass starts clean; nothing after it
    CD3DX12_CPU_DESCRIPTOR_HANDLE stripDsv(m_pDSVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    m_pCommandList->ClearDepthStencilView(stripDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    const float stripH = max(190.0f, min((float)m_iBufferHeight * 0.45f, (float)m_iBufferHeight * 0.28f));
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

    HRESULT res = S_OK;
    {
        static int s_stripLog = 0;
        if ((s_stripLog++ & 7) == 0) {
            char buf[96];
            sprintf_s(buf, "notes:strip=%zu sampled=%zu stride=%zu", stripCount, sampledCount, stride);
            HeartbeatLog(buf);
        }
    }
    // always suffices; the decimated entries are copied individually with the
    const size_t stripByteOffset = (size_t)PianoRollStripNoteOffset * sizeof(NoteData);
    D3D12_RANGE noteRange = {
        stripByteOffset,
        stripByteOffset + sampledCount * sizeof(NoteData),
    };
    NoteData* notes = nullptr;
    res = m_pNoteBuffers[m_uFrameIndex]->Map(0, &noteRange, (void**)&notes);
    if (FAILED(res)) {
        m_RootConstants = savedRoot;
        return;
    }
    for (size_t s = 0; s < sampledCount; s++)
        notes[PianoRollStripNoteOffset + s] = stripNotes[s * stride];
    m_pNoteBuffers[m_uFrameIndex]->Unmap(0, &noteRange);

    SetPipeline(Pipeline::Note);
    m_pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_pCommandList->IASetVertexBuffers(0, 0, nullptr);
    m_pCommandList->IASetIndexBuffer(&m_IndexBufferView);
    m_pCommandList->SetGraphicsRootShaderResourceView(1, m_pFixedBuffer->GetGPUVirtualAddress());
    m_pCommandList->SetGraphicsRootShaderResourceView(2, m_pTrackColorBuffer->GetGPUVirtualAddress());
    m_pCommandList->SetGraphicsRootShaderResourceView(3,
        m_pNoteBuffers[m_uFrameIndex]->GetGPUVirtualAddress() + stripByteOffset);
    m_pCommandList->SetGraphicsRoot32BitConstants(0, sizeof(m_RootConstants) / 4, &m_RootConstants, 0);
    m_pCommandList->DrawIndexedInstanced(sampledCount * 6, 1, 0, 0, 0);

    m_RootConstants = savedRoot;
    SetPipeline(Pipeline::Note);
}

void D3D12Renderer::DrawPianoRollStripKeyboard() {
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

HRESULT D3D12Renderer::BeginText() {
    ImGui::Render();
    m_pDrawList->_ResetForNewFrame();
    m_pDrawList->PushClipRectFullScreen();
    m_pDrawList->PushTextureID(ImGui::GetIO().Fonts->TexID);
    return S_OK;
}

HRESULT D3D12Renderer::DrawTextW(const WCHAR*, FontSize, LPRECT, DWORD, DWORD, INT) {
    return S_OK;
}

HRESULT D3D12Renderer::DrawTextA(const CHAR*, FontSize, LPRECT, DWORD, DWORD, INT) {
    return S_OK;
}

HRESULT D3D12Renderer::EndText() {
    return S_OK;
}

HRESULT D3D12Renderer::DrawRect(float x, float y, float cx, float cy, DWORD color) {
    return DrawRect(x, y, cx, cy, color, color, color, color);
}

HRESULT D3D12Renderer::DrawRect(float x, float y, float cx, float cy, DWORD c1, DWORD c2, DWORD c3, DWORD c4) {
    m_vRectsIntermediate.insert(m_vRectsIntermediate.end(), {
        {x,      y,      c1},
        {x + cx, y,      c2},
        {x + cx, y + cy, c3},
        {x,      y + cy, c4},
    });
    return S_OK;
}

HRESULT D3D12Renderer::DrawSkew(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, DWORD color) {
    return DrawSkew(x1, y1, x2, y2, x3, y3, x4, y4, color, color, color, color);
}

HRESULT D3D12Renderer::DrawSkew(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, DWORD c1, DWORD c2, DWORD c3, DWORD c4) {
    m_vRectsIntermediate.insert(m_vRectsIntermediate.end(), {
        {x1, y1, c1},
        {x2, y2, c2},
        {x3, y3, c3},
        {x4, y4, c4},
    });
    return S_OK;
}

HRESULT D3D12Renderer::RenderBatch(bool) {
    return S_OK;
}

HRESULT D3D12Renderer::SetLimitFPS(bool bLimitFPS) {
    m_bLimitFPS = bLimitFPS;
    return S_OK;
}

std::wstring D3D12Renderer::GetAdapterName() {
    if (m_pAdapter) {
        DXGI_ADAPTER_DESC2 desc = {};
        if (FAILED(m_pAdapter->GetDesc2(&desc)))
            return L"GetDesc2 failed";
        return desc.Description;
    }
    return L"None";
}

HRESULT D3D12Renderer::WaitForGPU() {
    if (g_bSkipGPUWait) {
        static bool logged = false;
        if (!logged) { HeartbeatLog("waitgpu:skipped"); logged = true; }
        m_pFenceValues[m_uFrameIndex]++;
        return S_OK;
    }

    HeartbeatLog("waitgpu:signal");
    HRESULT res = m_pCommandQueue->Signal(m_pFence.Get(), m_pFenceValues[m_uFrameIndex]);
    if (FAILED(res))
        return res;

    auto val = m_pFence->GetCompletedValue();
    if (val < m_pFenceValues[m_uFrameIndex]) {
        ResetEvent(m_hFenceEvent);
        HeartbeatLog("waitgpu:waiting");
        res = m_pFence->SetEventOnCompletion(m_pFenceValues[m_uFrameIndex], m_hFenceEvent);
        if (FAILED(res))
            return res;
        while (WaitForSingleObjectEx(m_hFenceEvent, 1000, FALSE) == WAIT_TIMEOUT) {
            HRESULT reason = m_pDevice->GetDeviceRemovedReason();
            if (FAILED(reason)) {
                m_bDeviceLost = true;
                HeartbeatLog("waitgpu:deviceremoved");
                return reason;
            }
            HeartbeatLog("waitgpu:spinning");
            if (g_bGfxDestroyed)
                return E_ABORT;
        }
    }

    HeartbeatLog("waitgpu:done");

    m_pFenceValues[m_uFrameIndex]++;

    return S_OK;
}

void D3D12Renderer::SetPipeline(Pipeline pipeline) {
    switch (pipeline) {
    case Pipeline::Rect:
        m_pCommandList->SetPipelineState(m_pRectPipelineState.Get());
        m_pCommandList->SetGraphicsRootSignature(m_pRectRootSignature.Get());
        m_pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_pCommandList->IASetVertexBuffers(0, 1, &m_VertexBufferViews[m_uFrameIndex]);
        m_pCommandList->IASetIndexBuffer(&m_IndexBufferView);
        m_pCommandList->SetGraphicsRoot32BitConstants(0, sizeof(m_RootConstants) / 4, &m_RootConstants, 0);
        break;
    case Pipeline::Note:
        m_pCommandList->SetPipelineState(m_pNotePipelineState.Get());
        m_pCommandList->SetGraphicsRootSignature(m_pNoteRootSignature.Get());
        m_pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_pCommandList->IASetVertexBuffers(0, 0, nullptr);
        m_pCommandList->IASetIndexBuffer(&m_IndexBufferView);
        m_pCommandList->SetGraphicsRootShaderResourceView(1, m_pFixedBuffer->GetGPUVirtualAddress());
        m_pCommandList->SetGraphicsRootShaderResourceView(2, m_pTrackColorBuffer->GetGPUVirtualAddress());
        m_pCommandList->SetGraphicsRootShaderResourceView(3, m_pNoteBuffers[m_uFrameIndex]->GetGPUVirtualAddress());
        break;
    case Pipeline::Background:
        ID3D12DescriptorHeap* heaps[] = { m_pTextureSRVDescriptorHeap.Get() };
        const BackgroundConstants backgroundConstants = {};
        m_pCommandList->SetDescriptorHeaps(_countof(heaps), heaps);
        m_pCommandList->SetPipelineState(m_pBackgroundPipelineState.Get());
        m_pCommandList->SetGraphicsRootSignature(m_pBackgroundRootSignature.Get());
        m_pCommandList->SetGraphicsRoot32BitConstants(0, sizeof(backgroundConstants) / 4, &backgroundConstants, 0);
        m_pCommandList->SetGraphicsRootDescriptorTable(1, m_pTextureSRVDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        m_pCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_pCommandList->IASetVertexBuffers(0, 0, nullptr);
        m_pCommandList->IASetIndexBuffer(&m_IndexBufferView);
        break;
    }
}

void D3D12Renderer::SetupCommandList() {
    D3D12_VIEWPORT viewport = {
        .TopLeftX = 0,
        .TopLeftY = 0,
        .Width = (float)m_iBufferWidth,
        .Height = (float)m_iBufferHeight,
        .MinDepth = 0.0,
        .MaxDepth = 1.0,
    };
    D3D12_RECT scissor = { 0, 0, m_iBufferWidth, m_iBufferHeight };
    m_pCommandList->RSSetViewports(1, &viewport);
    m_pCommandList->RSSetScissorRects(1, &scissor);
    m_pCommandList->SetGraphicsRoot32BitConstants(0, sizeof(m_RootConstants) / 4, &m_RootConstants, 0);

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(m_pRTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), m_uFrameIndex, m_uRTVDescriptorSize);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(m_pDSVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    m_pCommandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
}

char* D3D12Renderer::Screenshot() {
    m_pCommandAllocator[m_uFrameIndex]->Reset();
    m_pCommandList->Reset(m_pCommandAllocator[m_uFrameIndex].Get(), m_pRectPipelineState.Get());

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pRenderTargets[m_uFrameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_pCommandList->ResourceBarrier(1, &barrier);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {
        .Offset = 0,
        .Footprint = {
            .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
            .Width = (UINT)m_iBufferWidth,
            .Height = (UINT)m_iBufferHeight,
            .Depth = 1,
            .RowPitch = (UINT)m_ullScreenshotPitch,
        },
    };
    CD3DX12_TEXTURE_COPY_LOCATION copy_dst(m_pScreenshotStaging.Get(), footprint);
    CD3DX12_TEXTURE_COPY_LOCATION copy_src(m_pRenderTargets[m_uFrameIndex].Get(), 0);
    m_pCommandList->CopyTextureRegion(&copy_dst, 0, 0, 0, &copy_src, nullptr);

    barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pRenderTargets[m_uFrameIndex].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    m_pCommandList->ResourceBarrier(1, &barrier);

    m_pCommandList->Close();
    ID3D12CommandList* command_lists[] = { m_pCommandList.Get() };
    m_pCommandQueue->ExecuteCommandLists(1, command_lists);

    if (FAILED(WaitForGPU()))
        return nullptr;

    D3D12_RANGE staging_range = {
        .Begin = 0,
        .End = m_ullScreenshotPitch * m_iBufferHeight,
    };
    char* staging = nullptr;
    if (FAILED(m_pScreenshotStaging->Map(0, &staging_range, (void**)&staging)))
        return nullptr;
    for (int y = 0; y < m_iBufferHeight; y++)
        memcpy(&m_vScreenshotOutput[y * m_iBufferWidth * 4], &staging[y * m_ullScreenshotPitch], m_iBufferWidth * 4);
    m_pScreenshotStaging->Unmap(0, &staging_range);

    return m_vScreenshotOutput.data();
}

bool D3D12Renderer::LoadBackgroundBitmap(std::wstring path) {
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

bool D3D12Renderer::UploadBackgroundBitmap() {
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

    m_pCommandAllocator[m_uFrameIndex]->Reset();
    m_pCommandList->Reset(m_pCommandAllocator[m_uFrameIndex].Get(), nullptr);

    D3D12_SUBRESOURCE_DATA texture_data = {
        .pData = scaled.data(),
        .RowPitch = m_iBufferWidth * 4,
        .SlicePitch = (LONG_PTR)scaled.size(),
    };
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pTextureBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    m_pCommandList->ResourceBarrier(1, &barrier);
    UpdateSubresources(m_pCommandList.Get(), m_pTextureBuffer.Get(), m_pTextureUpload.Get(), 0, 0, 1, &texture_data);
    barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pTextureBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_pCommandList->ResourceBarrier(1, &barrier);
    
    m_pCommandList->Close();
    ID3D12CommandList* command_lists[] = { m_pCommandList.Get() };
    m_pCommandQueue->ExecuteCommandLists(1, command_lists);

    WaitForGPU();

    return true;
}

void D3D12Renderer::SetBackgroundBlur(float sigma) {
    m_fBGBlurSigma = sigma;
    if (!m_pUnscaledBackground)
        return;

    UploadBackgroundBitmap();

    if (sigma < 0.5f || !m_pBlurPipelineState || !m_pBGBlurHeap)
        return;

    const UINT iSigma = (UINT)(sigma + 0.5f);

    WaitForGPU();
    m_pCommandAllocator[m_uFrameIndex]->Reset();
    m_pCommandList->Reset(m_pCommandAllocator[m_uFrameIndex].Get(), nullptr);

    auto barrier1 = CD3DX12_RESOURCE_BARRIER::Transition(m_pTextureBuffer.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_pCommandList->ResourceBarrier(1, &barrier1);
    auto barrier2 = CD3DX12_RESOURCE_BARRIER::Transition(m_pBGBufferA.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_pCommandList->ResourceBarrier(1, &barrier2);
    auto barrier3 = CD3DX12_RESOURCE_BARRIER::Transition(m_pBGBufferB.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_pCommandList->ResourceBarrier(1, &barrier3);

    ID3D12DescriptorHeap* heaps[] = { m_pBGBlurHeap.Get() };
    m_pCommandList->SetDescriptorHeaps(1, heaps);
    m_pCommandList->SetComputeRootSignature(m_pBlurRootSignature.Get());
    m_pCommandList->SetPipelineState(m_pBlurPipelineState.Get());

    UINT blur_consts[4] = { 0, iSigma, 0, 0 };
    m_pCommandList->SetComputeRoot32BitConstants(0, 4, blur_consts, 0);
    m_pCommandList->SetComputeRootDescriptorTable(1, m_BGBgTexSRVGPU);
    m_pCommandList->SetComputeRootDescriptorTable(2, m_BGBgAUAVGPU);
    m_pCommandList->Dispatch((m_iBufferWidth + 255) / 256, m_iBufferHeight, 1);

    auto barrier4 = CD3DX12_RESOURCE_BARRIER::Transition(m_pBGBufferA.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_pCommandList->ResourceBarrier(1, &barrier4);

    blur_consts[0] = 1;
    m_pCommandList->SetComputeRoot32BitConstants(0, 4, blur_consts, 0);
    m_pCommandList->SetComputeRootDescriptorTable(1, m_BGBgASRVGPU);
    m_pCommandList->SetComputeRootDescriptorTable(2, m_BGBgBUAVGPU);
    m_pCommandList->Dispatch(m_iBufferWidth, (m_iBufferHeight + 255) / 256, 1);

    auto barrier5 = CD3DX12_RESOURCE_BARRIER::Transition(m_pBGBufferB.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_pCommandList->ResourceBarrier(1, &barrier5);
    auto barrier6 = CD3DX12_RESOURCE_BARRIER::Transition(m_pTextureBuffer.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    m_pCommandList->ResourceBarrier(1, &barrier6);
    m_pCommandList->CopyResource(m_pTextureBuffer.Get(), m_pBGBufferB.Get());
    auto barrier7 = CD3DX12_RESOURCE_BARRIER::Transition(m_pTextureBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_pCommandList->ResourceBarrier(1, &barrier7);

    m_pCommandList->Close();
    ID3D12CommandList* lists[] = { m_pCommandList.Get() };
    m_pCommandQueue->ExecuteCommandLists(1, lists);
    WaitForGPU();
}

static const char* g_BackgroundPSHLSL = R"(
Texture2D tex : register(t0);
SamplerState tex_sampler : register(s0);

cbuffer BackgroundConstants : register(b0) {
    float fade_start;
    float fade_end;
    float fade_enabled;
    float opacity;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET {
    float4 c = tex.Sample(tex_sampler, input.uv);
    float vis;
    if (fade_enabled > 0.5) {
        float denom = max(fade_start + 1.0, fade_end) - fade_start;
        float t = saturate((input.position.y - fade_start) / denom);
        vis = 1.0 - t * t * (3.0 - 2.0 * t);
    } else {
        vis = c.a;
    }
    return float4(c.rgb, 1.0 - opacity * vis);
}
)";

static HRESULT CompileBackgroundPS(ID3DBlob** ppBlob) {
    return D3DCompile(g_BackgroundPSHLSL, strlen(g_BackgroundPSHLSL), nullptr, nullptr, nullptr, "main", "ps_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, ppBlob, nullptr);
}

static const char* g_BlurHLSL = R"(
RWTexture2D<float4> g_output : register(u0);
Texture2D<float4> g_input : register(t0);

cbuffer cb : register(b0) {
    int g_blurDirection;
    int g_sigma;
    int2 g_padding;
};

[numthreads(256, 1, 1)]
void CSMain(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID, uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint w, h;
    g_input.GetDimensions(w, h);
    int sigma = max(g_sigma, 1);
    int radius = min(sigma * 3, 63);
    float sigma2 = (float)(sigma * sigma);
    int2 coord;
    if (g_blurDirection == 0) {
        coord = int2((int)dispatchThreadId.x, (int)groupId.y);
    } else {
        coord = int2((int)groupId.x, (int)(groupId.y * 256 + groupThreadId.x));
    }
    if (coord.x >= (int)w || coord.y >= (int)h) return;
    float4 result = 0;
    float totalW = 0;
    [loop] for (int i = -radius; i <= radius; i++) {
        float d = (float)i;
        float wgt = exp(-(d * d) / (2.0f * sigma2));
        int2 sc = coord;
        if (g_blurDirection == 0) sc.x = clamp(coord.x + i, 0, (int)w - 1);
        else sc.y = clamp(coord.y + i, 0, (int)h - 1);
        result += g_input[sc] * wgt;
        totalW += wgt;
    }
    float4 blurred = result / totalW;
    blurred.a = 1.0; // Force opaque so the blur is always visible
    g_output[coord] = blurred;
}
)";

static HRESULT CompileBlurShader(ID3DBlob** ppBlob) {
    return D3DCompile(g_BlurHLSL, strlen(g_BlurHLSL), nullptr, nullptr, nullptr, "CSMain", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, ppBlob, nullptr);
}

static const char* g_BloomExtractHLSL = R"(
Texture2D<float4> g_input : register(t0);
RWTexture2D<float4> g_output : register(u0);

cbuffer cb : register(b0) {
    float g_threshold;
    float g_knee;
    float g_pregain;
    float g_padding;
};

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID) {
    uint ow, oh;
    g_output.GetDimensions(ow, oh);
    if (dtid.x >= ow || dtid.y >= oh) return;

    uint iw, ih;
    g_input.GetDimensions(iw, ih);
    int2 base = int2(dtid.xy) * 2;
    float4 s00 = g_input[min(base + int2(0,0), int2(iw-1, ih-1))];
    float4 s10 = g_input[min(base + int2(1,0), int2(iw-1, ih-1))];
    float4 s01 = g_input[min(base + int2(0,1), int2(iw-1, ih-1))];
    float4 s11 = g_input[min(base + int2(1,1), int2(iw-1, ih-1))];
    float3 color = (s00.rgb + s10.rgb + s01.rgb + s11.rgb) * 0.25;

    float maxc = max(color.r, max(color.g, color.b));
    float knee2 = g_knee * 2.0;
    float soft = clamp(maxc - g_threshold + g_knee, 0.0, knee2);
    soft = (knee2 > 0.00001) ? (soft * soft / (4.0 * g_knee + 0.00001)) : 0.0;
    float contribution = max(soft, maxc - g_threshold) / max(maxc, 0.00001);
    contribution = max(contribution, 0.0);

    float3 extracted = color * contribution * g_pregain;
    g_output[dtid.xy] = float4(extracted, 1.0);
}
)";

static HRESULT CompileBloomExtractShader(ID3DBlob** ppBlob) {
    return D3DCompile(g_BloomExtractHLSL, strlen(g_BloomExtractHLSL), nullptr, nullptr, nullptr, "CSMain", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, ppBlob, nullptr);
}

static const char* g_BloomHLSL = R"(
Texture2D<float4> g_bloom : register(t0);
SamplerState g_sampler : register(s0);

cbuffer BloomConstants : register(b0) {
    float saturation;
    float brightness;
    float2 padding;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET {
    float3 bloom = g_bloom.Sample(g_sampler, input.uv).rgb;

    float maxC = max(bloom.r, max(bloom.g, bloom.b));
    if (maxC > 0.0001) {
        float3 normalized = bloom / maxC;

        float gray = dot(normalized, float3(0.2126, 0.7152, 0.0722));
        float effectiveSat = saturation * (1.0 + 0.35 * max(0.0, brightness - 1.0));
        normalized = lerp(float3(gray, gray, gray), normalized, effectiveSat);
        normalized = max(normalized, 0.0);

        float scaledMax = maxC * brightness;
        float compressedMax = scaledMax / (1.0 + 0.25 * scaledMax);

        bloom = normalized * compressedMax;
    }

    return float4(bloom, 0.0);
}
)";

static HRESULT CompileBloomShader(ID3DBlob** ppBlob) {
    return D3DCompile(g_BloomHLSL, strlen(g_BloomHLSL), nullptr, nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, ppBlob, nullptr);
}

HRESULT D3D12Renderer::CreateBlurResources() {
    if (m_pBlurPipelineState)
        DestroyBlurResources();

    HRESULT res;

    ComPtr<ID3DBlob> cs_blob;
    HeartbeatLog("blur:compile");
    res = CompileBlurShader(&cs_blob);
    if (FAILED(res)) return res;
    HeartbeatLog("blur:rootsig");

    D3D12_DESCRIPTOR_RANGE srv_range = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
        .NumDescriptors = 1,
        .BaseShaderRegister = 0,
        .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };
    D3D12_DESCRIPTOR_RANGE uav_range = {
        .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
        .NumDescriptors = 1,
        .BaseShaderRegister = 0,
        .RegisterSpace = 0,
        .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
    };
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srv_range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &uav_range;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc = {
        .NumParameters = 3,
        .pParameters = params,
        .NumStaticSamplers = 0,
        .pStaticSamplers = nullptr,
        .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
    };
    ComPtr<ID3DBlob> rs_blob, rs_err;
    res = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err);
    if (FAILED(res)) return res;
    res = m_pDevice->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_pBlurRootSignature));
    if (FAILED(res)) return res;

    D3D12_COMPUTE_PIPELINE_STATE_DESC ps_desc = {
        .pRootSignature = m_pBlurRootSignature.Get(),
        .CS = { .pShaderBytecode = cs_blob->GetBufferPointer(), .BytecodeLength = cs_blob->GetBufferSize() },
        .NodeMask = 0,
    };
    res = m_pDevice->CreateComputePipelineState(&ps_desc, IID_PPV_ARGS(&m_pBlurPipelineState));
    if (FAILED(res)) return res;
    HeartbeatLog("blur:pso");

    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC composite_desc = {};
        composite_desc.pRootSignature = m_pBackgroundRootSignature.Get();
        composite_desc.VS = { .pShaderBytecode = g_pBackgroundVertexShader, .BytecodeLength = sizeof(g_pBackgroundVertexShader) };
        composite_desc.PS = { .pShaderBytecode = g_pBackgroundPixelShader, .BytecodeLength = sizeof(g_pBackgroundPixelShader) };
        composite_desc.BlendState.RenderTarget[0] = {
            .BlendEnable = TRUE,
            .LogicOpEnable = FALSE,
            .SrcBlend = D3D12_BLEND_BLEND_FACTOR,
            .DestBlend = D3D12_BLEND_ONE,
            .BlendOp = D3D12_BLEND_OP_ADD,
            .SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR,
            .DestBlendAlpha = D3D12_BLEND_ONE,
            .BlendOpAlpha = D3D12_BLEND_OP_ADD,
            .LogicOp = D3D12_LOGIC_OP_NOOP,
            .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
        };
        composite_desc.SampleMask = UINT_MAX;
        composite_desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        composite_desc.DepthStencilState = { .DepthEnable = FALSE, .StencilEnable = FALSE };
        composite_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        composite_desc.NumRenderTargets = 1;
        composite_desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        composite_desc.SampleDesc = { .Count = 1 };
        res = m_pDevice->CreateGraphicsPipelineState(&composite_desc, IID_PPV_ARGS(&m_pCompositePipelineState));
        if (FAILED(res)) return res;
    }

    {
        ComPtr<ID3DBlob> extract_cs_blob;
        res = CompileBloomExtractShader(&extract_cs_blob);
        if (FAILED(res)) return res;

        D3D12_DESCRIPTOR_RANGE extract_srv_range = {
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 0,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
        };
        D3D12_DESCRIPTOR_RANGE extract_uav_range = {
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 0,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
        };
        D3D12_ROOT_PARAMETER extract_params[3] = {};
        extract_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        extract_params[0].Constants.ShaderRegister = 0;
        extract_params[0].Constants.RegisterSpace = 0;
        extract_params[0].Constants.Num32BitValues = 4;
        extract_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        extract_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        extract_params[1].DescriptorTable.NumDescriptorRanges = 1;
        extract_params[1].DescriptorTable.pDescriptorRanges = &extract_srv_range;
        extract_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        extract_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        extract_params[2].DescriptorTable.NumDescriptorRanges = 1;
        extract_params[2].DescriptorTable.pDescriptorRanges = &extract_uav_range;
        extract_params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC extract_rs_desc = {
            .NumParameters = 3,
            .pParameters = extract_params,
            .NumStaticSamplers = 0,
            .pStaticSamplers = nullptr,
            .Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE,
        };
        ComPtr<ID3DBlob> extract_rs_blob, extract_rs_err;
        res = D3D12SerializeRootSignature(&extract_rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &extract_rs_blob, &extract_rs_err);
        if (FAILED(res)) return res;
        res = m_pDevice->CreateRootSignature(0, extract_rs_blob->GetBufferPointer(), extract_rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_pBloomExtractRootSignature));
        if (FAILED(res)) return res;

        D3D12_COMPUTE_PIPELINE_STATE_DESC extract_ps_desc = {
            .pRootSignature = m_pBloomExtractRootSignature.Get(),
            .CS = { .pShaderBytecode = extract_cs_blob->GetBufferPointer(), .BytecodeLength = extract_cs_blob->GetBufferSize() },
            .NodeMask = 0,
        };
        res = m_pDevice->CreateComputePipelineState(&extract_ps_desc, IID_PPV_ARGS(&m_pBloomExtractPipelineState));
        if (FAILED(res)) return res;
    }

    {
        D3D12_DESCRIPTOR_RANGE bloom_srv_ranges[1] = {
            {
                .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                .NumDescriptors = 1,
                .BaseShaderRegister = 0,
                .RegisterSpace = 0,
                .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND,
            },
        };
        D3D12_ROOT_PARAMETER bloom_params[2] = {};
        bloom_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        bloom_params[0].Constants.ShaderRegister = 0;
        bloom_params[0].Constants.RegisterSpace = 0;
        bloom_params[0].Constants.Num32BitValues = 4;
        bloom_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        bloom_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        bloom_params[1].DescriptorTable.NumDescriptorRanges = _countof(bloom_srv_ranges);
        bloom_params[1].DescriptorTable.pDescriptorRanges = bloom_srv_ranges;
        bloom_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC bloom_sampler = {
            .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .MipLODBias = 0,
            .MaxAnisotropy = 0,
            .ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER,
            .BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK,
            .MinLOD = 0,
            .MaxLOD = D3D12_FLOAT32_MAX,
            .ShaderRegister = 0,
            .RegisterSpace = 0,
            .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL,
        };
        D3D12_ROOT_SIGNATURE_DESC bloom_rs_desc = {
            .NumParameters = _countof(bloom_params),
            .pParameters = bloom_params,
            .NumStaticSamplers = 1,
            .pStaticSamplers = &bloom_sampler,
            .Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS,
        };
        ComPtr<ID3DBlob> bloom_rs_blob, bloom_rs_err;
        res = D3D12SerializeRootSignature(&bloom_rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &bloom_rs_blob, &bloom_rs_err);
        if (FAILED(res)) return res;
        res = m_pDevice->CreateRootSignature(0, bloom_rs_blob->GetBufferPointer(), bloom_rs_blob->GetBufferSize(), IID_PPV_ARGS(&m_pBloomRootSignature));
        if (FAILED(res)) return res;

        ComPtr<ID3DBlob> bloom_ps_blob;
        res = CompileBloomShader(&bloom_ps_blob);
        if (FAILED(res)) return res;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC bloom_desc = {};
        bloom_desc.pRootSignature = m_pBloomRootSignature.Get();
        bloom_desc.VS = { .pShaderBytecode = g_pBackgroundVertexShader, .BytecodeLength = sizeof(g_pBackgroundVertexShader) };
        bloom_desc.PS = { .pShaderBytecode = bloom_ps_blob->GetBufferPointer(), .BytecodeLength = bloom_ps_blob->GetBufferSize() };
        bloom_desc.BlendState.RenderTarget[0] = {
            .BlendEnable = TRUE,
            .LogicOpEnable = FALSE,
            .SrcBlend = D3D12_BLEND_BLEND_FACTOR,
            .DestBlend = D3D12_BLEND_ONE,
            .BlendOp = D3D12_BLEND_OP_ADD,
            .SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR,
            .DestBlendAlpha = D3D12_BLEND_ONE,
            .BlendOpAlpha = D3D12_BLEND_OP_ADD,
            .LogicOp = D3D12_LOGIC_OP_NOOP,
            .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL,
        };
        bloom_desc.SampleMask = UINT_MAX;
        bloom_desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        bloom_desc.DepthStencilState = { .DepthEnable = FALSE, .StencilEnable = FALSE };
        bloom_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        bloom_desc.NumRenderTargets = 1;
        bloom_desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        bloom_desc.SampleDesc = { .Count = 1 };
        res = m_pDevice->CreateGraphicsPipelineState(&bloom_desc, IID_PPV_ARGS(&m_pBloomPipelineState));
        if (FAILED(res)) return res;
    }

    D3D12_RESOURCE_DESC tex_desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = (UINT64)m_iBufferWidth,
        .Height = (UINT)m_iBufferHeight,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_NONE,
    };

    D3D12_HEAP_PROPERTIES heap_default = {
        .Type = D3D12_HEAP_TYPE_DEFAULT,
        .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
        .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
        .CreationNodeMask = 1,
        .VisibleNodeMask = 1,
    };

    res = m_pDevice->CreateCommittedResource(&heap_default, D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_pSceneCopy));
    if (FAILED(res)) return res;

    tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    res = m_pDevice->CreateCommittedResource(&heap_default, D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_pBlurTemp));
    if (FAILED(res)) return res;

    res = m_pDevice->CreateCommittedResource(&heap_default, D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_pBlurOutput));
    if (FAILED(res)) return res;

    D3D12_RESOURCE_DESC bg_tex_desc = tex_desc;
    bg_tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    bg_tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    res = m_pDevice->CreateCommittedResource(&heap_default, D3D12_HEAP_FLAG_NONE, &bg_tex_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_pBGBufferA));
    if (FAILED(res)) return res;
    res = m_pDevice->CreateCommittedResource(&heap_default, D3D12_HEAP_FLAG_NONE, &bg_tex_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_pBGBufferB));
    if (FAILED(res)) return res;

    D3D12_DESCRIPTOR_HEAP_DESC bgblur_heap_desc = {
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        .NumDescriptors = 4,
        .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
        .NodeMask = 0,
    };
    res = m_pDevice->CreateDescriptorHeap(&bgblur_heap_desc, IID_PPV_ARGS(&m_pBGBlurHeap));
    if (FAILED(res)) return res;

    m_iBloomHalfWidth = max(1, m_iBufferWidth / 2);
    m_iBloomHalfHeight = max(1, m_iBufferHeight / 2);

    D3D12_RESOURCE_DESC bloom_tex_desc = {
        .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
        .Alignment = 0,
        .Width = (UINT64)m_iBloomHalfWidth,
        .Height = (UINT)m_iBloomHalfHeight,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
        .SampleDesc = { .Count = 1, .Quality = 0 },
        .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN,
        .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
    };

    res = m_pDevice->CreateCommittedResource(&heap_default, D3D12_HEAP_FLAG_NONE, &bloom_tex_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_pBloomHalf));
    if (FAILED(res)) return res;

    res = m_pDevice->CreateCommittedResource(&heap_default, D3D12_HEAP_FLAG_NONE, &bloom_tex_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_pBloomBlurTemp));
    if (FAILED(res)) return res;

    res = m_pDevice->CreateCommittedResource(&heap_default, D3D12_HEAP_FLAG_NONE, &bloom_tex_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_pBloomBlurResult));
    if (FAILED(res)) return res;

    auto srv_inc = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto heap_start_cpu = m_pImGuiSRVDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    auto heap_start_gpu = m_pImGuiSRVDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1, .PlaneSlice = 0, .ResourceMinLODClamp = 0 },
    };

    m_BlurOutputSRVCPU = { heap_start_cpu.ptr + 1 * srv_inc };
    m_BlurOutputSRVGPU = { heap_start_gpu.ptr + 1 * srv_inc };
    m_pDevice->CreateShaderResourceView(m_pBlurOutput.Get(), &srv_desc, m_BlurOutputSRVCPU);
    m_BlurTextureID = (ImTextureID)m_BlurOutputSRVGPU.ptr;

    m_BlurSceneSRVCPU = { heap_start_cpu.ptr + 2 * srv_inc };
    m_BlurSceneSRVGPU = { heap_start_gpu.ptr + 2 * srv_inc };
    m_pDevice->CreateShaderResourceView(m_pSceneCopy.Get(), &srv_desc, m_BlurSceneSRVCPU);

    m_BlurTempSRVCPU = { heap_start_cpu.ptr + 3 * srv_inc };
    m_BlurTempSRVGPU = { heap_start_gpu.ptr + 3 * srv_inc };
    m_pDevice->CreateShaderResourceView(m_pBlurTemp.Get(), &srv_desc, m_BlurTempSRVCPU);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D,
        .Texture2D = { .MipSlice = 0, .PlaneSlice = 0 },
    };

    m_BlurTempUAVCPU = { heap_start_cpu.ptr + 4 * srv_inc };
    m_BlurTempUAVGPU = { heap_start_gpu.ptr + 4 * srv_inc };
    m_pDevice->CreateUnorderedAccessView(m_pBlurTemp.Get(), nullptr, &uav_desc, m_BlurTempUAVCPU);

    m_BlurOutputUAVCPU = { heap_start_cpu.ptr + 5 * srv_inc };
    m_BlurOutputUAVGPU = { heap_start_gpu.ptr + 5 * srv_inc };
    m_pDevice->CreateUnorderedAccessView(m_pBlurOutput.Get(), nullptr, &uav_desc, m_BlurOutputUAVCPU);

    D3D12_SHADER_RESOURCE_VIEW_DESC bg_srv_desc = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1, .PlaneSlice = 0, .ResourceMinLODClamp = 0 },
    };
    D3D12_UNORDERED_ACCESS_VIEW_DESC bg_uav_desc = {
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D,
        .Texture2D = { .MipSlice = 0, .PlaneSlice = 0 },
    };

    {
        auto bgcpu = m_pBGBlurHeap->GetCPUDescriptorHandleForHeapStart();
        auto bggpu = m_pBGBlurHeap->GetGPUDescriptorHandleForHeapStart();
        m_BGBgTexSRVCPU = bgcpu;
        m_BGBgTexSRVGPU = bggpu;
        m_BGBgAUAVCPU = { bgcpu.ptr + 1 * srv_inc };
        m_BGBgAUAVGPU = { bggpu.ptr + 1 * srv_inc };
        m_BGBgASRVCPU = { bgcpu.ptr + 2 * srv_inc };
        m_BGBgASRVGPU = { bggpu.ptr + 2 * srv_inc };
        m_BGBgBUAVCPU = { bgcpu.ptr + 3 * srv_inc };
        m_BGBgBUAVGPU = { bggpu.ptr + 3 * srv_inc };
        m_pDevice->CreateShaderResourceView(m_pTextureBuffer.Get(), &bg_srv_desc, m_BGBgTexSRVCPU);
        m_pDevice->CreateUnorderedAccessView(m_pBGBufferA.Get(), nullptr, &bg_uav_desc, m_BGBgAUAVCPU);
        m_pDevice->CreateShaderResourceView(m_pBGBufferA.Get(), &bg_srv_desc, m_BGBgASRVCPU);
        m_pDevice->CreateUnorderedAccessView(m_pBGBufferB.Get(), nullptr, &bg_uav_desc, m_BGBgBUAVCPU);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC bloom_hdr_srv_desc = {
        .Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
        .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1, .PlaneSlice = 0, .ResourceMinLODClamp = 0 },
    };
    D3D12_UNORDERED_ACCESS_VIEW_DESC bloom_hdr_uav_desc = {
        .Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
        .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D,
        .Texture2D = { .MipSlice = 0, .PlaneSlice = 0 },
    };

    m_BloomHalfSRVCPU = { heap_start_cpu.ptr + 6 * srv_inc };
    m_BloomHalfSRVGPU = { heap_start_gpu.ptr + 6 * srv_inc };
    m_pDevice->CreateShaderResourceView(m_pBloomHalf.Get(), &bloom_hdr_srv_desc, m_BloomHalfSRVCPU);
    m_BloomHalfUAVCPU = { heap_start_cpu.ptr + 7 * srv_inc };
    m_BloomHalfUAVGPU = { heap_start_gpu.ptr + 7 * srv_inc };
    m_pDevice->CreateUnorderedAccessView(m_pBloomHalf.Get(), nullptr, &bloom_hdr_uav_desc, m_BloomHalfUAVCPU);

    m_BloomBlurTempSRVCPU = { heap_start_cpu.ptr + 8 * srv_inc };
    m_BloomBlurTempSRVGPU = { heap_start_gpu.ptr + 8 * srv_inc };
    m_pDevice->CreateShaderResourceView(m_pBloomBlurTemp.Get(), &bloom_hdr_srv_desc, m_BloomBlurTempSRVCPU);
    m_BloomBlurTempUAVCPU = { heap_start_cpu.ptr + 9 * srv_inc };
    m_BloomBlurTempUAVGPU = { heap_start_gpu.ptr + 9 * srv_inc };
    m_pDevice->CreateUnorderedAccessView(m_pBloomBlurTemp.Get(), nullptr, &bloom_hdr_uav_desc, m_BloomBlurTempUAVCPU);

    m_BloomBlurResultSRVCPU = { heap_start_cpu.ptr + 10 * srv_inc };
    m_BloomBlurResultSRVGPU = { heap_start_gpu.ptr + 10 * srv_inc };
    m_pDevice->CreateShaderResourceView(m_pBloomBlurResult.Get(), &bloom_hdr_srv_desc, m_BloomBlurResultSRVCPU);
    m_BloomBlurResultUAVCPU = { heap_start_cpu.ptr + 11 * srv_inc };
    m_BloomBlurResultUAVGPU = { heap_start_gpu.ptr + 11 * srv_inc };
    m_pDevice->CreateUnorderedAccessView(m_pBloomBlurResult.Get(), nullptr, &bloom_hdr_uav_desc, m_BloomBlurResultUAVCPU);

    return S_OK;
}

void D3D12Renderer::DestroyBlurResources() {
    m_pBlurRootSignature.Reset();
    m_pBlurPipelineState.Reset();
    m_pBloomRootSignature.Reset();
    m_pBloomPipelineState.Reset();
    m_pBloomExtractRootSignature.Reset();
    m_pBloomExtractPipelineState.Reset();
    m_pSceneCopy.Reset();
    m_pBlurTemp.Reset();
    m_pBlurOutput.Reset();
    m_pBGBlurHeap.Reset();
    m_pBGBufferA.Reset();
    m_pBGBufferB.Reset();
    m_pBloomHalf.Reset();
    m_pBloomBlurTemp.Reset();
    m_pBloomBlurResult.Reset();
    m_BlurTextureID = 0;
}

void D3D12Renderer::ApplyBlur() {
    // Guard against partial creation (a failed blur rebuild during a TDR recovery
    // can leave some members null) and skip entirely while the loading modal is
    // up: the scene behind it is frozen and the load is exactly when the driver
    // is most at risk, so there is no reason to dispatch compute here.
    if (g_bShowLoading || !m_pBlurPipelineState || !m_pSceneCopy || !m_pBlurTemp ||
        !m_pBlurOutput || !m_pBlurRootSignature)
        return;

    auto* backbuffer = m_pRenderTargets[m_uFrameIndex].Get();

    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_pCommandList->ResourceBarrier(1, &barrier);
    }
    if (m_SceneCopyState != D3D12_RESOURCE_STATE_COPY_DEST) {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pSceneCopy.Get(), m_SceneCopyState, D3D12_RESOURCE_STATE_COPY_DEST);
        m_pCommandList->ResourceBarrier(1, &barrier);
    }
    m_SceneCopyState = D3D12_RESOURCE_STATE_COPY_DEST;

    m_pCommandList->CopyResource(m_pSceneCopy.Get(), backbuffer);

    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_pCommandList->ResourceBarrier(1, &barrier);
    }
    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pSceneCopy.Get(), m_SceneCopyState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_pCommandList->ResourceBarrier(1, &barrier);
    }
    m_SceneCopyState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pBlurTemp.Get(), m_BlurTempState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_pCommandList->ResourceBarrier(1, &barrier);
    }
    m_BlurTempState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    m_pCommandList->SetComputeRootSignature(m_pBlurRootSignature.Get());
    m_pCommandList->SetPipelineState(m_pBlurPipelineState.Get());

    ID3D12DescriptorHeap* heaps[] = { m_pImGuiSRVDescriptorHeap.Get() };
    m_pCommandList->SetDescriptorHeaps(1, heaps);

    const VizSettings& blurViz = Config::GetConfig().GetVizSettings();
    const int sigma = blurViz.bBloom ? max(1, (int)(blurViz.fBloomSpread + 0.5f)) : 10;
    UINT blur_consts[4] = { 0, (UINT)sigma, 0, 0 };
    m_pCommandList->SetComputeRoot32BitConstants(0, 4, blur_consts, 0);

    m_pCommandList->SetComputeRootDescriptorTable(1, m_BlurSceneSRVGPU);
    m_pCommandList->SetComputeRootDescriptorTable(2, m_BlurTempUAVGPU);

    UINT groups_x = (m_iBufferWidth + 255) / 256;
    m_pCommandList->Dispatch(groups_x, m_iBufferHeight, 1);

    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pBlurTemp.Get(), m_BlurTempState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_pCommandList->ResourceBarrier(1, &barrier);
    }
    m_BlurTempState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pBlurOutput.Get(), m_BlurOutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_pCommandList->ResourceBarrier(1, &barrier);
    }
    m_BlurOutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    blur_consts[0] = 1;
    m_pCommandList->SetComputeRoot32BitConstants(0, 4, blur_consts, 0);

    m_pCommandList->SetComputeRootDescriptorTable(1, m_BlurTempSRVGPU);
    m_pCommandList->SetComputeRootDescriptorTable(2, m_BlurOutputUAVGPU);

    UINT groups_y = (m_iBufferHeight + 255) / 256;
    m_pCommandList->Dispatch(m_iBufferWidth, groups_y, 1);

    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pBlurOutput.Get(), m_BlurOutputState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_pCommandList->ResourceBarrier(1, &barrier);
    }
    m_BlurOutputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pBlurTemp.Get(), m_BlurTempState, D3D12_RESOURCE_STATE_COMMON);
        m_pCommandList->ResourceBarrier(1, &barrier);
    }
    m_BlurTempState = D3D12_RESOURCE_STATE_COMMON;

    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_pSceneCopy.Get(), m_SceneCopyState, D3D12_RESOURCE_STATE_COPY_DEST);
        m_pCommandList->ResourceBarrier(1, &barrier);
    }
    m_SceneCopyState = D3D12_RESOURCE_STATE_COPY_DEST;
}

void D3D12Renderer::ImGuiStartFrame()
{
    const auto& viz = Config::GetConfig().GetVizSettings();
    if (m_fLastUIScale != viz.fUIScale || m_sLastFont != viz.sUIFont)
        UpdateImGuiSettings();
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    RenderImGuiFrame();
}

void D3D12Renderer::RenderImGuiFrame() {
    auto& io = ImGui::GetIO();
    auto& viz = Config::GetConfig().GetVizSettings();
    if (io.KeyCtrl && io.KeyAlt &&
        (ImGui::IsKeyPressed(ImGuiKey_LeftCtrl, false) || ImGui::IsKeyPressed(ImGuiKey_RightCtrl, false) ||
         ImGui::IsKeyPressed(ImGuiKey_LeftAlt, false) || ImGui::IsKeyPressed(ImGuiKey_RightAlt, false)))
        viz.bDisableUI = false;
    if (viz.bDisableUI) return;

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
            ImGui::Checkbox("Colored Ribbon", &viz.bColoredRibbon);
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
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + menuBarHeight));
            ImGui::SetNextWindowSize(ImVec2(vp->Size.x, toolbarHeight));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGuiWindowFlags tbFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoNav;

if (ImGui::Begin("##Toolbar", &m_bShowToolbar, tbFlags)) {
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

                bool muted = playback.GetMute();
                if (ImGui::Button(muted ? "Unmute" : "Mute", ImVec2(55, 0)))
                    playback.ToggleMute(true);

                ImGui::SameLine(); ImGui::Separator(); ImGui::SameLine();

                ImGui::BeginDisabled(!hasSong);
                static int seekPos = 0;
                if (m_fPlaybackPosition < 0.001f) seekPos = 0;
                if (!ImGui::IsItemActive()) {
                    seekPos = (int)(m_fPlaybackPosition * 1000.0f);
                }
                float availW = ImGui::GetContentRegionAvail().x;
                float sliderW = max(availW - 350.0f, 150.0f);
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
                    int renderer = vis.eRenderer;
                    if (ImGui::Combo("Renderer", &renderer, "Direct3D\0OpenGL\0GDI\0"))
                        vis.eRenderer = (VideoSettings::Renderer)renderer;
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
                    ImGui::Checkbox("Tick-based Mode", &viz.bTickBased);
                    ImGui::Checkbox("Show Markers", &viz.bShowMarkers);
                    ImGui::Combo("Marker Encoding", (int*)&viz.eMarkerEncoding, "CP-1252 (Western)\0CP-932 (Japanese)\0UTF-8\0");
                    ImGui::Checkbox("Nerd Stats", &viz.bNerdStats);
                    ImGui::Checkbox("Visualize Pitch Bends", &viz.bVisualizePitchBends);
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
            snprintf(buf, sizeof(buf), "%llu / %llu", progLoaded, progMax);
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

void D3D12Renderer::UpdateImGuiSettings() {
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
    ImGui_ImplDX12_CreateDeviceObjects();

    auto& style = ImGui::GetStyle();
    style.WindowMinSize = ImVec2(1, 1);
    style.WindowBorderSize = 0.0f;
    style.WindowPadding = ImVec2(6 * scale, 6 * scale);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.50f);
}
