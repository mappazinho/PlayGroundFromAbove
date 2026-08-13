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
    float2 position = float2(x, y);
    position.y -= cy * float(vertex < 2);
    position.x += cx * float(vertex == 1 || vertex == 2);
    
    result.position = colors[track * 16 + chan].colors[2] == 0xFFFFFFFF ? float4(0, 0, 0, 0) : mul(root.proj, float4(position, !sharp * 0.5, 1));
    result.color = float4(unpack_color(colors[track * 16 + chan].colors[color_idx]), 0);
    result.border = float4(unpack_color(colors[track * 16 + chan].colors[2]), 0);
    //result.edges = float4(left_top, right_bottom);
    result.edges = float4(x, y, x + cx, y - cy);
    //result.color = outline ? float4(0, 0, 0, 0) : float4(1, 1, 1, 0);

    return result;
}
