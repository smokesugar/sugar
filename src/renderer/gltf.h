#pragma once

#include "renderer.h"

struct LoadGLTFResult {
    u32 num_materials;
    Material* materials;
    u32 num_instances;
    MeshInstance* instances;
};

LoadGLTFResult load_gltf(Arena* arena, Renderer* renderer, RendererUploadContext* upload_context, char* path);
