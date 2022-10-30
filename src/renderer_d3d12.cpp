#include <dxgi1_4.h>
#include <agility/d3d12.h>
#include <dxc/dxcapi.h>

#include "renderer.h"

extern "C" __declspec(dllexport) extern const UINT D3D12SDKVersion = 606;
extern "C" __declspec(dllexport) extern const char* D3D12SDKPath = u8"./d3d12/";

#define RENDERER_MEMORY_ARENA_SIZE (3 * 1024 * 1024)

#define MAX_COMMAND_LIST_COUNT 16
#define MAX_RTV_COUNT 1024
#define MAX_DSV_COUNT 1024
#define BINDLESS_HEAP_CAPACITY 1000000

#define CONSTANT_BUFFER_SIZE 256
#define CONSTANT_BUFFER_POOL_SIZE 256

#define MESH_MEMORY_COUNT (8 * 1024)

struct Descriptor {
    #if _DEBUG
        u32 meta;
    #endif
    u32 index;
};

struct DescriptorHeap {
    #if _DEBUG
        u16 id;
        u32 size;
        u16* versions;
    #endif
    int free_count;
    u32* free_list;
    u64 stride;
    ID3D12DescriptorHeap* heap;
    u64 base_cpu;
    u64 base_gpu;
};

internal DescriptorHeap descriptor_heap_init(Arena* arena, ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 size, b32 shader_visible, u16 id) {
    UNUSED(id);

    DescriptorHeap heap = {};

    #if _DEBUG
        heap.id = id;
        heap.size = size;

        heap.versions = arena_push_array(arena, u16, size);
        for (u32 i = 0; i < size; ++i) {
            heap.versions[i] = 1;
        }
    #endif
    
    heap.free_list = arena_push_array(arena, u32, size);
    for (u32 i = 0; i < size; ++i) {
        heap.free_list[heap.free_count++] = i;
    }

    heap.stride = device->GetDescriptorHandleIncrementSize(type);

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = type;
    heap_desc.NumDescriptors = size;
    heap_desc.Flags = shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap.heap));

    heap.base_cpu = heap.heap->GetCPUDescriptorHandleForHeapStart().ptr;

    if (shader_visible) {
        heap.base_gpu = heap.heap->GetGPUDescriptorHandleForHeapStart().ptr;
    }

    return heap;
}

internal Descriptor alloc_descriptor(DescriptorHeap* heap) {
    Descriptor descriptor;

    assert(heap->free_count > 0);
    u32 index = heap->free_list[--heap->free_count];

    descriptor.index = index;
    
    #if _DEBUG
        descriptor.meta = (heap->id << 16) | heap->versions[index];
    #endif

    return descriptor;
}

#if _DEBUG
    internal void validate_descriptor(DescriptorHeap* heap, Descriptor descriptor) {
        UNUSED(heap);
        UNUSED(descriptor);

        assert(descriptor.index < heap->size);
        assert((descriptor.meta >> 16) == heap->id);
        assert((descriptor.meta & UINT16_MAX) == heap->versions[descriptor.index]);
    }
#else
    #define validate_descriptor(heap, descriptor) 0
#endif

internal void free_descriptor(DescriptorHeap* heap, Descriptor descriptor) {
    validate_descriptor(heap, descriptor);

    #if _DEBUG
        ++heap->versions[descriptor.index];
    #endif

    heap->free_list[heap->free_count++] = descriptor.index;
}

internal D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor_handle(DescriptorHeap* heap, Descriptor descriptor) {
    validate_descriptor(heap, descriptor);
    return { heap->base_cpu + heap->stride * descriptor.index };
}

internal D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor_handle(DescriptorHeap* heap, Descriptor descriptor) {
    validate_descriptor(heap, descriptor);
    return { heap->base_gpu + heap->stride * descriptor.index };
}

struct ConstantBuffer {
    ConstantBuffer* next;
    void* ptr;
    Descriptor view;
};

