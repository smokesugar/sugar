#include "common.hlsl"

cbuffer RootConstants : register(b0, space0)
{
    uint di_input_commands;
    uint num_input_commands;
    uint di_output_commands;
    uint di_num_output_commands;
    uint di_frustum;
};

struct IndirectCommand {
    uint vbuffer_index;
    uint ibuffer_index;
    uint transform_index;
    uint texture_index;
    uint aabb_index;
    uint vertex_count_per_instance;
    uint instance_count;
    uint start_vertex_location;
    uint start_instance_location;
};

struct AABB {
    float3 min;
    float pad0;
    float3 max;
    float pad1;
};

struct Frustum {
    float4 planes[6];
};

groupshared uint active_threads_in_group;
groupshared uint group_base_counter;

[numthreads(256, 1, 1)]
void cs_main(uint3 thread_id : SV_DispatchThreadID, uint group_index : SV_GroupIndex) {
    if (group_index == 0) {
        active_threads_in_group = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    int culled = false;

    IndirectCommand instance;

    if (thread_id.x >= num_input_commands) {
        culled = true;
    }
    else {
        StructuredBuffer<IndirectCommand> input_commands = ResourceDescriptorHeap[di_input_commands];
        instance = input_commands[thread_id.x];

        ConstantBuffer<Frustum> frustum = ResourceDescriptorHeap[di_frustum];
        ConstantBuffer<MatrixStruct> transform = ResourceDescriptorHeap[instance.transform_index];
        ConstantBuffer<AABB> aabb = ResourceDescriptorHeap[instance.aabb_index];

        float3 min = aabb.min;
        float3 max = aabb.max;

        float4 obb_points[8] = {
            { max.x, max.y, max.z, 1.0f },
            { min.x, max.y, max.z, 1.0f },
            { min.x, max.y, min.z, 1.0f },
            { max.x, max.y, min.z, 1.0f },
            { max.x, min.y, max.z, 1.0f },
            { min.x, min.y, max.z, 1.0f },
            { min.x, min.y, min.z, 1.0f },
            { max.x, min.y, min.z, 1.0f },
        };

        for (int i = 0; i < 8; ++i) {
            obb_points[i] = mul(transform.m, obb_points[i]);
        }

        for (int plane_index = 0; plane_index < 6; ++plane_index) {
            float4 p = frustum.planes[plane_index];

            bool any_in_front = false;
            for (int corner_index = 0; corner_index < 8; ++corner_index) {
                float dist = dot(p.xyz, obb_points[corner_index].xyz + p.xyz * p.w);
                if (dist > 0) {
                    any_in_front = true;
                }
            }
            
            if (!any_in_front) {
                culled = true;
            }
        }
    }
    
    uint4 ballot = WaveActiveBallot(!culled);
    uint active_threads_in_wave = dot(countbits(ballot), 1.xxxx);

    if (WaveIsFirstLane()) {
        InterlockedAdd(active_threads_in_group, active_threads_in_wave);
    }

    GroupMemoryBarrierWithGroupSync();

    if (group_index == 0) {
        RWBuffer<uint> num_output_commands = ResourceDescriptorHeap[di_num_output_commands];
        InterlockedAdd(num_output_commands[0], active_threads_in_group, group_base_counter);
    }

    AllMemoryBarrierWithGroupSync();

    uint wave_base_index;
    if (WaveIsFirstLane()) {
        InterlockedAdd(group_base_counter, active_threads_in_wave, wave_base_index);
    }

    wave_base_index = WaveReadLaneFirst(wave_base_index);
    int offset = WavePrefixSum(!culled);

    if (!culled) {
        RWStructuredBuffer<IndirectCommand> output_commands = ResourceDescriptorHeap[di_output_commands];
        output_commands[wave_base_index + offset] = instance;
    }
}