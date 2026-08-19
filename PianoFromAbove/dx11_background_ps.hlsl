// D3D11 (SM 5.0) port of the D3D12 renderer's runtime-compiled background PS
// (g_BackgroundPSHLSL in Renderer.cpp). Same cbuffer layout, same fade math.

Texture2D tex : register(t0);
SamplerState tex_sampler : register(s0);

cbuffer BackgroundConstants : register(b0) {
    float fade_start;
    float fade_end;
    float fade_enabled;
    float opacity;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET {
    float4 c = tex.Sample(tex_sampler, input.uv);
    float alpha;
    if (fade_enabled > 0.5) {
        float denom = max(fade_start + 1.0, fade_end) - fade_start;
        float t = saturate((input.position.y - fade_start) / denom);
        float s = t * t * (3.0 - 2.0 * t);
        // 4th cbuffer float = fade mode: 1 gives alpha = smoothstep (blur in
        // the strip, fading out toward fade_end / bottom edge); 0 gives
        // alpha = 1 - smoothstep (sharp above, fading out toward fade_start /
        // top edge).
        alpha = (opacity > 0.5) ? s : (1.0 - s);
    } else {
        alpha = 1.0 - opacity * c.a;
    }
    return float4(c.rgb, alpha);
}