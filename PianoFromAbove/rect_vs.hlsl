#include "common.hlsli"

ConstantBuffer<RootSignatureData> consts : register(b0);

RectPSInput main(float2 position : POSITION, float4 color : COLOR, uint id : SV_VertexID) {
    RectPSInput result;

    // Each quad (4 consecutive vertices) gets its own stable corruption level;
    // the hash only shifts in half-second steps so keys hold their shape between steps.
    float qt = floor(consts.fWarpTime * 2.0);
    float h = WarpHash(float2((float)(id / 4) * 0.013, qt * 2.71));
    float amp = KeyCorruptAmp(consts.fWarp, KeyCorruptFactor(h));
    position.xy += WarpOffset(position.xy, amp, consts.fWarpTime, consts.fWarpSeedX, consts.fWarpSeedY, (float)(id / 4) * 0.013);
    result.position = mul(consts.proj, float4(position, 0, 1));
    result.color = color;

    return result;
}