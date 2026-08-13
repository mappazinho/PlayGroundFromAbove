#include "common.hlsli"

Texture2D tex : register(t0);
SamplerState tex_sampler : register(s0);

cbuffer BackgroundConstants : register(b0) {
    float fade_start;
    float fade_end;
    float fade_enabled;
    float padding;
};

float4 main(BackgroundPSInput input) : SV_TARGET{
	float4 col = tex.Sample(tex_sampler, input.uv);
	if (fade_enabled > 0.5f) {
		float fade = 1.0f - smoothstep(fade_start, max(fade_end, fade_start + 1.0f), input.position.y);
		return float4(col.rgb, 1.0f - fade);
	}
	return float4(col.rgb, 1.0 - col.a);
}
