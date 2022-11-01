#include <dxgi1_4.h>
#include <agility/d3d12.h>
#include <dxc/dxcapi.h>

#include "renderer.h"

extern "C" __declspec(dllexport) extern const UINT D3D12SDKVersion = 606;
extern "C" __declspec(dllexport) extern const char* D3D12SDKPath = u8"./d3d12/";

#define RENDERER_MEMORY_ARENA_SIZE (3 * 1024 * 1024)

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

struct CommandQueue {
    ID3D12CommandQueue* queue;
    u64 fence_val;
    ID3D12Fence* fence;
};

struct CommandList {
    CommandList* next;
    u64 fence_val;
    D3D12_COMMAND_LIST_TYPE type;
    CommandQueue* queue;
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

internal CommandQueue command_queue_init(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type) {
    CommandQueue queue = {};

    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = type;

    device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue.queue));
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue.fence));

    return queue;
}

internal void command_queue_release(CommandQueue* queue) {
    queue->fence->Release();
    queue->queue->Release();
}

internal u64 command_queue_signal(CommandQueue* queue) {
    u64 val = ++queue->fence_val;
    queue->queue->Signal(queue->fence, val);
    return val;
};

internal bool command_queue_reached(CommandQueue* queue, u64 val) {
    return queue->fence->GetCompletedValue() >= val;
}

internal void command_queue_wait(CommandQueue* queue, u64 val) {
    if (queue->fence->GetCompletedValue() < val) {
        queue->fence->SetEventOnCompletion(val, 0);
    }
}

internal void command_queue_flush(CommandQueue* queue) {
    command_queue_wait(queue, command_queue_signal(queue));
}

struct Renderer {
    Arena arena;

    ReleasableResource* releasable_resource_slots;

    IDXGIFactory3* factory;
    IDXGIAdapter* adapter;
    ID3D12Device* device;

    CommandQueue direct_queue;
    CommandQueue copy_queue;

    DescriptorHeap rtv_heap;
    DescriptorHeap dsv_heap;
    DescriptorHeap bindless_heap;

    ID3D12RootSignature* root_signature;

