// D3D11 (SM 5.0) port of the D3D12 blur compute shader (g_BlurHLSL).

RWTexture2D<float4> g_output : register(u0);
Texture2D<float4> g_input : register(t0);

cbuffer cb : register(b0) {
    int g_blurDirection;
    int g_sigma;
    int2 g_padding;
};

[numthreads(256, 1, 1)]
void CSMain(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID, uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint w, h;
    g_input.GetDimensions(w, h);
    int sigma = max(g_sigma, 1);
    int radius = min(sigma * 3, 63);
    float sigma2 = (float)(sigma * sigma);
    int2 coord;
    if (g_blurDirection == 0) {
        coord = int2((int)dispatchThreadId.x, (int)groupId.y);
    } else {
        coord = int2((int)groupId.x, (int)(groupId.y * 256 + groupThreadId.x));
    }
    if (coord.x >= (int)w || coord.y >= (int)h) return;
    float4 result = 0;
    float totalW = 0;
    [loop] for (int i = -radius; i <= radius; i++) {
        float d = (float)i;
        float wgt = exp(-(d * d) / (2.0f * sigma2));
        int2 sc = coord;
        if (g_blurDirection == 0) sc.x = clamp(coord.x + i, 0, (int)w - 1);
        else sc.y = clamp(coord.y + i, 0, (int)h - 1);
        result += g_input[sc] * wgt;
        totalW += wgt;
    }
    float4 blurred = result / totalW;
    blurred.a = 1.0; // Force opaque so the blur is always visible
    g_output[coord] = blurred;
}