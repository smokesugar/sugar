#include <string.h>
#include <stdio.h>
#include <stddef.h>

#include "gltf.h"
#include "json.h"

struct GLTFBuffer {
    u64 len;
    void* memory;
};

struct GLTFBufferView {
    GLTFBuffer* buffer;
    u64 len;
    u64 offset;
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
    u64 offset;
    GLTFType type;
    u32 count;
    int component_count;
};

struct GLTFMesh {
    u32 num_primitives;
    Mesh* primitives;
};

struct GLTFNode {
    u32 num_children;
    GLTFNode** children;
    XMMATRIX transform;
    GLTFMesh* mesh;
};

struct GLBHeader {
    u32 magic;
    u32 version;
    u32 len;
};

enum GLBChunkType {
    GLB_CHUNK_JSON = 0x4E4F534A,
    GLB_CHUNK_BIN = 0x004E4942,
};

struct GLBChunk {
    u32 len;
    u32 type;
    char memory[1];
};

struct DecodeBase64Result {
    u64 size;
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
    result.size = out_len;
    result.memory = out;
    
    return result;
}

internal void process_gltf_node(Arena* arena, LoadGLTFResult* result, GLTFNode* node, XMMATRIX parent_transform) {
    XMMATRIX absolute_transform = node->transform * parent_transform;
    
    if (node->mesh) {
        for (u32 i = 0; i < node->mesh->num_primitives; ++i) {
            MeshInstance* instance = arena_push_struct(arena, MeshInstance);
            ++result->num_instances;
            instance->mesh = node->mesh->primitives[i];
            instance->transform = absolute_transform;
        }
    }

    for (u32 i = 0; i < node->num_children; ++i) {
        process_gltf_node(arena, result, node->children[i], absolute_transform);
    }
}

internal XMVECTOR extract_json_vector(Json* j) {
    assert(json_len(j) <= 4);

    XMVECTOR result = {};
    f32* ptr = (f32*)&result;

    JSON_FOREACH(j, e) {
        switch (e->type) {
            case JSON_INTEGER:
                *(ptr++) = (f32)e->integer;
                break;
            case JSON_REAL:
                *(ptr++) = (f32)e->real;
                break;
            default:
                assert(false);
                break;
        }
    }

    return result;
}

