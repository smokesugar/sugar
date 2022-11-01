#include <Windows.h> 
#include <stdarg.h>
#include <stdio.h>

#include "common.h"
#include "renderer/renderer.h"
#include "renderer/gltf.h"

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

#define SCRATCH_ARENA_SIZE (1024 * 1024 * 1024)
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
    if (scratch.ptr < scratch.arena->cursor) {
        scratch.arena->cursor = scratch.ptr;
    }
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

void write_file(char* path, void* data, u64 size) {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_WRITE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);

    if (file == INVALID_HANDLE_VALUE) {
        system_message_box("Couldn't create file: '%s'", path);
    }

    DWORD bytes_written;
    WriteFile(file, data, (DWORD)size, &bytes_written, 0);

    assert(bytes_written == size);

    CloseHandle(file);
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

    u64 perm_arena_size = 64 * 1024u * 1024u;
    u64 frame_arena_size = 3 * 1024 * 1024;
    Arena perm_arena = arena_init(page_alloc(perm_arena_size), perm_arena_size);
    Arena frame_arena = arena_init(page_alloc(frame_arena_size), frame_arena_size);

    Renderer* renderer = renderer_init(&perm_arena, window);

    RendererUploadContext* upload_context = renderer_open_upload_context(&perm_arena, renderer);
    LoadGLTFResult gltf = load_gltf(&perm_arena, renderer, upload_context, "models/as-val.glb");
    RendererUploadTicket* upload_ticket = renderer_submit_upload_context(&perm_arena, renderer, upload_context);

    for (u32 i = 0; i < gltf.num_instances; ++i) {
        gltf.instances[i].transform *= XMMatrixScaling(0.4f, 0.4f, 0.4f);
    }

    f32 last_time = engine_time();

    while (true) {
        arena_clear(&frame_arena);

        f32 time = engine_time();
        f32 dt = time - last_time;
        last_time = time;

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

        for (u32 i = 0; i < gltf.num_instances; ++i) {
            gltf.instances[i].transform *= XMMatrixRotationRollPitchYaw(0.0f, dt * PI32, 0.0f);
        }

        Camera camera;
        camera.transform = XMMatrixTranslation(0.0f, 0.0f, 3.0f);
        camera.fov = PI32 * 0.5f;

        MeshInstance* queue = 0;
        int queue_len = 0;

        if (renderer_upload_finished(renderer, upload_ticket)) {
            queue = arena_push_array(&frame_arena, MeshInstance, gltf.num_instances);
            memcpy(queue, gltf.instances, gltf.num_instances * sizeof(MeshInstance));
            queue_len = gltf.num_instances;
        }

        renderer_render_frame(renderer, &camera, queue, queue_len);
    }

    #if _DEBUG
    for (u32 i = 0; i < gltf.num_instances; ++i) {
        if (renderer_mesh_alive(renderer, gltf.instances[i].mesh)) {
            renderer_free_mesh(renderer, gltf.instances[i].mesh);
        }
    }

    for (u32 i = 0; i < gltf.num_materials; ++i) {
        renderer_free_material(renderer, gltf.materials[i]);
    }
    #endif

    renderer_release_backend(renderer);

    return 0;
}