#include "common.hlsli"

ConstantBuffer<RootSignatureData> root : register(b0);

float4 main(RibbonPSInput input) : SV_TARGET {
    if (root.stripMode > 0.5)
        return (abs(input.position.x - input.edges.x) <= root.deflate ||
                abs(input.position.x - input.edges.z) <= root.deflate) ? input.border : input.color;

    return (abs(input.position.x - input.edges.x) <= input.tol ||
            abs(input.position.x - input.edges.z) <= input.tol ||
            abs(input.position.y - input.edges.y) <= root.deflate ||
            abs(input.position.y - input.edges.w) <= root.deflate) ? input.border : input.color;
}
