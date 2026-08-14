#include "common.hlsli"

struct NoteData {
    uint packed;
    float pos;
    float length;
};

#define MAX_TRACK_COLORS 65536
struct TrackColor {
    uint colors[3]; // primary, dark, darker
};

struct FixedSizeData {
    float note_x[128];
    float bends[16];
};

ConstantBuffer<RootSignatureData> root : register(b0);
StructuredBuffer<FixedSizeData> fixed : register(t1);
StructuredBuffer<TrackColor> colors : register(t2);
StructuredBuffer<NoteData> note_data : register(t3);

float3 unpack_color(uint col) {
    return float3(float((col >> 16) & 0xFF) / 255.0, float((col >> 8) & 0xFF) / 255.0, float(col & 0xFF) / 255.0);
}

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
    if (root.stripMode > 0.5) {
        float strip_pos = note_data[note_index].pos / root.stripTimeSpan;
        float strip_length = max(note_data[note_index].length / root.stripTimeSpan, 0);
        x = root.notes_cy * strip_pos;
        cx = root.notes_cy * strip_length;
        sharp = ((1 << (note % 12)) & 0x54A) != 0;
        uint r = note % 12u;
        uint octave = note / 12u;
        static const uint sharps_below[12] = { 0u, 0u, 1u, 1u, 2u, 2u, 2u, 3u, 3u, 4u, 4u, 5u };
        uint whites_below = note - octave * 5u - sharps_below[r];
        float whiteH = root.white_cx;
        if (!sharp) {
            y = root.notes_y + whiteH * (float)(whites_below + 1u);
            cy = whiteH;
        } else {
            float nudge = 0.0;
            if (r == 1u || r == 6u) nudge = -0.05f;
            else if (r == 3u || r == 10u) nudge = 0.05f;
            float boundary = root.notes_y + whiteH * (float)whites_below;
            y = boundary + whiteH * (0.325f + nudge);
            cy = whiteH * 0.65f;
        }
    } else {
        sharp = ((1 << (note % 12)) & 0x54A) != 0;
        x = fixed[0].note_x[note] + fixed[0].bends[chan];
        y = round(root.notes_y + root.notes_cy * (1.0f - note_data[note_index].pos / root.timespan));
        cx = sharp ? root.white_cx * 0.65f : root.white_cx;
        cy = max(round(root.notes_cy * note_data[note_index].length / root.timespan), root.deflate);
    }
    bool is_right = vertex == 1 || vertex == 2;
    uint color_idx = root.stripMode > 0.5 ? (vertex < 2) : is_right;
    float2 base = float2(x, y);
    // Per-lane corruption: notes sharing a key share a stable level that evolves
    // in half-second steps, always a step ahead of (and hotter than) the keys.
    float qt = floor(root.fWarpTime * 2.0);
    float h = WarpHash(float2(x * 0.013, qt * 2.71));
    float amp = NoteCorruptAmp(root.fWarp, NoteCorruptFactor(h));
    // Some lanes also get their gradient scrambled (never all): the harder the
    // corruption, the more notes shuffle primary/dark/darker across corners.
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
    // Warp each corner on its own (like the keys) so notes stretch and tear
    // instead of sliding around as rigid bars.
    float2 corners[4];
    [unroll]
    for (uint v = 0; v < 4; v++) {
        float2 p = base;
        p.y -= cy * float(v < 2);
        p.x += cx * float(v == 1 || v == 2);
        corners[v] = p + WarpOffset(p, amp, root.fWarpTime, root.fWarpSeedX, root.fWarpSeedY, x * 0.013);
    }
    float2 position = corners[vertex];
    float2 minp = min(min(corners[0], corners[1]), min(corners[2], corners[3]));
    float2 maxp = max(max(corners[0], corners[1]), max(corners[2], corners[3]));

    result.position = colors[track * 16 + chan].colors[2] == 0xFFFFFFFF ? float4(0, 0, 0, 0) : mul(root.proj, float4(position, !sharp * 0.5, 1));
    result.color = float4(unpack_color(color), 0);
    result.border = float4(unpack_color(colors[track * 16 + chan].colors[2]), 0);
    result.edges = float4(minp.x, minp.y, maxp.x, maxp.y);
    //result.color = outline ? float4(0, 0, 0, 0) : float4(1, 1, 1, 0);

    return result;
}
