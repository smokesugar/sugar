#include <Windows.h> 
#include <stdarg.h>
#include <stdio.h>

#include "common.h"
#include "renderer.h"
#include "json.h"

void system_message_box(char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    MessageBoxA(0, buf, "Sugar", 0);

    va_end(args);
}

void debug_message(char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    OutputDebugStringA(buf);

    va_end(args);
}

global_var LARGE_INTEGER counter_start;
global_var LARGE_INTEGER counter_freq;

f32 engine_time() {
    LARGE_INTEGER counter_now;
    QueryPerformanceCounter(&counter_now);
    i64 elapsed = counter_now.QuadPart - counter_start.QuadPart;
    f64 elapsed_seconds = (f64)elapsed / (f64)counter_freq.QuadPart;
    return (f32)elapsed_seconds;
}

#define SCRATCH_ARENA_SIZE (3 * 1024 * 1024)
#define NUM_SCRATCH_ARENAS 2
Arena scratch_arenas[NUM_SCRATCH_ARENAS];

Scratch get_scratch(Arena** conflicts, u32 conflict_count) {
    for (int i = 0; i < NUM_SCRATCH_ARENAS; ++i)
    {
        bool conflicting = false;
        for (u32 j = 0; j < conflict_count; ++j) {
            if (conflicts[j] == &scratch_arenas[i]) {
                conflicting = true;
                break;
            }
        }
        
        if (!conflicting) {
            Scratch scratch;
            scratch.arena = &scratch_arenas[i];
            scratch.ptr = scratch.arena->cursor;
            return scratch;
        }
    }

    assert(false && "Unable to retrieve a scratch arena that isn't in conflict");
    return {};
}

void release_scratch(Scratch scratch) {
    scratch.arena->cursor = scratch.ptr;
}

ReadFileResult read_file(Arena* arena, char* path) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);

    if (file == INVALID_HANDLE_VALUE) {
        system_message_box("Missing file: '%s'", path);
    }

    LARGE_INTEGER file_size;
    GetFileSizeEx(file, &file_size);

    char* memory = (char*)arena_push(arena, file_size.QuadPart + 1);
    DWORD bytes_read = 0;
    ReadFile(file, memory,(DWORD)file_size.QuadPart, &bytes_read, 0);

    assert((LONGLONG)bytes_read == file_size.QuadPart);
    memory[bytes_read] = '\0';

    CloseHandle(file);

    ReadFileResult result;
    result.memory = (char*)memory;
    result.size = file_size.QuadPart;

    return result;
}

struct WindowEvents {
    b32 closed;
    b32 resized;
};

internal LRESULT CALLBACK window_callback(HWND window, UINT msg, WPARAM w_param, LPARAM l_param) {
    LRESULT result = 0;

    WindowEvents* events = (WindowEvents*)GetWindowLongPtrA(window, GWLP_USERDATA);

    switch (msg) {
        case WM_SIZE:
            events->resized = true;
            break;
        case WM_CLOSE:
            events->closed = true;
            break;
        default:
            result = DefWindowProcA(window, msg, w_param, l_param);
    }

    return result;
}

struct LoadGLTFResult {
    Mesh* mesh_memory;
    u32 num_meshes;
};

struct GLTFBuffer {
    u32 len;
    void* memory;
};

struct GLTFBufferView {
    GLTFBuffer* buffer;
    u32 len;
    u32 offset;
};

enum GLTFType {
    GLTF_UNSIGNED_BYTE = 0x1401,
    GLTF_SHORT = 0x1402,
    GLTF_UNSIGNED_SHORT = 0x1403,
    GLTF_INT = 0x1404,
    GLTF_UNSIGNED_INT = 0x1405,
    GLTF_FLOAT = 0x1406,
};

struct GLTFAccessor {
    GLTFBufferView* view;
    u32 offset;
    GLTFType type;
    u32 count;
    int component_count;
};

