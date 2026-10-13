#include "common.hlsli"

// Pitch-bend ribbon notes. Instanced: SV_InstanceID picks the note, SV_VertexID walks
// 6 vertices (two triangles) per slice. A note is a vertical strip of `slices` quads
// whose horizontal offset follows the channel's sampled bend curve (t4), so the note
// itself bends instead of sliding as a rigid bar. The slice count is a power of two
// carried in bits 4..6 of the channel byte (bits 0..3 are the channel).

#define RIBBON_CURVE_N 1024

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
StructuredBuffer<float> bend_curve : register(t4); // [channel][RIBBON_CURVE_N], semitones, now -> now + timespan

float3 unpack_color(uint col) {
    return float3(float((col >> 16) & 0xFF) / 255.0, float((col >> 8) & 0xFF) / 255.0, float(col & 0xFF) / 255.0);
}

float SampleBend(uint chan, float y) {
    float u = saturate(1.0 - (y - root.notes_y) / root.notes_cy);
    float f = u * (RIBBON_CURVE_N - 1);
    uint i0 = min((uint)f, RIBBON_CURVE_N - 2);
    float a = bend_curve[chan * RIBBON_CURVE_N + i0];
    float b = bend_curve[chan * RIBBON_CURVE_N + i0 + 1];
    return lerp(a, b, saturate(f - (float)i0));
}

// Modulation (rows 16..31 of the curve, 0..1) waves the note sideways: a sine along the note,
// anchored to its bottom so the wave travels with it, whose width follows the modulation amount.
float SampleMod(uint chan, float y) {
    float u = saturate(1.0 - (y - root.notes_y) / root.notes_cy);
    float f = u * (RIBBON_CURVE_N - 1);
    uint i0 = min((uint)f, RIBBON_CURVE_N - 2);
    float a = bend_curve[(16u + chan) * RIBBON_CURVE_N + i0];
    float b = bend_curve[(16u + chan) * RIBBON_CURVE_N + i0 + 1];
    return lerp(a, b, saturate(f - (float)i0));
}

// Total horizontal offset of the note's centre line at row y: pitch bend plus modulation wave.
float RowOffset(uint chan, float y, float yb, float scale, float cx) {
    float wavelength = max(root.white_cx * 2.0f, 8.0f);
    float wave = SampleMod(chan, y) * cx * 0.7f * sin((yb - y) * 6.2831853f / wavelength);
    return SampleBend(chan, y) * scale + wave;
}
// y of slice boundary `row` (0 = note top ... slices = note bottom). The interior boundaries
// are spread over the part of the note that is actually on screen so a note much taller than
// the view still gets full resolution where it can be seen.
float RowY(uint row, uint slices, float yt, float yb) {
    float vlo = max(yt, root.notes_y);
    float vhi = min(yb, root.notes_y + root.notes_cy);
    if (vhi <= vlo) {
        vlo = yt;
        vhi = yb;
    }
    float y = lerp(vlo, vhi, (float)row / (float)slices);
    y = row == 0 ? yt : y;
    y = row >= slices ? yb : y;
    return y;
}

// ---- Dual roll (strip) ribbons ------------------------------------------------------------------
// The strip lays time along x and the keys along y, so a bend shifts a note vertically and a
// modulation wave runs along its length. The curve here covers the strip's own time window.

float SampleStripCurve(uint row, float x) {
    float u = saturate(x / root.notes_cy);
    float f = u * (RIBBON_CURVE_N - 1);
    uint i0 = min((uint)f, RIBBON_CURVE_N - 2);
    float a = bend_curve[row * RIBBON_CURVE_N + i0];
    float b = bend_curve[row * RIBBON_CURVE_N + i0 + 1];
    return lerp(a, b, saturate(f - (float)i0));
}

// Vertical offset of the note's centre line at x: pitch bend plus modulation wave.
float StripOffset(uint chan, float x, float xs, float cy) {
    float wavelength = max(cy * 12.0f, 24.0f);
    float wave = SampleStripCurve(16u + chan, x) * cy * 1.5f * sin((x - xs) * 6.2831853f / wavelength);
    return SampleStripCurve(chan, x) * (root.white_cx * (7.0f / 12.0f)) + wave;
}

float StripRowX(uint row, uint slices, float xs, float xe) {
    float vlo = max(xs, 0.0f);
    float vhi = min(xe, root.notes_cy);
    if (vhi <= vlo) {
        vlo = xs;
        vhi = xe;
    }
    float x = lerp(vlo, vhi, (float)row / (float)slices);
    x = row == 0 ? xs : x;
    x = row >= slices ? xe : x;
    return x;
}

