#include "common.hlsl"

cbuffer RootConstants : register(b0, space0)
{
    uint di_input_commands;
    uint num_input_commands;
    uint di_output_commands;
    uint di_num_output_commands;
};

struct IndirectCommand {
    uint vbuffer_index;
    uint ibuffer_index;
    uint transform_index;
    uint texture_index;
    uint vertex_count_per_instance;
    uint instance_count;
    uint start_vertex_location;
    uint start_instance_location;
};

[numthreads(256, 1, 1)]
void cs_main(uint3 thread_id : SV_DispatchThreadID, uint group_index : SV_GroupIndex) {
    StructuredBuffer<IndirectCommand> input_commands = ResourceDescriptorHeap[di_input_commands];
    RWStructuredBuffer<IndirectCommand> output_commands = ResourceDescriptorHeap[di_output_commands];
    RWBuffer<uint> num_output_commands = ResourceDescriptorHeap[di_num_output_commands];

    if (thread_id.x < num_input_commands) {
        uint index;
        InterlockedAdd(num_output_commands[0], 1, index);
        output_commands[index] = input_commands[thread_id.x];
    }
}