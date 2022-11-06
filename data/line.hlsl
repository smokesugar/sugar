#include "common.hlsl"

cbuffer DescriptorIndices : register(b0, space0)
{
    uint di_camera;
    uint di_vbuffer;
    uint di_ibuffer;
};

float4 vs_main(uint vertex_id : SV_VertexID) : SV_Position{
    ConstantBuffer<MatrixStruct> camera = ResourceDescriptorHeap[di_camera];
    StructuredBuffer<float4> vbuffer = ResourceDescriptorHeap[di_vbuffer];
    StructuredBuffer<uint> ibuffer = ResourceDescriptorHeap[di_ibuffer];
    
    return mul(camera.m, float4(vbuffer[ibuffer[vertex_id]].xyz, 1.0f));
};

float4 ps_main() : SV_Target{
    return float4(1.0f, 1.0f, 1.0f, 1.0f);
};