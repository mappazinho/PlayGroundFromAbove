// D3D11 (SM 5.0) port of the D3D12 bloom extract compute shader (g_BloomExtractHLSL).

Texture2D<float4> g_input : register(t0);
RWTexture2D<float4> g_output : register(u0);

cbuffer cb : register(b0) {
    float g_threshold;
    float g_knee;
    float g_pregain;
    float g_padding;
};

[numthreads(16, 16, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID) {
    uint ow, oh;
    g_output.GetDimensions(ow, oh);
    if (dtid.x >= ow || dtid.y >= oh) return;

    uint iw, ih;
    g_input.GetDimensions(iw, ih);
    int2 base = int2(dtid.xy) * 2;
    float4 s00 = g_input[min(base + int2(0,0), int2(iw-1, ih-1))];
    float4 s10 = g_input[min(base + int2(1,0), int2(iw-1, ih-1))];
    float4 s01 = g_input[min(base + int2(0,1), int2(iw-1, ih-1))];
    float4 s11 = g_input[min(base + int2(1,1), int2(iw-1, ih-1))];
    float3 color = (s00.rgb + s10.rgb + s01.rgb + s11.rgb) * 0.25;

    float maxc = max(color.r, max(color.g, color.b));
    float knee2 = g_knee * 2.0;
    float soft = clamp(maxc - g_threshold + g_knee, 0.0, knee2);
    soft = (knee2 > 0.00001) ? (soft * soft / (4.0 * g_knee + 0.00001)) : 0.0;
    float contribution = max(soft, maxc - g_threshold) / max(maxc, 0.00001);
    contribution = max(contribution, 0.0);

    float3 extracted = color * contribution * g_pregain;
    g_output[dtid.xy] = float4(extracted, 1.0);
}