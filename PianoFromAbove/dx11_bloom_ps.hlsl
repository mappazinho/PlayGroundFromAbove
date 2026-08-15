// D3D11 (SM 5.0) port of the D3D12 bloom composite pixel shader (g_BloomHLSL).

Texture2D<float4> g_bloom : register(t0);
SamplerState g_sampler : register(s0);

cbuffer BloomConstants : register(b0) {
    float saturation;
    float brightness;
    float3 tint;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET {
    float3 bloom = g_bloom.Sample(g_sampler, input.uv).rgb;

    float maxC = max(bloom.r, max(bloom.g, bloom.b));
    if (maxC > 0.0001) {
        float3 normalized = bloom / maxC;

        float gray = dot(normalized, float3(0.2126, 0.7152, 0.0722));
        float effectiveSat = saturation * (1.0 + 0.35 * max(0.0, brightness - 1.0));
        normalized = lerp(float3(gray, gray, gray), normalized, effectiveSat);
        normalized = max(normalized, 0.0);

        float scaledMax = maxC * brightness;
        float compressedMax = scaledMax / (1.0 + 0.25 * scaledMax);

        bloom = normalized * compressedMax;
    }

    return float4(bloom * tint, 0.0);
}