struct CommandList {
    u64 fence_val;
    ID3D12CommandAllocator* allocator;
    ID3D12GraphicsCommandList* list;
    ConstantBuffer* constant_buffer_list;
};

struct ReleasableResource {
    ReleasableResource* next;
    ID3D12Resource* resource;
};

struct MeshData {
    ID3D12Resource* vbuffer;
    ID3D12Resource* ibuffer;
    Descriptor vbuffer_view;
    Descriptor ibuffer_view;
    u32 index_count;
};

struct Renderer {
    Arena arena;

    IDXGIFactory3* factory;
    IDXGIAdapter* adapter;
    ID3D12Device* device;

    ID3D12CommandQueue* queue;
    ID3D12Fence* fence;
    u64 fence_counter;

    DescriptorHeap rtv_heap;
    DescriptorHeap dsv_heap;
    DescriptorHeap bindless_heap;

    ID3D12RootSignature* root_signature;

    IDXGISwapChain3* swapchain;
    ID3D12Resource* swapchain_buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    u64 swapchain_fences[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    Descriptor swapchain_rtvs[DXGI_MAX_SWAP_CHAIN_BUFFERS];

    ConstantBuffer* available_constant_buffer_list;

    int num_available_command_lists;
    int num_executing_command_lists;
    CommandList available_command_lists[MAX_COMMAND_LIST_COUNT];
    CommandList executing_command_lists[MAX_COMMAND_LIST_COUNT];

    ReleasableResource* releasable_resource_list;

    ID3D12PipelineState* pipeline;
    ID3D12Resource* depth_buffer;
    Descriptor depth_view;

    int num_free_meshes;
    u32 mesh_free_list[MESH_MEMORY_COUNT];
    MeshData mesh_data[MESH_MEMORY_COUNT];
    #if _DEBUG
        u16 mesh_versions[MESH_MEMORY_COUNT];
    #endif
};

internal u64 fence_signal(Renderer* r) {
    u64 val = ++r->fence_counter;
    r->queue->Signal(r->fence, val);
    return val;
}

internal void fence_wait(Renderer* r, u64 val) {
    if (r->fence->GetCompletedValue() < val) {
        r->fence->SetEventOnCompletion(val, 0);
    }
}

internal b32 fence_reached(Renderer* r, u64 val) {
    return r->fence->GetCompletedValue() >= val;
}

internal void wait_device_idle(Renderer* r) {
    fence_wait(r, fence_signal(r));
}

internal void update_available_command_lists(Renderer* r) { 
    for (int i = r->num_executing_command_lists - 1; i >= 0; --i)
    {
        CommandList* cmd = &r->executing_command_lists[i];
        if (fence_reached(r, cmd->fence_val)) {
            if (cmd->constant_buffer_list) {
                ConstantBuffer* n = cmd->constant_buffer_list;
                while (n->next) {
                    n = n->next;
                }
                n->next = r->available_constant_buffer_list;
                r->available_constant_buffer_list = cmd->constant_buffer_list;
                cmd->constant_buffer_list = NULL;
            }

            assert(r->num_available_command_lists < MAX_COMMAND_LIST_COUNT);
            r->available_command_lists[r->num_available_command_lists++] = *cmd;
            r->executing_command_lists[i] = r->executing_command_lists[--r->num_executing_command_lists];
        }
    }
}

internal CommandList open_command_list(Renderer* r) {
    update_available_command_lists(r);

    if (r->num_available_command_lists == 0) {
        CommandList cmd = {};

        r->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd.allocator));
        r->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd.allocator, 0, IID_PPV_ARGS(&cmd.list));
        cmd.list->Close();

        debug_message("Allocated a command list.\n");

        r->available_command_lists[r->num_available_command_lists++] = cmd;
    }

    CommandList cmd = r->available_command_lists[--r->num_available_command_lists];

    cmd.allocator->Reset();
    cmd.list->Reset(cmd.allocator, 0);

    cmd.list->SetGraphicsRootSignature(r->root_signature);
    cmd.list->SetDescriptorHeaps(1, &r->bindless_heap.heap);

    return cmd;
}

