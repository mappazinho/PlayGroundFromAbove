#pragma once
// TEMP diagnostic: per-frame inclusive timings of image-buffer stages on the game thread.
// Compiled out unless PGFA_IMGBUF_TRACE is defined. Slow frames are written to the heartbeat log.

#ifdef PGFA_IMGBUF_TRACE
#include <Windows.h>
#include <chrono>
#include <cstdio>

extern DWORD g_dwGameThreadId;
void HeartbeatLog(const char* tag);

enum PGFAIbStage {
    IB_PREWARM, IB_TRYCOLLECT, IB_PROGRESS, IB_PRIME, IB_FULLPRIME, IB_ESTIMATE, IB_EXACT,
    IB_ENSUREINDEX, IB_NOTESIB, IB_STRIPIB, IB_PACKEDPUMP, IB_RENDERCHUNK, IB_RENDERIB,
    IB_LOGIC, IB_RENDER, IB_CHANGESTATE, IB_SCREENDTOR, IB_SCREENINIT, IB_JUMPTO, IB_ADVANCE, IB_SKIPPED,
    IB_AUDIOSTART, IB_AUDIOSTOP, IB_RGLOBALS, IB_RNOTES, IB_RKEYS, IB_RTEXT, IB_ENDSCENE, IB_PRESENT, IB_COUNT
};
inline const char* PGFAIbStageName(int s) {
    static const char* names[IB_COUNT] = { "prewarm", "trycollect", "progress", "prime", "fullprime",
        "estimate", "exact", "ensureidx", "notesIB", "stripIB", "packedpump", "renderchunk", "renderIB",
        "logic", "render", "changestate", "dtor", "init", "jumpto", "advance", "skipped", "audiostart", "audiostop",
        "rglobals", "rnotes", "rkeys", "rtext", "endscene", "present" };
    return names[s];
}
struct PGFAIbTrace { double ms[IB_COUNT] = {}; unsigned calls[IB_COUNT] = {}; };
inline PGFAIbTrace& PGFAIbTraceGet() { static PGFAIbTrace t; return t; }

struct PGFAIbScope {
    int stage;
    bool active;
    std::chrono::steady_clock::time_point t0;
    explicit PGFAIbScope(int s) : stage(s), active(GetCurrentThreadId() == g_dwGameThreadId) {
        if (active) t0 = std::chrono::steady_clock::now();
    }
    ~PGFAIbScope() {
        if (!active) return;
        auto& t = PGFAIbTraceGet();
        t.ms[stage] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        t.calls[stage]++;
    }
};
#define IB_TRACE_CAT2(a, b) a##b
#define IB_TRACE_CAT(a, b) IB_TRACE_CAT2(a, b)
#define IB_TRACE(stage) PGFAIbScope IB_TRACE_CAT(ibTraceScope, __LINE__)(stage)

inline void PGFAIbTraceFlush(double frameMs) {
    auto& t = PGFAIbTraceGet();
    if (frameMs > 25.0) {
        char buf[1024];
        int n = sprintf_s(buf, "ibtrace frame=%.1fms", frameMs);
        for (int i = 0; i < IB_COUNT && n > 0 && n < (int)sizeof(buf) - 48; i++)
            if (t.calls[i])
                n += sprintf_s(buf + n, sizeof(buf) - n, " %s=%.1f/%u", PGFAIbStageName(i), t.ms[i], t.calls[i]);
        HeartbeatLog(buf);
    }
    t = PGFAIbTrace{};
}
#else
#define IB_TRACE(stage) ((void)0)
#endif
