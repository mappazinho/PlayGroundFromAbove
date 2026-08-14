// D3D11 (SM 5.0) port of rect_ps.hlsl.

struct RectPSInput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

float4 main(RectPSInput input) : SV_TARGET {
    return input.color;
}
