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
		// 4th cbuffer float = fade mode: 1 gives alpha = smoothstep (blur in
		// the strip, fading out toward fade_end / bottom edge); 0 gives
		// alpha = 1 - smoothstep (sharp above, fading out toward fade_start /
		// top edge).
		float s = smoothstep(fade_start, max(fade_end, fade_start + 1.0f), input.position.y);
		float alpha = (padding > 0.5f) ? s : (1.0f - s);
		return float4(col.rgb, alpha);
	}
	return float4(col.rgb, 1.0 - col.a);
}
