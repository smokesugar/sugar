#pragma once

#include <DirectXMath.h>
using namespace DirectX;

#include "common.h"

struct Renderer;

struct Mesh { 
    u64 handle;
};

Renderer* renderer_init(Arena* arena, void* window);
void renderer_release_backend(Renderer* r);

void renderer_handle_resize(Renderer* r, u32 width, u32 height);

struct MeshInstance {
    Mesh mesh;
    XMMATRIX transform;
};

struct Camera {
    XMMATRIX transform;
    f32 fov;
};

void renderer_render_frame(Renderer* r, Camera* camera, MeshInstance* queue, u32 queue_len);

struct RendererUploadContext;
struct RendererUploadTicket;

RendererUploadContext* renderer_open_upload_context(Arena* arena, Renderer* r);
RendererUploadTicket* renderer_submit_upload_context(Arena* arena, Renderer* r, RendererUploadContext* context);
bool renderer_upload_finished(Renderer* r, RendererUploadTicket* ticket);
void renderer_flush_upload(Renderer* r, RendererUploadTicket* ticket);

struct Vertex {
    XMFLOAT3 pos;
    XMFLOAT3 norm;
    XMFLOAT2 uv;
};

Mesh renderer_new_mesh(Renderer* r, RendererUploadContext* upload_context, Vertex* vertex_data, u32 vertex_count, u32* index_data, u32 index_count);
void renderer_free_mesh(Renderer* r, Mesh mesh);
bool renderer_mesh_alive(Renderer* r, Mesh mesh);