struct DecodeBase64Result {
    u32 size;
    void* memory;
};


internal DecodeBase64Result decode_base64(Arena* arena, char *in)
{
    // Taken from https://nachtimwald.com/2017/11/18/base64-encode-and-decode-in-c/

	u64 in_len = strlen(in);

    u64 out_len = in_len / 4 * 3;
	for (u64 i=in_len; i-->0; ) {
		if (in[i] == '=') {
			out_len--;
		} else {
			break;
		}
	}
    
    int b64invs[] = {
        62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58,
        59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0, 1, 2, 3, 4, 5,
        6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
        21, 22, 23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28,
        29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42,
        43, 44, 45, 46, 47, 48, 49, 50, 51
    };

    char* out = (char*)arena_push(arena, out_len);

	for (u64 i=0, j=0; i<in_len; i+=4, j+=3) {
		int v = b64invs[in[i]-43];
		v = (v << 6) | b64invs[in[i+1]-43];
		v = in[i+2]=='=' ? v << 6 : (v << 6) | b64invs[in[i+2]-43];
		v = in[i+3]=='=' ? v << 6 : (v << 6) | b64invs[in[i+3]-43];

		out[j] = (v >> 16) & 0xFF;
		if (in[i+2] != '=')
			out[j+1] = (v >> 8) & 0xFF;
		if (in[i+3] != '=')
			out[j+2] = v & 0xFF;
	}

    DecodeBase64Result result;
    result.size = (u32)out_len;
    result.memory = out;
    
    return result;
}

