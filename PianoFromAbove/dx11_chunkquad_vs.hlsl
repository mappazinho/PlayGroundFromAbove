// D3D11 (SM 5.0) port of chunkquad_vs.hlsl.

cbuffer root : register(b0) {
    float4x4 proj;
    float2 quadPos;  // left, top in screen pixels
    float2 quadSize; // width, height in screen pixels
};

struct ChunkQuadPSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

ChunkQuadPSInput main(uint id : SV_VertexID) {
    ChunkQuadPSInput result;
    uint vertex = id % 4;
    float2 uv = float2(vertex == 1 || vertex == 2 ? 1.0f : 0.0f, vertex >= 2 ? 1.0f : 0.0f);
    result.uv = uv;
    result.position = mul(proj, float4(quadPos + quadSize * uv, 0.5f, 1.0f));
    return result;
}