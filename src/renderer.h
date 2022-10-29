#pragma once

#include <DirectXMath.h>
using namespace DirectX;

#include "common.h"

struct Renderer;

struct Mesh { 
    u32 index;
    #if _DEBUG
        u16 version;
    #endif
};

Renderer* renderer_init(Arena* arena, void* window);
void renderer_release_backend(Renderer* r);

void renderer_handle_resize(Renderer* r, u32 width, u32 height);

struct MeshSubmission {
    Mesh mesh;
    XMMATRIX transform;
};

void renderer_render_frame(Renderer* r, MeshSubmission* queue, u32 queue_len);

struct Vertex {
    XMFLOAT3 pos;
    XMFLOAT3 norm;
    XMFLOAT2 uv;
};

Mesh renderer_new_mesh(Renderer* r, Vertex* vertex_data, u32 vertex_count, u32* index_data, u32 index_count);
