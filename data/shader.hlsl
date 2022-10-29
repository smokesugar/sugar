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
};

cbuffer DescriptorIndices : register(b0, space0)
{
    uint di_camera;
    uint di_vbuffer;
    uint di_ibuffer;
    uint di_transform;
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

    return vso;
}

float4 ps_main(VSOut vso) : SV_Target{
    float3 normal = normalize(vso.normal);
    return float4(sqrt(normal * 0.5f + 0.5f), 1.0f);
}