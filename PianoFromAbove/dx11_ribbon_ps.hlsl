// D3D11 (SM 5.0) port of ribbon_ps.hlsl.

cbuffer root : register(b0) {
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

struct RibbonPSInput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
    float4 edges : TEXCOORD0; // left, top, right, bottom
    float4 border : COLOR1;
    float tol : TEXCOORD1;
};

float4 main(RibbonPSInput input) : SV_TARGET {
    return (abs(input.position.x - input.edges.x) <= input.tol ||
            abs(input.position.x - input.edges.z) <= input.tol ||
            abs(input.position.y - input.edges.y) <= deflate ||
            abs(input.position.y - input.edges.w) <= deflate) ? input.border : input.color;
}
