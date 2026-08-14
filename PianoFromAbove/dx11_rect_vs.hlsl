// D3D11 (SM 5.0) port of common.hlsli + rect_vs.hlsl. Must match the D3D12
// shader behavior: same cbuffer layout, same warp/corruption math.

cbuffer consts : register(b0) {
    float4x4 proj;
    float deflate;
    float notes_y;
    float notes_cy;
    float white_cx;
    float timespan;
    float stripMode;
    float stripTimeSpan;
    float fWarp;
    float fWarpTime;
    float fWarpSeedX;
    float fWarpSeedY;
    float notes_x;
    float notes_cx;
};

float WarpHash(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float KeyCorruptFactor(float h) {
    return 0.3 + 1.9 * pow(h, 1.7);
}

float KeyCorruptAmp(float w, float f) {
    return saturate((w - 0.15) / 0.85) * f;
}

float2 WarpOffset(float2 pos, float amp, float t, float s0, float s1, float qseed) {
    float2 result = 0;
    if (amp > 0.0) {
        float qt = floor(t * 2.0);
        float cMix = WarpHash(float2(qseed * 1.37, qt * 0.87 + 0.41));
        float2 random = (float2(WarpHash(float2(qseed * 3.19, qt * 1.13 + 0.62)),
                                WarpHash(float2(qseed * 5.71, qt * 1.47 + 0.29))) - 0.5) * float2(3000.0, 2000.0);
        float2 converge = cMix < 0.45 ? float2(-100.0, -60.0) : random;
        float h = WarpHash(pos * 0.013 + float2(qt * 3.71, qt * 1.93) + s0 * 3.17);
        float k = 16.0 - amp * 13.0;
        float f = pow(h, k);
        f += 0.06 * sin(t * 0.9 + WarpHash(pos * 0.021) * 6.28);
        f = saturate(f) * amp;
        float2 scatter = (float2(WarpHash(pos * 0.05), WarpHash(pos * 0.05 + 0.7)) * 2.0 - 1.0) * 70.0;
        float2 dir = converge + scatter - pos;
        result = dir * f * (0.6 + 1.4 * amp);
    }
    return result;
}

struct RectPSInput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

RectPSInput main(float2 position : POSITION, float4 color : COLOR, uint id : SV_VertexID) {
    RectPSInput result;

    float qt = floor(fWarpTime * 2.0);
    float h = WarpHash(float2((float)(id / 4) * 0.013, qt * 2.71));
    float amp = KeyCorruptAmp(fWarp, KeyCorruptFactor(h));
    position.xy += WarpOffset(position.xy, amp, fWarpTime, fWarpSeedX, fWarpSeedY, (float)(id / 4) * 0.013);
    result.position = mul(proj, float4(position, 0, 1));
    result.color = color;

    return result;
}