internal LoadGLTFResult load_gltf(Arena* arena, Renderer* renderer, char* path) {
    Scratch scratch = get_scratch(&arena, 1);

    ReadFileResult file = read_file(scratch.arena, path);
    Json* root = parse_json_string(scratch.arena, file.memory);

    char dir[MAX_PATH + 1];
    {
        strcpy_s(dir, sizeof(dir), path);

        for (char* c = dir; *c; ++c) {
            if (*c == '\\') {
                *c = '/';
            }
        }

        char* last_slash = strrchr(dir, '/');
        if (last_slash) {
            last_slash[1] = '\0';
        }
        else {
            dir[0] = '\0';
        }
    }

    assert(strcmp(json_query(json_query(root, "asset"), "version")->string, "2.0") == 0 && "Unsupported GLTF version");

    Json* asset_buffers = json_query(root, "buffers");
    GLTFBuffer* buffers = arena_push_array(scratch.arena, GLTFBuffer, json_len(asset_buffers));
    u32 num_buffers = 0;

    JSON_FOREACH(json_query(root, "buffers"), src_buf) {
        GLTFBuffer* buf = &buffers[num_buffers++];
        buf->len = (u32)json_query(src_buf, "byteLength")->number;

        char* base64_header = "data:application/octet-stream;base64,";
        char* uri = json_query(src_buf, "uri")->string;

        if (strncmp(uri, base64_header, strlen(base64_header)) == 0) {
            DecodeBase64Result decode = decode_base64(scratch.arena, uri + strlen(base64_header));
            assert(decode.size == buf->len);
            buf->memory = decode.memory;
        }
        else {
            char absolute_uri[MAX_PATH + 1];
            snprintf(absolute_uri, sizeof(absolute_uri), "%s%s", dir, uri);
            ReadFileResult buf_file = read_file(scratch.arena, absolute_uri);

            assert(buf_file.size == buf->len);
            buf->memory = buf_file.memory;
        }
    }

    Json* asset_views = json_query(root, "bufferViews");
    GLTFBufferView* views = arena_push_array(scratch.arena, GLTFBufferView, json_len(asset_views));
    u32 num_views = 0;

    JSON_FOREACH(asset_views, asset_view) {
        GLTFBufferView* view = &views[num_views++];

        u32 buffer_index = (u32)json_query(asset_view, "buffer")->number;
        assert(buffer_index < num_buffers);
        view->buffer = &buffers[buffer_index];

        view->len = (u32)json_query(asset_view, "byteLength")->number;
        view->offset = (u32)json_query(asset_view, "byteOffset")->number;
    }

    Json* asset_accessors = json_query(root, "accessors");
    GLTFAccessor* accessors = arena_push_array(scratch.arena, GLTFAccessor, json_len(asset_accessors));
    u32 num_accessors = 0;

    JSON_FOREACH(asset_accessors, asset_accessor) {
        GLTFAccessor* accessor = &accessors[num_accessors++];

        u32 view_index = (u32)json_query(asset_accessor, "bufferView")->number;
        assert(view_index < num_views);
        accessor->view = &views[view_index];

        Json* j_offset = json_query(asset_accessor, "byteOffset");
        if (j_offset) {
            accessor->offset = (u32)j_offset->number;
        }
        else {
            accessor->offset = 0;
        }

        accessor->type = (GLTFType)json_query(asset_accessor, "componentType")->number;
        accessor->count = (u32)json_query(asset_accessor, "count")->number;

        char* component_count = json_query(asset_accessor, "type")->string;

        if (strcmp(component_count, "SCALAR") == 0) {
            accessor->component_count = 1;
        }

        if (strcmp(component_count, "VEC2") == 0) {
            accessor->component_count = 2;
        }

        if (strcmp(component_count, "VEC3") == 0) {
            accessor->component_count = 3;
        }

        if (strcmp(component_count, "VEC4") == 0) {
            accessor->component_count = 4;
        }
    }

    int num_meshes = 0;
    Mesh* mesh_memory = arena_mark(arena, Mesh);

    JSON_FOREACH(json_query(root, "meshes"), asset_mesh) {
        JSON_FOREACH(json_query(asset_mesh, "primitives"), primitive) {
            Scratch prim_scratch = get_scratch(&arena, 1);

            Json* attributes = json_query(primitive, "attributes");
            u32 pos_index  = (u32)json_query(attributes, "POSITION")->number;
            u32 norm_index = (u32)json_query(attributes, "NORMAL")->number;
            u32 uv_index   = (u32)json_query(attributes, "TEXCOORD_0")->number;
            u32 indices_index = (u32)json_query(primitive, "indices")->number;

            assert(pos_index < num_accessors);
            assert(norm_index < num_accessors);
            assert(uv_index < num_accessors);
            assert(indices_index < num_accessors);

            GLTFAccessor* pos_accessor     = &accessors[pos_index];
            GLTFAccessor* norm_accessor    = &accessors[norm_index];
            GLTFAccessor* uv_accessor      = &accessors[uv_index];
            GLTFAccessor* indices_accessor = &accessors[indices_index];

            assert(pos_accessor->count == norm_accessor->count && pos_accessor->count == uv_accessor->count);
            assert(pos_accessor->type == GLTF_FLOAT && norm_accessor->type == GLTF_FLOAT && uv_accessor->type == GLTF_FLOAT);
            assert(indices_accessor->type == GLTF_UNSIGNED_INT || indices_accessor->type == GLTF_UNSIGNED_SHORT);

            u32 vertex_count = pos_accessor->count;
            u32 index_count = indices_accessor->count;

            Vertex* vertex_data = arena_push_array(prim_scratch.arena, Vertex, vertex_count);
            u32* index_data = arena_push_array(prim_scratch.arena, u32, index_count);

            f32* pos_src = (f32*)((u8*)pos_accessor->view->buffer->memory + pos_accessor->view->offset + pos_accessor->offset);
            f32* norm_src = (f32*)((u8*)norm_accessor->view->buffer->memory + norm_accessor->view->offset + norm_accessor->offset);
            f32* uv_src = (f32*)((u8*)uv_accessor->view->buffer->memory + uv_accessor->view->offset + uv_accessor->offset);
            void* index_src = (u8*)indices_accessor->view->buffer->memory + indices_accessor->view->offset + indices_accessor->offset;

            for (u32 i = 0; i < vertex_count; ++i) {
                Vertex* v = &vertex_data[i];

                f32* pos  = pos_src  + i * pos_accessor->component_count;
                f32* norm = norm_src + i * norm_accessor->component_count;
                f32* uv   = uv_src   + i * uv_accessor->component_count;

                v->pos  = { pos[0], pos[1], pos[2] };
                v->norm = { norm[0], norm[1], norm[2] };
                v->uv   = { uv[0], uv[1] };
            }

            switch (indices_accessor->type) {
                case GLTF_UNSIGNED_INT:
                    memcpy(index_data, index_src, index_count * sizeof(u32));
                    break;
                case GLTF_UNSIGNED_SHORT:
                    for (u32 i = 0; i < index_count; ++i) {
                        index_data[i] = ((u16*)index_src)[i];
                    }
                    break;
                default:
                    assert(false && "Unreachable");
            }

            Mesh* m = arena_push_struct(arena, Mesh);
            *m = renderer_new_mesh(renderer, vertex_data, vertex_count, index_data, index_count);
            num_meshes++;
 
            release_scratch(prim_scratch);
        }
    }
   
    release_scratch(scratch);

    LoadGLTFResult result;
    result.num_meshes = num_meshes;
    result.mesh_memory = mesh_memory;

    return result;
};