internal LoadGLTFResult process_gltf(Arena* arena, Renderer* renderer, RendererUploadContext* upload_context, Json* root, GLTFBuffer* buffers, u32 num_buffers) {
    UNUSED(num_buffers);

    Scratch scratch = get_scratch(&arena, 1);

    Json* asset_views = json_query(root, "bufferViews");
    GLTFBufferView* views = arena_push_array(scratch.arena, GLTFBufferView, json_len(asset_views));
    u32 num_views = 0;

    JSON_FOREACH(asset_views, asset_view) {
        GLTFBufferView* view = &views[num_views++];

        u32 buffer_index = (u32)json_query(asset_view, "buffer")->integer;
        assert(buffer_index < num_buffers);
        view->buffer = &buffers[buffer_index];

        view->len = json_query(asset_view, "byteLength")->integer;
        Json* j_offset = json_query(asset_view, "byteOffset");
        if (j_offset) {
            view->offset = j_offset->integer;
        }
        else {
            view->offset = 0;
        }
    }

    Json* asset_accessors = json_query(root, "accessors");
    GLTFAccessor* accessors = arena_push_array(scratch.arena, GLTFAccessor, json_len(asset_accessors));
    u32 num_accessors = 0;

    JSON_FOREACH(asset_accessors, asset_accessor) {
        GLTFAccessor* accessor = &accessors[num_accessors++];

        u32 view_index = (u32)json_query(asset_accessor, "bufferView")->integer;
        assert(view_index < num_views);
        accessor->view = &views[view_index];

        Json* j_offset = json_query(asset_accessor, "byteOffset");
        if (j_offset) {
            accessor->offset = j_offset->integer;
        }
        else {
            accessor->offset = 0;
        }

        accessor->type = (GLTFType)json_query(asset_accessor, "componentType")->integer;
        accessor->count = (u32)json_query(asset_accessor, "count")->integer;

        char* component_count = json_query(asset_accessor, "type")->string;

        if (strcmp(component_count, "SCALAR") == 0) {
            accessor->component_count = 1;
        }
        else if (strcmp(component_count, "VEC2") == 0) {
            accessor->component_count = 2;
        }
        else if (strcmp(component_count, "VEC3") == 0) {
            accessor->component_count = 3;
        }
        else if (strcmp(component_count, "VEC4") == 0) {
            accessor->component_count = 4;
        }
        else {
            assert(false);
        }
    }

    Json* asset_meshes = json_query(root, "meshes");
    GLTFMesh* meshes = arena_push_array(scratch.arena, GLTFMesh, json_len(asset_meshes));
    int num_meshes = 0;

    JSON_FOREACH(json_query(root, "meshes"), asset_mesh) {
        GLTFMesh* mesh = &meshes[num_meshes++];

        Json* mesh_primitives = json_query(asset_mesh, "primitives");
        mesh->num_primitives = json_len(mesh_primitives);
        mesh->primitives = arena_push_array(scratch.arena, Mesh, mesh->num_primitives);

        int primitive_index = 0;
        JSON_FOREACH(mesh_primitives, primitive) {
            Scratch prim_scratch = get_scratch(&arena, 1);

            Json* attributes = json_query(primitive, "attributes");
            u32 pos_index = (u32)json_query(attributes, "POSITION")->integer;
            u32 norm_index = (u32)json_query(attributes, "NORMAL")->integer;
            u32 uv_index = (u32)json_query(attributes, "TEXCOORD_0")->integer;
            u32 indices_index = (u32)json_query(primitive, "indices")->integer;

            assert(pos_index < num_accessors);
            assert(norm_index < num_accessors);
            assert(uv_index < num_accessors);
            assert(indices_index < num_accessors);

            GLTFAccessor* pos_accessor = &accessors[pos_index];
            GLTFAccessor* norm_accessor = &accessors[norm_index];
            GLTFAccessor* uv_accessor = &accessors[uv_index];
            GLTFAccessor* indices_accessor = &accessors[indices_index];

            assert(pos_accessor->count == norm_accessor->count && pos_accessor->count == uv_accessor->count);
            assert(pos_accessor->type == GLTF_FLOAT && norm_accessor->type == GLTF_FLOAT && uv_accessor->type == GLTF_FLOAT);
            assert(indices_accessor->type == GLTF_UNSIGNED_INT || indices_accessor->type == GLTF_UNSIGNED_SHORT);
            assert(indices_accessor->component_count == 1);

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

                f32* pos = pos_src + i * pos_accessor->component_count;
                f32* norm = norm_src + i * norm_accessor->component_count;
                f32* uv = uv_src + i * uv_accessor->component_count;

                v->pos = { pos[0], pos[1], pos[2] };
                v->norm = { norm[0], norm[1], norm[2] };
                v->uv = { uv[0], uv[1] };
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

            Mesh* prim = &mesh->primitives[primitive_index++];
            *prim = renderer_new_mesh(renderer, upload_context, vertex_data, vertex_count, index_data, index_count);

            release_scratch(prim_scratch);
        }
    }

    Json* asset_nodes = json_query(root, "nodes");
    int num_nodes = json_len(asset_nodes);
    GLTFNode* nodes = arena_push_array(scratch.arena, GLTFNode, num_nodes);

    int node_index = 0;
    JSON_FOREACH(asset_nodes, asset_node) {
        GLTFNode* node = &nodes[node_index++];

        Json* node_children = json_query(asset_node, "children");
        if (node_children) {
            node->num_children = json_len(node_children);
            node->children = arena_push_array(scratch.arena, GLTFNode*, node->num_children);

            int child_index = 0;
            JSON_FOREACH(node_children, child) {
                assert(child->integer < num_nodes);
                node->children[child_index++] = &nodes[child->integer];
            }
        }
        else {
            node->num_children = 0;
            node->children = 0;
        }
        
        Json* matrix = json_query(asset_node, "matrix");
        if (matrix) {
            assert(json_len(matrix) == 16);
            f32* matrix_element = (f32*)&node->transform;
            JSON_FOREACH(matrix, el) {
                switch (el->type) {
                    case JSON_REAL:
                        *(matrix_element++) = (f32)el->real;
                        break;
                    case JSON_INTEGER:
                        *(matrix_element++) = (f32)el->integer;
                        break;
                    default:
                        assert(false);
                        break;
                }
            }
        }
        else {
            node->transform = XMMatrixIdentity();

            Json* scaling = json_query(asset_node, "scale");
            if (scaling) {
                node->transform *= XMMatrixScalingFromVector(extract_json_vector(scaling));
            }

            Json* rotation = json_query(asset_node, "rotation");
            if (rotation) {
                node->transform *= XMMatrixRotationQuaternion(extract_json_vector(rotation));
            }

            Json* translation = json_query(asset_node, "translation");
            if (translation) {
                node->transform *= XMMatrixTranslationFromVector(extract_json_vector(translation));
            }
        }

        Json* mesh = json_query(asset_node, "mesh");
        if (mesh) {
            assert(mesh->integer < num_meshes);
            node->mesh = &meshes[mesh->integer];
        }
        else {
            node->mesh = 0;
        }
    }

    LoadGLTFResult result;
    result.num_instances = 0;
    result.instances = arena_mark(arena, MeshInstance);

    JSON_FOREACH(json_query(root, "scenes"), scene) {
        JSON_FOREACH(json_query(scene, "nodes"), node) {
            process_gltf_node(arena, &result, &nodes[node->integer], XMMatrixIdentity());
        }
    }
   
    release_scratch(scratch);

    return result;
}