internal void submit_command_list(Renderer* r, CommandList cmd) {
    cmd.list->Close();

    ID3D12CommandList* decayed = cmd.list;
    r->queue->ExecuteCommandLists(1, &decayed);
    cmd.fence_val = fence_signal(r);
    
    assert(r->num_executing_command_lists < MAX_COMMAND_LIST_COUNT);
    r->executing_command_lists[r->num_executing_command_lists++] = cmd;
}

internal void get_swapchain_buffers_and_create_rtvs(Renderer* r) {
    DXGI_SWAP_CHAIN_DESC1 desc;
    r->swapchain->GetDesc1(&desc);

    for (u32 i = 0; i < desc.BufferCount; ++i) {
        r->swapchain->GetBuffer(i, IID_PPV_ARGS(&r->swapchain_buffers[i]));
        
        D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = {};
        rtv_desc.Format = desc.Format;
        rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_descriptor_handle(&r->rtv_heap, r->swapchain_rtvs[i]);
        r->device->CreateRenderTargetView(r->swapchain_buffers[i], &rtv_desc, handle);
    }
}

internal void release_swapchain_buffers(Renderer* r) {
    DXGI_SWAP_CHAIN_DESC1 desc;
    r->swapchain->GetDesc1(&desc);

    for (u32 i = 0; i < desc.BufferCount; ++i) {
        r->swapchain_buffers[i]->Release();
    }
}

internal IDxcBlob* compile_shader(char* path, char* entry, char* target) {
    Scratch scratch = get_scratch(0, 0);

    IDxcUtils* utils = nullptr;
    DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));

    IDxcCompiler3* compiler = nullptr;
    DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    
    IDxcIncludeHandler* include_handler = nullptr;
    utils->CreateDefaultIncludeHandler(&include_handler);

    wchar_t w_path[512];
    MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, path, -1, w_path, sizeof(w_path));

    wchar_t w_entry[512];
    MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, entry, -1, w_entry, sizeof(w_entry));

    wchar_t w_target[512];
    MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, target, -1, w_target, sizeof(w_target));
    
    LPCWSTR args[] = {
        w_path, 
        L"-E", w_entry,
        L"-T", w_target,
        L"-Zi",
    };

    ReadFileResult file = read_file(scratch.arena, path);

    DxcBuffer source;
    source.Ptr = file.memory;
    source.Size = file.size;
    source.Encoding = DXC_CP_ACP;

    IDxcResult* result = nullptr;
    compiler->Compile(&source, args, ARRAY_LEN(args), include_handler, IID_PPV_ARGS(&result));

    IDxcBlobUtf8* errors = nullptr;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);

    bool had_errors = false;

    if (errors) {
        if (errors->GetStringLength() != 0) {
            had_errors = true;
            debug_message("Shader errors:\n%s", (char*)errors->GetStringPointer());
        }
        errors->Release();
    }

    IDxcBlob* shader = nullptr;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shader), nullptr);

    result->Release();
    include_handler->Release();
    compiler->Release();
    utils->Release();

    release_scratch(scratch);

    if (had_errors) {
        shader->Release();
        return NULL;
    }
    else {
        return shader;
    }
}

internal void create_depth_buffer(Renderer* r, u32 width, u32 height) {
    D3D12_RESOURCE_DESC resource_desc = {};
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Width = width;
    resource_desc.Height = height;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_R32_TYPELESS;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_HEAP_PROPERTIES heap_props = {};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    r->device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, 0, IID_PPV_ARGS(&r->depth_buffer));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {};
    dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

    r->device->CreateDepthStencilView(r->depth_buffer, &dsv_desc, cpu_descriptor_handle(&r->dsv_heap, r->depth_view));
}

