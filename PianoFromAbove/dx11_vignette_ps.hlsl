// D3D11 (SM 5.0) port of the D3D12 vignette pixel shader (g_VignetteHLSL).
// The pass runs after bloom and multiplies the frame by the vignette factor,
// darkening the corners toward the edges.

cbuffer VignetteConstants : register(b0) {
    float intensity;    // 0..1 darkening amount
    float aspect;       // width / height (aspect-corrected radius)
    float innerRadius;  // falloff start in normalized UV distance
    float smoothness;   // falloff width
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET {
    float2 c = input.uv - 0.5;
    c.x *= aspect;
    float d = length(c) * 2.0;
    float v = 1.0 - intensity * smoothstep(innerRadius, innerRadius + smoothness, d);
    return float4(v, v, v, 1.0);
}