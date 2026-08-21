#include "common.hlsli"

cbuffer ChunkQuadConstants : register(b0) {
    float4x4 proj;
    float2 quadPos;  // left, top in screen pixels
    float2 quadSize; // width, height in screen pixels
};

BackgroundPSInput main(uint id : SV_VertexID) {
    BackgroundPSInput result;

    uint vertex = id % 4;
    float2 uv = float2(vertex == 1 || vertex == 2 ? 1.0f : 0.0f, vertex >= 2 ? 1.0f : 0.0f);
    result.uv = uv;
    result.position = mul(proj, float4(quadPos + quadSize * uv, 0.5f, 1.0f));

    return result;
}