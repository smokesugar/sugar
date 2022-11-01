#include "samplers.hlsl"

struct Vertex {
    float3 pos;
    float3 norm;
    float2 uv;
};

struct MatrixStruct {
    float4x4 m;
};

struct VSOut {
    float4 sv_pos : SV_Position;
    float3 normal : Normal;
    float2 uv : UV;
};

cbuffer DescriptorIndices : register(b0, space0)
{
    uint di_camera;
    uint di_vbuffer;
    uint di_ibuffer;
    uint di_transform;
    uint di_texture;
};

VSOut vs_main(uint vertex_id : SV_VertexID) {
    ConstantBuffer<MatrixStruct> camera = ResourceDescriptorHeap[di_camera];

    StructuredBuffer<Vertex> vbuffer = ResourceDescriptorHeap[di_vbuffer];
    StructuredBuffer<uint> ibuffer = ResourceDescriptorHeap[di_ibuffer];

    ConstantBuffer<MatrixStruct> transform = ResourceDescriptorHeap[di_transform];

    Vertex vertex = vbuffer[ibuffer[vertex_id]];
    
    VSOut vso;
    vso.sv_pos = mul(camera.m, mul(transform.m, float4(vertex.pos, 1.0f)));
    vso.normal = normalize(mul((float3x3)transform.m, vertex.norm));
    vso.uv = vertex.uv;

    return vso;
}

float4 ps_main(VSOut vso) : SV_Target{
    float3 normal = normalize(vso.normal);
    float3 light_dir = normalize(float3(1.0f, 3.0f, 3.0f));

    Texture2D<float3> test_texture = ResourceDescriptorHeap[di_texture];

    float3 diffuse_color = test_texture.Sample(linear_sampler, vso.uv);;
    float3 diffuse = max(dot(light_dir, normal), 0.0f) * diffuse_color;

    float3 ambient = 0.01f.xxx;

    float3 lighting = ambient + diffuse;

    float3 tonemapped_output = sqrt(lighting);
    return float4(tonemapped_output, 1.0f);
}