Renderer* renderer_init(Arena* arena, void* window) {
    Renderer* r = arena_push_struct_zero(arena, Renderer);

    r->arena = arena_init(arena_push(arena, RENDERER_MEMORY_ARENA_SIZE), RENDERER_MEMORY_ARENA_SIZE);

    #if _DEBUG
    {
        ID3D12Debug* debug_interface;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_interface)))) {
            debug_interface->EnableDebugLayer();
            debug_interface->Release();
        }
    }
    #endif

    if (FAILED(CreateDXGIFactory(IID_PPV_ARGS(&r->factory)))) {
        system_message_box("Failed to create DXGI device.");
    }

    if (FAILED(r->factory->EnumAdapters(0, &r->adapter))) {
        system_message_box("Failed to get DXGI adapter.");
    }

    if (FAILED(D3D12CreateDevice(r->adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&r->device)))) {
        system_message_box("Failed to create D3D12 device.");
    }

    #if _DEBUG
    {
        ID3D12InfoQueue* info_queue;
        if (SUCCEEDED(r->device->QueryInterface(&info_queue))) {
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

            D3D12_MESSAGE_SEVERITY severity_filter = D3D12_MESSAGE_SEVERITY_INFO;

            D3D12_MESSAGE_ID message_filter[] = {
                D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
                D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,
                D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,
                D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_DEPTHSTENCILVIEW_NOT_SET
            };

            D3D12_INFO_QUEUE_FILTER filter = {};
            filter.DenyList.NumSeverities = 1;
            filter.DenyList.pSeverityList = &severity_filter;
            filter.DenyList.NumIDs = ARRAY_LEN(message_filter);
            filter.DenyList.pIDList = message_filter;

            info_queue->PushStorageFilter(&filter);
            info_queue->Release();
        }
    }
    #endif

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    r->device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&r->queue));
    r->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&r->fence));

    r->rtv_heap = descriptor_heap_init(arena, r->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, MAX_RTV_COUNT, false, 1);
    r->dsv_heap = descriptor_heap_init(arena, r->device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, MAX_DSV_COUNT, false, 2);
    r->bindless_heap = descriptor_heap_init(arena, r->device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, BINDLESS_HEAP_CAPACITY, true, 3);

    D3D12_ROOT_PARAMETER root_param = {};
    root_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    root_param.Constants.Num32BitValues = 16;

    D3D12_ROOT_SIGNATURE_DESC root_signature_desc = {};
    root_signature_desc.NumParameters = 1;
    root_signature_desc.pParameters = &root_param;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

    ID3DBlob* root_signature_data;
    D3D12SerializeRootSignature(&root_signature_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &root_signature_data, 0);
    r->device->CreateRootSignature(0, root_signature_data->GetBufferPointer(), root_signature_data->GetBufferSize(), IID_PPV_ARGS(&r->root_signature));
    root_signature_data->Release();

    RECT client_rect;
    GetClientRect((HWND)window, &client_rect);

    DXGI_SWAP_CHAIN_DESC1 swapchain_desc = {};
    swapchain_desc.Width = client_rect.right - client_rect.left;
    swapchain_desc.Height = client_rect.bottom - client_rect.top;
    swapchain_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapchain_desc.SampleDesc.Count = 1;
    swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchain_desc.BufferCount = 2;
    swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    IDXGISwapChain1* swapchain_1;
    if (FAILED(r->factory->CreateSwapChainForHwnd(r->queue, (HWND)window, &swapchain_desc, 0, 0, &swapchain_1))) {
        system_message_box("Failed to create D3D12 swapchain.");
    }
    swapchain_1->QueryInterface(&r->swapchain);
    swapchain_1->Release();

    for (int i = 0; i < DXGI_MAX_SWAP_CHAIN_BUFFERS; ++i) {
        r->swapchain_rtvs[i] = alloc_descriptor(&r->rtv_heap);
    }

    get_swapchain_buffers_and_create_rtvs(r);

    IDxcBlob* vs = compile_shader("shader.hlsl", "vs_main", "vs_6_6");
    IDxcBlob* ps = compile_shader("shader.hlsl", "ps_main", "ps_6_6");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = r->root_signature;

    pso_desc.VS.BytecodeLength = vs->GetBufferSize();
    pso_desc.VS.pShaderBytecode = vs->GetBufferPointer();
    pso_desc.PS.BytecodeLength = ps->GetBufferSize();
    pso_desc.PS.pShaderBytecode = ps->GetBufferPointer();

    for (int i = 0; i < ARRAY_LEN(pso_desc.BlendState.RenderTarget); ++i) {
        D3D12_RENDER_TARGET_BLEND_DESC* blend = pso_desc.BlendState.RenderTarget + i;
        blend->SrcBlend = D3D12_BLEND_ONE;
        blend->DestBlend = D3D12_BLEND_ZERO;
        blend->BlendOp = D3D12_BLEND_OP_ADD;
        blend->SrcBlendAlpha = D3D12_BLEND_ONE;
        blend->DestBlendAlpha = D3D12_BLEND_ZERO;
        blend->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend->LogicOp = D3D12_LOGIC_OP_NOOP;
        blend->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    pso_desc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;

    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.RasterizerState.FrontCounterClockwise = TRUE;

    pso_desc.DepthStencilState.DepthEnable = TRUE;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;

    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

    pso_desc.SampleDesc.Count = 1;

    r->device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&r->pipeline));

    vs->Release();
    ps->Release();

    r->depth_view = alloc_descriptor(&r->dsv_heap);
    create_depth_buffer(r, swapchain_desc.Width, swapchain_desc.Height);

    for (int i = 0; i < MESH_MEMORY_COUNT; ++i) {
        r->mesh_free_list[r->num_free_meshes++] = i;
        #if _DEBUG
        r->mesh_versions[i] = 1;
        #endif
    }

    return r;
}