internal LoadGLTFResult load_gltf_glb(Arena* arena, Renderer* renderer, RendererUploadContext* upload_context, char* path) {
    Scratch scratch = get_scratch(&arena, 1);

    assert(strcmp(strrchr(path, '.'), ".glb") == 0);
    ReadFileResult file = read_file(scratch.arena, path);
    u8* file_cursor = (u8*)file.memory;
    u8* file_end = file_cursor + file.size;

    GLBHeader* header = (GLBHeader*)file_cursor;
    file_cursor += sizeof(*header);

    assert(memcmp(&header->magic, "glTF", 4) == 0);
    assert(header->version == 2);
    assert(header->len == file.size);

    #define READ_CHUNK(name) GLBChunk* name = (GLBChunk*)file_cursor; file_cursor += offsetof(GLBChunk, memory) + name->len
    
    READ_CHUNK(json_chunk);
    assert(json_chunk->type == GLB_CHUNK_JSON);

    char* json_string = (char*)arena_push(scratch.arena, json_chunk->len + 1);
    memcpy(json_string, json_chunk->memory, json_chunk->len);
    json_string[json_chunk->len] = '\0';

    Arena* conflicts[] = { arena, scratch.arena };
    Scratch scratch_2 = get_scratch(conflicts, ARRAY_LEN(conflicts));

    int num_buffers = 0;
    GLTFBuffer* buffers = arena_mark(scratch_2.arena, GLTFBuffer);

    while (file_cursor != file_end) {
        READ_CHUNK(buf_chunk);
        assert(buf_chunk->type == GLB_CHUNK_BIN);

        GLTFBuffer* buf = arena_push_struct(scratch_2.arena, GLTFBuffer);
        ++num_buffers;

        buf->len = buf_chunk->len;
        buf->memory = buf_chunk->memory;
    }

    #undef READ_CHUNK

    Json* root = parse_json_string(scratch.arena, json_string);
    Json* ahd = json_query(root, "buffers");
    UNUSED(ahd);
    LoadGLTFResult result = process_gltf(arena, renderer, upload_context, root, buffers, num_buffers);
        
    release_scratch(scratch_2);
    release_scratch(scratch);

    return result;
}

internal LoadGLTFResult load_gltf_gltf(Arena* arena, Renderer* renderer, RendererUploadContext* upload_context, char* path) {
    Scratch scratch = get_scratch(&arena, 1);

    assert(strcmp(strrchr(path, '.'), ".gltf") == 0);
    ReadFileResult file = read_file(scratch.arena, path);
    Json* root = parse_json_string(scratch.arena, file.memory);

    char dir[1024];
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
        buf->len = json_query(src_buf, "byteLength")->integer;

        char* base64_header = "data:application/octet-stream;base64,";
        char* uri = json_query(src_buf, "uri")->string;

        if (strncmp(uri, base64_header, strlen(base64_header)) == 0) {
            DecodeBase64Result decode = decode_base64(scratch.arena, uri + strlen(base64_header));
            assert(decode.size == buf->len);
            buf->memory = decode.memory;
        }
        else {
            char absolute_uri[1024];
            snprintf(absolute_uri, sizeof(absolute_uri), "%s%s", dir, uri);
            ReadFileResult buf_file = read_file(scratch.arena, absolute_uri);

            assert(buf_file.size == buf->len);
            buf->memory = buf_file.memory;
        }
    }

    LoadGLTFResult result = process_gltf(arena, renderer, upload_context, root, buffers, num_buffers);

    release_scratch(scratch);

    return result;
};

LoadGLTFResult load_gltf(Arena* arena, Renderer* renderer, RendererUploadContext* upload_context, char* path) {
    char* extension = strrchr(path, '.');
    assert(extension);
    
    if (strcmp(extension, ".gltf") == 0) {
        return load_gltf_gltf(arena, renderer, upload_context, path);
    }

    if (strcmp(extension, ".glb") == 0) {
        return load_gltf_glb(arena, renderer, upload_context, path);
    }

    system_message_box("Invalid GLTF file:\n'%s'", path);

    return {};
};
