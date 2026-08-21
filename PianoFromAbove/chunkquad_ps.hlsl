#include "common.hlsli"

Texture2D chunkTexture : register(t0);
SamplerState chunkSampler : register(s0);

float4 main(BackgroundPSInput input) : SV_TARGET {
    return chunkTexture.Sample(chunkSampler, input.uv);
}