internal void* page_alloc(u64 size) {
    return VirtualAlloc(0, size, MEM_COMMIT, PAGE_READWRITE);
}

int CALLBACK WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    QueryPerformanceCounter(&counter_start);
    QueryPerformanceFrequency(&counter_freq);

    for (int i = 0; i < NUM_SCRATCH_ARENAS; ++i) {
        scratch_arenas[i] = arena_init(page_alloc(SCRATCH_ARENA_SIZE), SCRATCH_ARENA_SIZE);
    }

    WNDCLASSA window_class = {};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_callback;
    window_class.lpszClassName = "SugarWindowClass";

    RegisterClassA(&window_class);

    HWND window = CreateWindowExA(
        0,
        window_class.lpszClassName,
        "Sugar",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,
        0,
        0,
        instance,
        0
    );

    if (!window) {
        return 1;
    }

    WindowEvents events;
    SetWindowLongPtrA(window, GWLP_USERDATA, (LONG_PTR)&events);

    ShowWindow(window, SW_MAXIMIZE);

    u64 perm_arena_size = 64 * 1024 * 1024;
    u64 frame_arena_size = 3 * 1024 * 1024;
    Arena perm_arena = arena_init(page_alloc(perm_arena_size), perm_arena_size);
    Arena frame_arena = arena_init(page_alloc(frame_arena_size), frame_arena_size);

    Renderer* renderer = renderer_init(&perm_arena, window);

    LoadGLTFResult gltf = load_gltf(&perm_arena, renderer, "models/suzanne_embedded.gltf");

    while (true) {
        arena_clear(&frame_arena);
        
        events = {};
        MSG msg;
        while (PeekMessageA(&msg, window, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        if (events.closed) {
            break;
        }

        if (events.resized) {
            RECT client_rect;
            GetClientRect(window, &client_rect);
            renderer_handle_resize(renderer, client_rect.right - client_rect.left, client_rect.bottom - client_rect.top);
        }

        MeshSubmission* queue = arena_push_array(&frame_arena, MeshSubmission, gltf.num_meshes);

        for (u32 i = 0; i < gltf.num_meshes; ++i) {
            queue[i].mesh = gltf.mesh_memory[i];
            queue[i].transform = XMMatrixIdentity();
        }

        renderer_render_frame(renderer, queue, gltf.num_meshes);
    }

    renderer_release_backend(renderer);

    return 0;
}