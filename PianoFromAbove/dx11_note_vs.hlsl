// D3D11 (SM 5.0) port of common.hlsli + note_vs.hlsl. Must match the D3D12
// shader behavior: same cbuffer layout, same SRV registers, same warp/corruption math.

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

struct NotePSInput {
    float4 position : SV_POSITION;
    float4 color : COLOR;
    float4 edges : TEXCOORD0; // left, top, right, bottom
    float4 border : COLOR1;
};

NotePSInput main(uint id : SV_VertexID) {
    NotePSInput result;
    uint note_index = id / 4;
    uint vertex = id % 4;

    uint packed = note_data[note_index].packed;
    uint note = packed & 0xFF;
    uint chan = (packed >> 8) & 0xFF;
    uint track = ((packed >> 16) & 0xFFFF) % MAX_TRACK_COLORS;

    float x, y, cx, cy;
    bool sharp;
    if (stripMode > 0.5) {
        float strip_pos = note_data[note_index].pos / stripTimeSpan;
        float strip_length = max(note_data[note_index].length / stripTimeSpan, 0);
        x = notes_cy * strip_pos;
        cx = notes_cy * strip_length;
        sharp = ((1 << (note % 12)) & 0x54A) != 0;
        uint r = note % 12u;
        uint octave = note / 12u;
        static const uint sharps_below[12] = { 0u, 0u, 1u, 1u, 2u, 2u, 2u, 3u, 3u, 4u, 4u, 5u };
        uint whites_below = note - octave * 5u - sharps_below[r];
        float whiteH = white_cx;
        if (!sharp) {
            y = notes_y + whiteH * (float)(whites_below + 1u);
            cy = whiteH;
        } else {
            float nudge = 0.0;
            if (r == 1u || r == 6u) nudge = -0.05f;
            else if (r == 3u || r == 10u) nudge = 0.05f;
            float boundary = notes_y + whiteH * (float)whites_below;
            y = boundary + whiteH * (0.325f + nudge);
            cy = whiteH * 0.65f;
        }
    } else {
        sharp = ((1 << (note % 12)) & 0x54A) != 0;
        x = fixed[0].note_x[note] + fixed[0].bends[chan];
        y = round(notes_y + notes_cy * (1.0f - note_data[note_index].pos / timespan));
        cx = sharp ? white_cx * 0.65f : white_cx;
        cy = max(round(notes_cy * note_data[note_index].length / timespan), deflate);
    }
    bool is_right = vertex == 1 || vertex == 2;
    uint color_idx = stripMode > 0.5 ? (vertex < 2) : is_right;
    float2 base = float2(x, y);
    float qt = floor(fWarpTime * 2.0);
    float h = WarpHash(float2(x * 0.013, qt * 2.71));
    float amp = NoteCorruptAmp(fWarp, NoteCorruptFactor(h));
    float gh = WarpHash(float2(x * 0.019, qt * 0.61 + 0.83));
    bool gradBroken = gh > 1.0 - min(1.0, amp) * 0.65;
    uint palette[3] = { colors[track * 16 + chan].colors[0],
                        colors[track * 16 + chan].colors[1],
                        colors[track * 16 + chan].colors[2] };
    uint color = palette[color_idx];
    if (gradBroken) {
        float g = WarpHash(float2(x * 0.023, qt * 0.53 + (float)vertex * 0.37 + 0.19));
        color = palette[(uint)(g * 3.0)];
    }
    float2 corners[4];
    [unroll]
    for (uint v = 0; v < 4; v++) {
        float2 p = base;
        p.y -= cy * float(v < 2);
        p.x += cx * float(v == 1 || v == 2);
        corners[v] = p + WarpOffset(p, amp, fWarpTime, fWarpSeedX, fWarpSeedY, x * 0.013);
    }
    float2 position = corners[vertex];
    float2 minp = min(min(corners[0], corners[1]), min(corners[2], corners[3]));
    float2 maxp = max(max(corners[0], corners[1]), max(corners[2], corners[3]));

    result.position = colors[track * 16 + chan].colors[2] == 0xFFFFFFFF ? float4(0, 0, 0, 0) : mul(proj, float4(position, !sharp * 0.5, 1));
    result.color = float4(unpack_color(color), 0);
    result.border = float4(unpack_color(colors[track * 16 + chan].colors[2]), 0);
    result.edges = float4(minp.x, minp.y, maxp.x, maxp.y);

    return result;
}
