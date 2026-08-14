// D3D11 (SM 5.0) port of note_ps.hlsl.

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

struct NotePSInput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
    float4 edges : TEXCOORD0; // left, top, right, bottom
    float4 border : COLOR1;
};

float4 main(NotePSInput input) : SV_TARGET {
    if (stripMode > 0.5)
        return (abs(input.position.x - input.edges.x) <= deflate ||
                abs(input.position.x - input.edges.z) <= deflate) ? input.border : input.color;

    return (abs(input.position.x - input.edges.x) <= deflate ||
            abs(input.position.x - input.edges.z) <= deflate ||
            abs(input.position.y - input.edges.y) <= deflate ||
            abs(input.position.y - input.edges.w) <= deflate) ? input.border : input.color;
}