// D3D11 (SM 5.0) port of chunkquad_ps.hlsl.

struct ChunkQuadPSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

Texture2D chunkTexture : register(t0);
SamplerState chunkSampler : register(s0);

float4 main(ChunkQuadPSInput input) : SV_TARGET {
    return chunkTexture.Sample(chunkSampler, input.uv);
}