void renderer_release_backend(Renderer* r) {
    UNUSED(r);
#if _DEBUG
    wait_device_idle(r);

    update_available_command_lists(r);
    assert(r->num_executing_command_lists == 0);

    for (int i = 0; i < r->num_available_command_lists; ++i) {
        r->available_command_lists[i].allocator->Release();
        r->available_command_lists[i].list->Release();
    }

    for (int i = 0; i < MESH_MEMORY_COUNT; ++i) {
        if (r->mesh_data[i].vbuffer) {
            r->mesh_data[i].vbuffer->Release();
            r->mesh_data[i].ibuffer->Release();
        }
    }

    r->depth_buffer->Release();
    r->pipeline->Release();

    for (ReleasableResource* n = r->releasable_resource_list; n; n = n->next) {
        r->releasable_resource_list->resource->Release();
    }

    for (int i = 0; i < DXGI_MAX_SWAP_CHAIN_BUFFERS; ++i) {
        free_descriptor(&r->rtv_heap, r->swapchain_rtvs[i]);
    }

    release_swapchain_buffers(r);
    r->swapchain->Release();

    r->root_signature->Release();

    r->bindless_heap.heap->Release();
    r->dsv_heap.heap->Release();
    r->rtv_heap.heap->Release();

    r->fence->Release();
    r->queue->Release();

    r->device->Release();
    r->adapter->Release();
    r->factory->Release();
#endif
}

void renderer_handle_resize(Renderer* r, u32 width, u32 height) {
    if (width == 0 || height == 0) {
        return;
    }

    wait_device_idle(r);

    release_swapchain_buffers(r);
    r->swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    get_swapchain_buffers_and_create_rtvs(r);

    r->depth_buffer->Release();
    create_depth_buffer(r, width, height);

    debug_message("Resized swapchain (%d x %d)\n", width, height);
}