RibbonPSInput StripMain(uint id, uint inst) {
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
    bool at_bottom = corner_bottom[corner] != 0u;
    uint row = slice + corner_right[corner];

    float xs = root.notes_cy * (note_data[inst].pos / root.stripTimeSpan);
    float xe = xs + root.notes_cy * max(note_data[inst].length / root.stripTimeSpan, 0.0f);

    bool sharp = ((1 << (note % 12)) & 0x54A) != 0;
    uint r = note % 12u;
    uint octave = note / 12u;
    static const uint sharps_below[12] = { 0u, 0u, 1u, 1u, 2u, 2u, 2u, 3u, 3u, 4u, 4u, 5u };
    uint whites_below = note - octave * 5u - sharps_below[r];
    float whiteH = root.white_cx;
    float y, cy;
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

    float rx = StripRowX(row, slices, xs, xe);
    float off = StripOffset(chan, rx, xs, cy);
    float2 p = float2(rx, (at_bottom ? y : y - cy) + off);

    float qt = floor(root.fWarpTime * 2.0);
    float h = WarpHash(float2(xs * 0.013, qt * 2.71));
    float amp = NoteCorruptAmp(root.fWarp, NoteCorruptFactor(h));
    float gh = WarpHash(float2(xs * 0.019, qt * 0.61 + 0.83));
    bool gradBroken = gh > 1.0 - min(1.0, amp) * 0.65;
    uint palette[3] = { colors[track * 16 + chan].colors[0],
                        colors[track * 16 + chan].colors[1],
                        colors[track * 16 + chan].colors[2] };
    uint color = palette[at_bottom ? 0 : 1];
    if (gradBroken) {
        float g = WarpHash(float2(xs * 0.023, qt * 0.53 + (float)(id & 3u) * 0.37 + 0.19));
        color = palette[(uint)(g * 3.0)];
    }

    float2 position = p + WarpOffset(p, amp, root.fWarpTime, root.fWarpSeedX, root.fWarpSeedY, xs * 0.013);

    float edgeL = xs;
    float edgeR = xe;
    if (amp > 0.0) {
        float2 pl = float2(xs, y - cy * 0.5f);
        float2 pr = float2(xe, y - cy * 0.5f);
        edgeL = (pl + WarpOffset(pl, amp, root.fWarpTime, root.fWarpSeedX, root.fWarpSeedY, xs * 0.013)).x;
        edgeR = (pr + WarpOffset(pr, amp, root.fWarpTime, root.fWarpSeedX, root.fWarpSeedY, xs * 0.013)).x;
    }

    result.position = colors[track * 16 + chan].colors[2] == 0xFFFFFFFF ? float4(0, 0, 0, 0) : mul(root.proj, float4(position, !sharp * 0.5, 1));
    result.color = float4(unpack_color(color), 0);
    result.border = float4(unpack_color(colors[track * 16 + chan].colors[2]), 0);
    result.edges = float4(edgeL, 0, edgeR, 0);
    result.tol = root.deflate;

    return result;
}

RibbonPSInput main(uint id : SV_VertexID, uint inst : SV_InstanceID) {
    if (root.stripMode > 0.5)
        return StripMain(id, inst);
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
    float yb = round(root.notes_y + root.notes_cy * (1.0f - note_data[inst].pos / root.timespan));
    float cx = sharp ? root.white_cx * 0.65f : root.white_cx;
    float cy = max(round(root.notes_cy * note_data[inst].length / root.timespan), root.deflate);
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

    float qt = floor(root.fWarpTime * 2.0);
    float h = WarpHash(float2(x0 * 0.013, qt * 2.71));
    float amp = NoteCorruptAmp(root.fWarp, NoteCorruptFactor(h));
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

    float2 position = p + WarpOffset(p, amp, root.fWarpTime, root.fWarpSeedX, root.fWarpSeedY, x0 * 0.013);

    // Border edges at this row (left/right) and the note's top/bottom rows.
    float2 pl = float2(x0 + bend, ry);
    float2 pr = float2(x0 + bend + cx, ry);
    float edgeL = pl.x;
    float edgeR = pr.x;
    float edgeT = yt;
    float edgeB = yb;
    if (amp > 0.0) {
        edgeL = (pl + WarpOffset(pl, amp, root.fWarpTime, root.fWarpSeedX, root.fWarpSeedY, x0 * 0.013)).x;
        edgeR = (pr + WarpOffset(pr, amp, root.fWarpTime, root.fWarpSeedX, root.fWarpSeedY, x0 * 0.013)).x;
        float bt = RowOffset(chan, yt, yb, scale, cx);
        float bb = RowOffset(chan, yb, yb, scale, cx);
        float4 wy;
        wy.x = (float2(x0 + bt, yt) + WarpOffset(float2(x0 + bt, yt), amp, root.fWarpTime, root.fWarpSeedX, root.fWarpSeedY, x0 * 0.013)).y;
        wy.y = (float2(x0 + bt + cx, yt) + WarpOffset(float2(x0 + bt + cx, yt), amp, root.fWarpTime, root.fWarpSeedX, root.fWarpSeedY, x0 * 0.013)).y;
        wy.z = (float2(x0 + bb, yb) + WarpOffset(float2(x0 + bb, yb), amp, root.fWarpTime, root.fWarpSeedX, root.fWarpSeedY, x0 * 0.013)).y;
        wy.w = (float2(x0 + bb + cx, yb) + WarpOffset(float2(x0 + bb + cx, yb), amp, root.fWarpTime, root.fWarpSeedX, root.fWarpSeedY, x0 * 0.013)).y;
        edgeT = min(wy.x, wy.y);
        edgeB = max(wy.z, wy.w);
    }

    result.position = colors[track * 16 + chan].colors[2] == 0xFFFFFFFF ? float4(0, 0, 0, 0) : mul(root.proj, float4(position, !sharp * 0.5, 1));
    result.color = float4(unpack_color(color), 0);
    result.border = float4(unpack_color(colors[track * 16 + chan].colors[2]), 0);
    result.edges = float4(edgeL, edgeT, edgeR, edgeB);
    result.tol = root.deflate * sqrt(1.0f + slope * slope);

    return result;
}
