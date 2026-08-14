// D3D11 (SM 5.0) port of background_vs.hlsl (fullscreen triangle, no inputs).

struct BackgroundPSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

BackgroundPSInput main(uint id : SV_VertexID) {
    BackgroundPSInput result;

    // https://gist.github.com/rorydriscoll/1495603/56b12e19c62828bc2ecf28e6a90b65108879f461
    result.uv = float2((id << 1) & 2, id & 2);
    result.position = float4(result.uv * float2(2, -2) + float2(-1, 1), 0, 1);

    return result;
}