internal ConstantBuffer* get_constant_buffer(Renderer* r, void* data, u64 data_size) {
    if (!r->available_constant_buffer_list) {
        debug_message("Creating a constant buffer pool (%d constant buffers)\n", CONSTANT_BUFFER_POOL_SIZE);

        D3D12_RESOURCE_DESC resource_desc = {};
        resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resource_desc.Width = CONSTANT_BUFFER_SIZE * CONSTANT_BUFFER_POOL_SIZE;
        resource_desc.Height = 1;
        resource_desc.DepthOrArraySize = 1;
        resource_desc.MipLevels = 1;
        resource_desc.SampleDesc.Count = 1;
        resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_HEAP_PROPERTIES heap_props = {};
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;

        ID3D12Resource* resource;
        r->device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, 0, IID_PPV_ARGS(&resource));

        ReleasableResource* releasable_resource = arena_push_struct(&r->arena, ReleasableResource);
        releasable_resource->next = r->releasable_resource_list;
        releasable_resource->resource = resource;

        r->releasable_resource_list = releasable_resource;

        void* base;
        resource->Map(0, 0, &base);
        D3D12_GPU_VIRTUAL_ADDRESS base_gpu = resource->GetGPUVirtualAddress();

        for (int i = 0; i < CONSTANT_BUFFER_POOL_SIZE; ++i) {
            ConstantBuffer* cbuffer = arena_push_struct(&r->arena, ConstantBuffer);

            u64 offset = i * CONSTANT_BUFFER_SIZE;

            cbuffer->next = r->available_constant_buffer_list;
            cbuffer->ptr = (u8*)base + offset;
            cbuffer->view = alloc_descriptor(&r->bindless_heap);

            D3D12_CONSTANT_BUFFER_VIEW_DESC view_desc = {};
            view_desc.BufferLocation = base_gpu + offset;
            view_desc.SizeInBytes = CONSTANT_BUFFER_SIZE;

            r->device->CreateConstantBufferView(&view_desc, cpu_descriptor_handle(&r->bindless_heap, cbuffer->view));

            r->available_constant_buffer_list = cbuffer;
        }
    }

    ConstantBuffer* cbuffer = r->available_constant_buffer_list;
    r->available_constant_buffer_list = cbuffer->next;

    assert(data_size <= CONSTANT_BUFFER_SIZE);
    memcpy(cbuffer->ptr, data, data_size);

    return cbuffer;
}

internal void drop_constant_buffer(CommandList* cmd, ConstantBuffer* cbuffer) {
    cbuffer->next = cmd->constant_buffer_list;
    cmd->constant_buffer_list = cbuffer;
}

internal MeshData* get_mesh_data(Renderer* r, Mesh handle) {
    assert(handle.index < MESH_MEMORY_COUNT);
    assert(handle.version == r->mesh_versions[handle.index]);
    return &r->mesh_data[handle.index];
}

