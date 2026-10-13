// D3D11 (SM 5.0) port of ribbon_vs.hlsl. Must match the D3D12 shader behavior: same cbuffer
// layout, same SRV registers, same slice/curve math and warp/corruption math.

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

#define MAX_TRACK_COLORS 65536
#define RIBBON_CURVE_N 1024
struct TrackColor {
    uint colors[3]; // primary, dark, darker
};

struct FixedSizeData {
    float note_x[128];
    float bends[16];
};

struct NoteData {
    uint packed;
    float pos;
    float length;
};

StructuredBuffer<FixedSizeData> fixed : register(t1);
StructuredBuffer<TrackColor> colors : register(t2);
StructuredBuffer<NoteData> note_data : register(t3);
StructuredBuffer<float> bend_curve : register(t4); // [channel][RIBBON_CURVE_N], semitones, now -> now + timespan

float WarpHash(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float NoteCorruptFactor(float h) {
    return 0.4 + 2.1 * pow(h, 1.5);
}

float NoteCorruptAmp(float w, float f) {
    return saturate(w / 0.55) * f * 1.15;
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

float3 unpack_color(uint col) {
    return float3(float((col >> 16) & 0xFF) / 255.0, float((col >> 8) & 0xFF) / 255.0, float(col & 0xFF) / 255.0);
}

struct RibbonPSInput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
    float4 edges : TEXCOORD0; // left, top, right, bottom
    float4 border : COLOR1;
    float tol : TEXCOORD1;
};

float SampleBend(uint chan, float y) {
    float u = saturate(1.0 - (y - notes_y) / notes_cy);
    float f = u * (RIBBON_CURVE_N - 1);
    uint i0 = min((uint)f, RIBBON_CURVE_N - 2);
    float a = bend_curve[chan * RIBBON_CURVE_N + i0];
    float b = bend_curve[chan * RIBBON_CURVE_N + i0 + 1];
    return lerp(a, b, saturate(f - (float)i0));
}

// Modulation (rows 16..31 of the curve, 0..1) waves the note sideways: a sine along the note,
// anchored to its bottom so the wave travels with it, whose width follows the modulation amount.
float SampleMod(uint chan, float y) {
    float u = saturate(1.0 - (y - notes_y) / notes_cy);
    float f = u * (RIBBON_CURVE_N - 1);
    uint i0 = min((uint)f, RIBBON_CURVE_N - 2);
    float a = bend_curve[(16u + chan) * RIBBON_CURVE_N + i0];
    float b = bend_curve[(16u + chan) * RIBBON_CURVE_N + i0 + 1];
    return lerp(a, b, saturate(f - (float)i0));
}

// Total horizontal offset of the note's centre line at row y: pitch bend plus modulation wave.
float RowOffset(uint chan, float y, float yb, float scale, float cx) {
    float wavelength = max(white_cx * 2.0f, 8.0f);
    float wave = SampleMod(chan, y) * cx * 0.7f * sin((yb - y) * 6.2831853f / wavelength);
    return SampleBend(chan, y) * scale + wave;
}
float RowY(uint row, uint slices, float yt, float yb) {
    float vlo = max(yt, notes_y);
    float vhi = min(yb, notes_y + notes_cy);
    if (vhi <= vlo) {
        vlo = yt;
        vhi = yb;
    }
    float y = lerp(vlo, vhi, (float)row / (float)slices);
    y = row == 0 ? yt : y;
    y = row >= slices ? yb : y;
    return y;
}

RibbonPSInput main(uint id : SV_VertexID, uint inst : SV_InstanceID) {
    RibbonPSInput result;

    uint packed = note_data[inst].packed;
    uint note = packed & 0xFF;
    uint chanByte = (packed >> 8) & 0xFF;
    uint chan = chanByte & 15u;
    uint slices = 1u << ((chanByte >> 4) & 7u);
    uint track = ((packed >> 16) & 0xFFFF) % MAX_TRACK_COLORS;

    static const uint corner_right[6] = { 0u, 1u, 1u, 0u, 1u, 0u };
    static const uint corner_bottom[6] = { 0u, 0u, 1u, 0u, 1u, 1u };
    uint slice = id / 6u;
    uint corner = id % 6u;
    bool is_right = corner_right[corner] != 0u;
    uint row = slice + corner_bottom[corner];

    bool sharp = ((1 << (note % 12)) & 0x54A) != 0;
    float x0 = fixed[0].note_x[note];
    float yb = round(notes_y + notes_cy * (1.0f - note_data[inst].pos / timespan));
    float cx = sharp ? white_cx * 0.65f : white_cx;
    float cy = max(round(notes_cy * note_data[inst].length / timespan), deflate);
    float yt = yb - cy;
    float scale = fixed[0].note_x[1] - fixed[0].note_x[0];

    float ry0 = RowY(slice, slices, yt, yb);
    float ry1 = RowY(slice + 1u, slices, yt, yb);
    float bend0 = RowOffset(chan, ry0, yb, scale, cx);
    float bend1 = RowOffset(chan, ry1, yb, scale, cx);
    float ry = row == slice ? ry0 : ry1;
    float bend = row == slice ? bend0 : bend1;
    float slope = (bend1 - bend0) / max(ry1 - ry0, 1.0f);

    float2 p = float2(x0 + bend + (is_right ? cx : 0.0f), ry);

    float qt = floor(fWarpTime * 2.0);
    float h = WarpHash(float2(x0 * 0.013, qt * 2.71));
    float amp = NoteCorruptAmp(fWarp, NoteCorruptFactor(h));
    float gh = WarpHash(float2(x0 * 0.019, qt * 0.61 + 0.83));
    bool gradBroken = gh > 1.0 - min(1.0, amp) * 0.65;
    uint palette[3] = { colors[track * 16 + chan].colors[0],
                        colors[track * 16 + chan].colors[1],
                        colors[track * 16 + chan].colors[2] };
    uint color = palette[is_right ? 1 : 0];
    if (gradBroken) {
        float g = WarpHash(float2(x0 * 0.023, qt * 0.53 + (float)(id & 3u) * 0.37 + 0.19));
        color = palette[(uint)(g * 3.0)];
    }

    float2 position = p + WarpOffset(p, amp, fWarpTime, fWarpSeedX, fWarpSeedY, x0 * 0.013);

    float2 pl = float2(x0 + bend, ry);
    float2 pr = float2(x0 + bend + cx, ry);
    float edgeL = pl.x;
    float edgeR = pr.x;
    float edgeT = yt;
    float edgeB = yb;
    if (amp > 0.0) {
        edgeL = (pl + WarpOffset(pl, amp, fWarpTime, fWarpSeedX, fWarpSeedY, x0 * 0.013)).x;
        edgeR = (pr + WarpOffset(pr, amp, fWarpTime, fWarpSeedX, fWarpSeedY, x0 * 0.013)).x;
        float bt = RowOffset(chan, yt, yb, scale, cx);
        float bb = RowOffset(chan, yb, yb, scale, cx);
        float4 wy;
        wy.x = (float2(x0 + bt, yt) + WarpOffset(float2(x0 + bt, yt), amp, fWarpTime, fWarpSeedX, fWarpSeedY, x0 * 0.013)).y;
        wy.y = (float2(x0 + bt + cx, yt) + WarpOffset(float2(x0 + bt + cx, yt), amp, fWarpTime, fWarpSeedX, fWarpSeedY, x0 * 0.013)).y;
        wy.z = (float2(x0 + bb, yb) + WarpOffset(float2(x0 + bb, yb), amp, fWarpTime, fWarpSeedX, fWarpSeedY, x0 * 0.013)).y;
        wy.w = (float2(x0 + bb + cx, yb) + WarpOffset(float2(x0 + bb + cx, yb), amp, fWarpTime, fWarpSeedX, fWarpSeedY, x0 * 0.013)).y;
        edgeT = min(wy.x, wy.y);
        edgeB = max(wy.z, wy.w);
    }

    result.position = colors[track * 16 + chan].colors[2] == 0xFFFFFFFF ? float4(0, 0, 0, 0) : mul(proj, float4(position, !sharp * 0.5, 1));
    result.color = float4(unpack_color(color), 0);
    result.border = float4(unpack_color(colors[track * 16 + chan].colors[2]), 0);
    result.edges = float4(edgeL, edgeT, edgeR, edgeB);
    result.tol = deflate * sqrt(1.0f + slope * slope);

    return result;
}