    IDXGISwapChain3* swapchain;
    ID3D12Resource* swapchain_buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    u64 swapchain_fences[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    Descriptor swapchain_rtvs[DXGI_MAX_SWAP_CHAIN_BUFFERS];

    ConstantBuffer* available_constant_buffer_list;

    CommandList* available_command_lists;
    CommandList* executing_command_lists;

    #if _DEBUG
    ReleasableResource* garbage;
    #endif

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

internal void update_available_command_lists(Renderer* r) { 
    for (CommandList** p_cmd = &r->executing_command_lists; *p_cmd;)
    {
        CommandList* cmd = *p_cmd;
        if (command_queue_reached(cmd->queue, cmd->fence_val))
        {
            // Add all constant buffers back into available pool

            if (cmd->constant_buffer_list) {
                ConstantBuffer* n = cmd->constant_buffer_list;
                while (n->next) {
                    n = n->next;
                }
                n->next = r->available_constant_buffer_list;
                r->available_constant_buffer_list = cmd->constant_buffer_list;
                cmd->constant_buffer_list = NULL;
            }

            // Add command list into available list

            CommandList* next = cmd->next;
            cmd->next = r->available_command_lists;
            r->available_command_lists = cmd;
            *p_cmd = next;
        }
        else {
            p_cmd = &cmd->next;
        }
    }
}

internal CommandList* open_command_list(Renderer* r, D3D12_COMMAND_LIST_TYPE type) {
    update_available_command_lists(r);

    CommandList* found = 0;

    for (CommandList** p_cmd = &r->available_command_lists; *p_cmd;) {
        CommandList* cmd = *p_cmd;
        if (cmd->type == type) {
            *p_cmd = cmd->next;
            found = cmd;
            break;
        }
        else {
            p_cmd = &cmd->next;
        }
    }

    if (!found) {
        found = arena_push_struct_zero(&r->arena, CommandList);

        found->type = type;
        r->device->CreateCommandAllocator(type, IID_PPV_ARGS(&found->allocator));
        r->device->CreateCommandList(0, type, found->allocator, 0, IID_PPV_ARGS(&found->list));
        found->list->Close();

        debug_message("Allocated a command list.\n");
    }

    CommandList* cmd = found;

    cmd->allocator->Reset();
    cmd->list->Reset(cmd->allocator, 0);

    if (cmd->type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        cmd->list->SetGraphicsRootSignature(r->root_signature);
        cmd->list->SetDescriptorHeaps(1, &r->bindless_heap.heap);
    }

    return cmd;
}

internal void submit_command_list(Renderer* r, CommandQueue* queue, CommandList* cmd) {
    cmd->list->Close();

    ID3D12CommandList* decayed = cmd->list;
    queue->queue->ExecuteCommandLists(1, &decayed);
    cmd->fence_val = command_queue_signal(queue);
    cmd->queue = queue;
    
    cmd->next = r->executing_command_lists;
    r->executing_command_lists = cmd;
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

    r->direct_queue = command_queue_init(r->device, D3D12_COMMAND_LIST_TYPE_DIRECT);
    r->copy_queue = command_queue_init(r->device, D3D12_COMMAND_LIST_TYPE_COPY);

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
    if (FAILED(r->factory->CreateSwapChainForHwnd(r->direct_queue.queue, (HWND)window, &swapchain_desc, 0, 0, &swapchain_1))) {
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
    command_queue_flush(&r->direct_queue);
    command_queue_flush(&r->copy_queue);

    update_available_command_lists(r);
    assert(!r->executing_command_lists);

    for (CommandList* cmd = r->available_command_lists; cmd; cmd = cmd->next) {
        cmd->allocator->Release();
        cmd->list->Release();
    }

    for (int i = 0; i < MESH_MEMORY_COUNT; ++i) {
        if (r->mesh_data[i].vbuffer) {
            r->mesh_data[i].vbuffer->Release();
            r->mesh_data[i].ibuffer->Release();
        }
    }

    r->depth_buffer->Release();
    r->pipeline->Release();

    #if _DEBUG
    for (ReleasableResource* n = r->garbage; n; n = n->next) {
        n->resource->Release();
    }
    #endif

    for (int i = 0; i < DXGI_MAX_SWAP_CHAIN_BUFFERS; ++i) {
        free_descriptor(&r->rtv_heap, r->swapchain_rtvs[i]);
    }

    release_swapchain_buffers(r);
    r->swapchain->Release();

    r->root_signature->Release();

    r->bindless_heap.heap->Release();
    r->dsv_heap.heap->Release();
    r->rtv_heap.heap->Release();

    command_queue_release(&r->copy_queue);
    command_queue_release(&r->direct_queue);

    r->device->Release();
    r->adapter->Release();
    r->factory->Release();
#endif
}

void renderer_handle_resize(Renderer* r, u32 width, u32 height) {
    if (width == 0 || height == 0) {
        return;
    }

    command_queue_flush(&r->direct_queue);

    release_swapchain_buffers(r);
    r->swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    get_swapchain_buffers_and_create_rtvs(r);

    r->depth_buffer->Release();
    create_depth_buffer(r, width, height);

    debug_message("Resized swapchain (%d x %d)\n", width, height);
}

internal void append_releasable_resource(Renderer* r, ID3D12Resource* resource, ReleasableResource** target) {
    ReleasableResource* rr;

    if (!r->releasable_resource_slots) {
        rr = arena_push_struct_zero(&r->arena, ReleasableResource);
    }
    else {
        rr = r->releasable_resource_slots;
        r->releasable_resource_slots = rr->next;
    }

    rr->resource = resource;
    rr->next = *target;
    *target = rr;
}

internal void release_releasable_resource(Renderer* r, ReleasableResource* rr) {
    rr->resource->Release();
    rr->next = r->releasable_resource_slots;
    r->releasable_resource_slots = rr;
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

        #if _DEBUG
        append_releasable_resource(r, resource, &r->garbage);
        #endif

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

void renderer_render_frame(Renderer* r, Camera* camera, MeshInstance* queue, u32 queue_len) {
    DXGI_SWAP_CHAIN_DESC1 swapchain_desc;
    r->swapchain->GetDesc1(&swapchain_desc);

    u32 swapchain_index = r->swapchain->GetCurrentBackBufferIndex();
    command_queue_wait(&r->direct_queue, r->swapchain_fences[swapchain_index]);

    CommandList* cmd = open_command_list(r, D3D12_COMMAND_LIST_TYPE_DIRECT);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = r->swapchain_buffers[swapchain_index];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    cmd->list->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = cpu_descriptor_handle(&r->rtv_heap, r->swapchain_rtvs[swapchain_index]);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = cpu_descriptor_handle(&r->dsv_heap, r->depth_view);

    f32 clear_color[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
    cmd->list->ClearRenderTargetView(rtv_handle, clear_color, 0, 0);
    cmd->list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, 0);
    cmd->list->OMSetRenderTargets(1, &rtv_handle, false, &dsv_handle);

    D3D12_RECT scissor = {};
    scissor.right = swapchain_desc.Width;
    scissor.bottom = swapchain_desc.Height;

    D3D12_VIEWPORT viewport = {};
    viewport.MaxDepth = 1.0f;
    viewport.Width = (f32)swapchain_desc.Width;
    viewport.Height = (f32)swapchain_desc.Height;

    cmd->list->RSSetScissorRects(1, &scissor);
    cmd->list->RSSetViewports(1, &viewport);

    cmd->list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->list->SetPipelineState(r->pipeline);

    f32 aspect_ratio = (f32)swapchain_desc.Width / (f32)swapchain_desc.Height;
    XMMATRIX view_matrix = XMMatrixInverse(0, camera->transform);
    XMMATRIX projection_matrix = XMMatrixPerspectiveFovRH(camera->fov/aspect_ratio, aspect_ratio, 1000.0f, 0.1f);
    XMMATRIX view_projection = view_matrix * projection_matrix;
    ConstantBuffer* camera_cbuffer = get_constant_buffer(r, &view_projection, sizeof(view_projection));
    drop_constant_buffer(cmd, camera_cbuffer);
    cmd->list->SetGraphicsRoot32BitConstant(0, camera_cbuffer->view.index, 0);

    for (u32 i = 0; i < queue_len; ++i) {
        MeshInstance* mesh_sub = &queue[i];
        MeshData* mesh_data = get_mesh_data(r, mesh_sub->mesh);

        ConstantBuffer* transform_cbuffer = get_constant_buffer(r, &mesh_sub->transform, sizeof(mesh_sub->transform));
        drop_constant_buffer(cmd, transform_cbuffer);

        cmd->list->SetGraphicsRoot32BitConstant(0, mesh_data->vbuffer_view.index, 1);
        cmd->list->SetGraphicsRoot32BitConstant(0, mesh_data->ibuffer_view.index, 2);
        cmd->list->SetGraphicsRoot32BitConstant(0, transform_cbuffer->view.index, 3);

        cmd->list->DrawInstanced(mesh_data->index_count, 1, 0, 0);
    }

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd->list->ResourceBarrier(1, &barrier);

    submit_command_list(r, &r->direct_queue, cmd);

    r->swapchain->Present(1, 0);
    r->swapchain_fences[swapchain_index] = command_queue_signal(&r->direct_queue);
}

struct RendererUploadContext {
    CommandList* cmd;
    ReleasableResource* releasable_resources;
};

struct RendererUploadTicket {
    u64 fence_val;
    ReleasableResource* releasable_resources;
};

RendererUploadContext* renderer_open_upload_context(Arena* arena, Renderer* r) {
    RendererUploadContext* context = arena_push_struct(arena, RendererUploadContext);
    context->cmd = open_command_list(r, D3D12_COMMAND_LIST_TYPE_COPY);
    return context;
}

RendererUploadTicket* renderer_submit_upload_context(Arena* arena, Renderer* r, RendererUploadContext* context) {
    submit_command_list(r, &r->copy_queue, context->cmd);
    RendererUploadTicket* ticket = arena_push_struct(arena, RendererUploadTicket);
    ticket->fence_val = command_queue_signal(&r->copy_queue);
    ticket->releasable_resources = context->releasable_resources;
    return ticket;
}

bool renderer_upload_finished(Renderer* r, RendererUploadTicket* ticket) {
    bool result = command_queue_reached(&r->copy_queue, ticket->fence_val);

    if (result) {
        for (ReleasableResource* n = ticket->releasable_resources; n;) {
            ReleasableResource* next = n->next;
            release_releasable_resource(r, n);
            n = next;
        }

        ticket->releasable_resources = 0;
    }

    return result;
}

Mesh renderer_new_mesh(Renderer* r, RendererUploadContext* upload_context, Vertex* vertex_data, u32 vertex_count, u32* index_data, u32 index_count) {
    UNUSED(upload_context);

    assert(r->num_free_meshes > 0);

    u32 index = r->mesh_free_list[--r->num_free_meshes];

    u32 vertex_data_size = vertex_count * sizeof(Vertex);
    u32 index_data_size = index_count * sizeof(u32);

    MeshData* data = &r->mesh_data[index];

    ID3D12Resource* staging_vbuffer, *staging_ibuffer;

    D3D12_RESOURCE_DESC resource_desc = {};
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES upload_heap_props = {};
    upload_heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_HEAP_PROPERTIES default_heap_props = {};
    default_heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    resource_desc.Width = vertex_data_size;
    r->device->CreateCommittedResource(&upload_heap_props, D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, 0, IID_PPV_ARGS(&staging_vbuffer));
    r->device->CreateCommittedResource(&default_heap_props, D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_COMMON, 0, IID_PPV_ARGS(&data->vbuffer));

    resource_desc.Width = index_data_size;
    r->device->CreateCommittedResource(&upload_heap_props, D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ, 0, IID_PPV_ARGS(&staging_ibuffer));
    r->device->CreateCommittedResource(&default_heap_props, D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_COMMON, 0, IID_PPV_ARGS(&data->ibuffer));

    void* mapped_ptr;

    staging_vbuffer->Map(0, 0, &mapped_ptr);
    memcpy(mapped_ptr, vertex_data, vertex_data_size);
    staging_vbuffer->Unmap(0, 0);
    
    staging_ibuffer->Map(0, 0, &mapped_ptr);
    memcpy(mapped_ptr, index_data, index_data_size);
    staging_ibuffer->Unmap(0, 0);

    upload_context->cmd->list->CopyBufferRegion(data->vbuffer, 0, staging_vbuffer, 0, vertex_data_size);
    upload_context->cmd->list->CopyBufferRegion(data->ibuffer, 0, staging_ibuffer, 0, index_data_size);

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

    append_releasable_resource(r, staging_vbuffer, &upload_context->releasable_resources);
    append_releasable_resource(r, staging_ibuffer, &upload_context->releasable_resources);

    Mesh handle = {};
    handle.index = index;
    #if _DEBUG
    handle.version = r->mesh_versions[index];
    #endif
    
    return handle;
}