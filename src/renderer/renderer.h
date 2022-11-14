#pragma once

#include <DirectXMath.h>
using namespace DirectX;

#include "common.h"

struct Renderer;

struct Mesh { 
    u64 handle;
};

struct Material {
    u64 handle;
};

Renderer* renderer_init(Arena* arena, void* window);
void renderer_release_backend(Renderer* r);

void renderer_handle_resize(Renderer* r, u32 width, u32 height);

struct MeshInstance {
    Mesh mesh;
    Material material;
    XMMATRIX transform;
};

struct RendererCamera {
    XMMATRIX transform;
    f32 near_plane;
    f32 far_plane;
    f32 fov;
};

struct LineMesh {
    u32 num_line_vertices;
    u32 num_line_indices;
    XMFLOAT4* line_vertices;
    u32* line_indices;
};

struct RendererFrameData {
    u32 queue_len;
    u32 num_line_meshes;
    RendererCamera* camera;

    MeshInstance* queue;

    LineMesh* line_meshes;
    XMVECTOR frustum[6];
};

void renderer_render_frame(Renderer* r, RendererFrameData* frame);

Material renderer_get_default_material(Renderer* r);

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

struct AABB {
    XMFLOAT3 min;
    f32 pad0;
    XMFLOAT3 max;
    f32 pad1;
};

struct MeshCreateInfo {
    Vertex* vertex_data;
    u32* index_data;
    u32 vertex_count;
    u32 index_count;
    AABB aabb;
};

Mesh renderer_new_mesh(Renderer* r, RendererUploadContext* upload_context, MeshCreateInfo* info);
void renderer_free_mesh(Renderer* r, Mesh mesh);
bool renderer_mesh_alive(Renderer* r, Mesh mesh);

Material renderer_new_material(Renderer* r, RendererUploadContext* upload_context, u32 texture_w, u32 texture_h, void* texture_data);
void renderer_free_material(Renderer* r, Material mat);
bool renderer_material_alive(Renderer* r, Material mat);