void renderer_render_frame(Renderer* r, MeshInstance* queue, u32 queue_len) {
    DXGI_SWAP_CHAIN_DESC1 swapchain_desc;
    r->swapchain->GetDesc1(&swapchain_desc);

    u32 swapchain_index = r->swapchain->GetCurrentBackBufferIndex();
    fence_wait(r, r->swapchain_fences[swapchain_index]);

    CommandList cmd = open_command_list(r);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = r->swapchain_buffers[swapchain_index];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    cmd.list->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = cpu_descriptor_handle(&r->rtv_heap, r->swapchain_rtvs[swapchain_index]);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = cpu_descriptor_handle(&r->dsv_heap, r->depth_view);

    f32 clear_color[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
    cmd.list->ClearRenderTargetView(rtv_handle, clear_color, 0, 0);
    cmd.list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, 0);
    cmd.list->OMSetRenderTargets(1, &rtv_handle, false, &dsv_handle);

    D3D12_RECT scissor = {};
    scissor.right = swapchain_desc.Width;
    scissor.bottom = swapchain_desc.Height;

    D3D12_VIEWPORT viewport = {};
    viewport.MaxDepth = 1.0f;
    viewport.Width = (f32)swapchain_desc.Width;
    viewport.Height = (f32)swapchain_desc.Height;

    cmd.list->RSSetScissorRects(1, &scissor);
    cmd.list->RSSetViewports(1, &viewport);

    cmd.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd.list->SetPipelineState(r->pipeline);

    XMMATRIX view_matrix = XMMatrixLookAtRH({ 0.0f, 0.0f, 3.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f });
    XMMATRIX projection_matrix = XMMatrixPerspectiveFovRH(PI32 * 0.25f, (f32)swapchain_desc.Width / (f32)swapchain_desc.Height, 1000.0f, 0.1f);
    XMMATRIX view_projection = view_matrix * projection_matrix;
    ConstantBuffer* camera_cbuffer = get_constant_buffer(r, &view_projection, sizeof(view_projection));
    drop_constant_buffer(&cmd, camera_cbuffer);
    cmd.list->SetGraphicsRoot32BitConstant(0, camera_cbuffer->view.index, 0);

    for (u32 i = 0; i < queue_len; ++i) {
        MeshInstance* mesh_sub = &queue[i];
        MeshData* mesh_data = get_mesh_data(r, mesh_sub->mesh);

        ConstantBuffer* transform_cbuffer = get_constant_buffer(r, &mesh_sub->transform, sizeof(mesh_sub->transform));
        drop_constant_buffer(&cmd, transform_cbuffer);

        cmd.list->SetGraphicsRoot32BitConstant(0, mesh_data->vbuffer_view.index, 1);
        cmd.list->SetGraphicsRoot32BitConstant(0, mesh_data->ibuffer_view.index, 2);
        cmd.list->SetGraphicsRoot32BitConstant(0, transform_cbuffer->view.index, 3);

        cmd.list->DrawInstanced(mesh_data->index_count, 1, 0, 0);
    }

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd.list->ResourceBarrier(1, &barrier);

    submit_command_list(r, cmd);

    r->swapchain->Present(1, 0);
    r->swapchain_fences[swapchain_index] = fence_signal(r);
}

Mesh renderer_new_mesh(Renderer* r, Vertex* vertex_data, u32 vertex_count, u32* index_data, u32 index_count) {
    assert(r->num_free_meshes > 0);

    u32 index = r->mesh_free_list[--r->num_free_meshes];

    u32 vertex_data_size = vertex_count * sizeof(Vertex);
    u32 index_data_size = index_count * sizeof(u32);

    MeshData* data = &r->mesh_data[index];

    D3D12_RESOURCE_DESC resource_desc = {};
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES heap_props = {};
    heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;

    resource_desc.Width = vertex_data_size;
    r->device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, 0, IID_PPV_ARGS(&data->vbuffer));

    resource_desc.Width = index_data_size;
    r->device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, 0, IID_PPV_ARGS(&data->ibuffer));

    void* mapped_ptr;

    data->vbuffer->Map(0, 0, &mapped_ptr);
    memcpy(mapped_ptr, vertex_data, vertex_data_size);
    data->vbuffer->Unmap(0, 0);
    
    data->ibuffer->Map(0, 0, &mapped_ptr);
    memcpy(mapped_ptr, index_data, index_data_size);
    data->ibuffer->Unmap(0, 0);

    data->vbuffer_view = alloc_descriptor(&r->bindless_heap);
    data->ibuffer_view = alloc_descriptor(&r->bindless_heap);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    srv_desc.Buffer.NumElements = vertex_count;
    srv_desc.Buffer.StructureByteStride = sizeof(Vertex);
    r->device->CreateShaderResourceView(data->vbuffer, &srv_desc, cpu_descriptor_handle(&r->bindless_heap, data->vbuffer_view));

    srv_desc.Buffer.NumElements = index_count;
    srv_desc.Buffer.StructureByteStride = sizeof(u32);
    r->device->CreateShaderResourceView(data->ibuffer, &srv_desc, cpu_descriptor_handle(&r->bindless_heap, data->ibuffer_view));

    data->index_count = index_count;

    Mesh handle = {};
    handle.index = index;
    #if _DEBUG
    handle.version = r->mesh_versions[index];
    #endif
    
    return handle;
}
