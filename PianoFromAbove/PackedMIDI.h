// Packed MIDI store (prototype): a whole song kept as per-track compact record streams,
// cut into content-defined chunks with every distinct chunk stored once (the scheme
// Kimrashi uses). A time window of it can be expanded into an ordinary SMF byte stream,
// which the normal parser then turns into the row pool the renderer and audio read.
//
// Record format (per track):  vlv(delta << 2 | flags) [status] data
//   notes (0x9n):  key, [velocity if flags&1], [vlv(duration) if flags&2]   duration = end - start + 1, 0 = never released
//   0xAn/Bn/En:    d1, d2        0xCn/Dn: d1
// Note-offs are not stored; velocity / duration / status run from the previous record.
#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern void PRE_DbgLog(const char* format, ...);

namespace PGFAPacked
{
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;

static const u32 NEVER = 0xFFFFFFFFu;
static const size_t MAX_PENDING = (size_t)1 << 24;
static const u32 CKPT_EVERY = 256; // chunks between seek checkpoints

inline void PutVlv(std::vector<u8>& out, u64 v)
{
    u8 tmp[10]; int n = 0; tmp[n++] = v & 0x7F; v >>= 7;
    while (v) { tmp[n++] = 0x80 | (v & 0x7F); v >>= 7; }
    while (n) out.push_back(tmp[--n]);
}
inline u64 GetVlv(const u8* d, size_t& pos)
{
    u64 v = 0; u8 b;
    do { b = d[pos++]; v = (v << 7) | (b & 0x7F); } while (b & 0x80);
    return v;
}

// Walks an MTrk payload; ch(tick,status,d1,d2) for channel events, tempo(tick,us) for Set Tempo.
template <class FC, class FT>
inline u32 WalkRaw(const u8* d, size_t n, FC&& ch, FT&& tempo)
{
    size_t p = 0; u32 tick = 0; int run = 0;
    while (p < n) {
        u32 dt = 0; int k = 0; u8 b;
        do { if (p >= n) return tick; b = d[p++]; dt = (dt << 7) | (b & 0x7F); } while ((b & 0x80) && ++k < 4);
        tick += dt;
        if (p >= n) return tick;
        int st = d[p];
        if (st < 0x80) { if (!run) return tick; st = run; } else p++;
        if (st == 0xFF) {
            if (p >= n) return tick;
            int type = d[p++];
            u32 len = 0; k = 0;
            do { if (p >= n) return tick; b = d[p++]; len = (len << 7) | (b & 0x7F); } while ((b & 0x80) && ++k < 4);
            if (len > n - p) return tick;
            if (type == 0x51 && len == 3) tempo(tick, ((u32)d[p] << 16) | ((u32)d[p + 1] << 8) | d[p + 2]);
            p += len;
            if (type == 0x2F) return tick;
        } else if (st == 0xF0 || st == 0xF7) {
            u32 len = 0; k = 0;
            do { if (p >= n) return tick; b = d[p++]; len = (len << 7) | (b & 0x7F); } while ((b & 0x80) && ++k < 4);
            if (len > n - p) return tick;
            p += len;
        } else if (st >= 0xF1) {
        } else {
            run = st;
            int need = ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) ? 1 : 2;
            if (n - p < (size_t)need) return tick;
            u8 d1 = d[p] & 0x7F, d2 = need == 2 ? (d[p + 1] & 0x7F) : 0;
            p += need;
            ch(tick, (u8)st, d1, d2);
        }
    }
    return tick;
}

struct Chunk { u64 start; u32 len; };
struct Shard
{
    std::mutex m;
    std::unordered_map<u64, u32> index;
    std::vector<Chunk> chunks;
    std::vector<u8> bytes;
};
static const int SHARD_BITS = 6;

// Reader state at the start of a checkpoint interval, plus the latest note end inside it.
struct Ckpt { u32 ref; u32 tick; u8 running, vel; u32 dur; u32 maxEnd; };

struct PackedSong
{
    PackedSong() : shards(new Shard[1 << SHARD_BITS]) {}
    std::unique_ptr<Shard[]> shards;
    u16 divisionRaw = 480;
    std::vector<std::vector<u32>> refs;       // per track: chunk ids
    std::vector<std::vector<Ckpt>> ckpts;     // per track: seek checkpoints
    std::vector<std::pair<u32, u32>> tempos;  // (tick, us per quarter), sorted by tick
    u64 noteCount = 0;
    u32 endTick = 0;
    u32 firstNoteTick = 0xFFFFFFFFu;
    u32 bucketTicks = 1;                      // histogram resolution
    std::vector<u32> hist;                    // note starts per bucket
    u64 fileBytes = 0;
    u64 packedBytes = 0;
    double packMs = 0;
    bool valid = false;

    const u8* ChunkData(u32 id, u32& len) const
    {
        const Shard& sh = shards[id & ((1 << SHARD_BITS) - 1)];
        const Chunk& c = sh.chunks[id >> SHARD_BITS];
        len = c.len;
        return sh.bytes.data() + c.start;
    }
};

namespace detail
{
inline u64 HashChunk(const u8* c, size_t n)
{
    u64 h = 0x9E3779B97F4A7C15ull ^ n; size_t i = 0;
    for (; i + 8 <= n; i += 8) { u64 w; memcpy(&w, c + i, 8); h = (h ^ w) * 0xFF51AFD7ED558CCDull; h ^= h >> 32; }
    u64 t = 0; for (size_t j = n; j > i; j--) t = (t << 8) | c[j - 1];
    h = (h ^ t) * 0xC4CEB9FE1A85EC53ull; return h ^ (h >> 29);
}
inline u32 InsertChunk(PackedSong& ps, const u8* c, size_t n)
{
    u64 h = HashChunk(c, n);
    int s = (int)(h & ((1 << SHARD_BITS) - 1));
    Shard& sh = ps.shards[s];
    std::lock_guard<std::mutex> lk(sh.m);
    auto it = sh.index.find(h);
    if (it != sh.index.end()) {
        const Chunk& k = sh.chunks[it->second];
        if (k.len == n && !memcmp(&sh.bytes[k.start], c, n)) return (it->second << SHARD_BITS) | s;
    }
    u32 local = (u32)sh.chunks.size();
    sh.chunks.push_back(Chunk{ sh.bytes.size(), (u32)n });
    sh.bytes.insert(sh.bytes.end(), c, c + n);
    sh.index.emplace(h, local);
    return (local << SHARD_BITS) | s;
}

inline const u64* Gear()
{
    static u64 t[256]; static std::once_flag f;
    std::call_once(f, [] { u64 x = 0x9E3779B97F4A7C15ull; for (int i = 0; i < 256; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; t[i] = x; } });
    return t;
}

struct Pend { u32 tick; u8 status, d1, d2, open; u32 end; };

struct Packer
{
    PackedSong& ps; const u64* gear;
    std::vector<u8> data; std::vector<u32> refs; std::vector<Ckpt> ck;
    u32 lastTick = 0; u8 running = 0, runVel = 0; u32 runDur = 0; u64 hash = 0; u32 curMax = 0; u64 notes = 0; u32 firstNote = 0xFFFFFFFFu;
    explicit Packer(PackedSong& p) : ps(p), gear(Gear()) { ck.push_back(Ckpt{ 0, 0, 0, 0, 0, 0 }); }

    void flush()
    {
        if (!data.empty()) {
            refs.push_back(InsertChunk(ps, data.data(), data.size())); data.clear();
            if (refs.size() % CKPT_EVERY == 0) {
                ck.back().maxEnd = curMax; curMax = 0;
                ck.push_back(Ckpt{ (u32)refs.size(), lastTick, running, runVel, runDur, 0 });
            }
        }
        hash = 0;
    }
    void CutAfter(size_t start)
    {
        for (size_t i = start; i < data.size(); i++) hash = (hash << 1) + gear[data[i]];
        if ((data.size() >= 64 && (hash & 255) == 0) || data.size() >= 1024) flush();
    }
    void Emit(const Pend& p)
    {
        u64 delta = (u64)(u32)(p.tick - lastTick) << 2; lastTick = p.tick;
        size_t start = data.size();
        if ((p.status & 0xF0) != 0x90) {
            PutVlv(data, delta);
            if (p.status != running) { data.push_back(p.status); running = p.status; }
            data.push_back(p.d1);
            if ((p.status & 0xF0) != 0xC0 && (p.status & 0xF0) != 0xD0) data.push_back(p.d2);
            CutAfter(start); return;
        }
        u8 vel = p.d2;
        u32 dur = p.end == NEVER ? 0 : p.end - p.tick + 1;
        bool nv = vel != runVel, nd = dur != runDur;
        PutVlv(data, delta | (u64)nv | (u64)nd << 1);
        if (p.status != running) { data.push_back(p.status); running = p.status; }
        data.push_back(p.d1);
        if (nv) { data.push_back(vel); runVel = vel; }
        if (nd) { PutVlv(data, dur); runDur = dur; }
        curMax = (std::max)(curMax, p.end == NEVER ? NEVER : p.end);
        if ( !notes ) firstNote = p.tick;
        notes++;
        CutAfter(start);
    }
};

struct Scratch
{
    std::deque<Pend> q;
    std::vector<std::deque<u64>> fifo{ 16 * 128 };
    std::vector<u16> touched; std::vector<u8> isTouched = std::vector<u8>(16 * 128, 0);
    void Reset() { for (u16 t : touched) { fifo[t].clear(); isTouched[t] = 0; } touched.clear(); q.clear(); }
};

struct TrackOut
{
    std::vector<u32> refs; std::vector<Ckpt> ck; std::vector<std::pair<u32, u32>> tempos;
    u32 endTick = 0; u64 notes = 0; u32 firstNote = 0xFFFFFFFFu;
};

inline void PackTrack(PackedSong& ps, const u8* d, size_t n, Scratch& sc, TrackOut& out)
{
    sc.Reset();
    Packer pk(ps);
    u64 base = 0;
    auto drain = [&]() { while (!sc.q.empty() && !sc.q.front().open) { pk.Emit(sc.q.front()); sc.q.pop_front(); base++; } };
    u32 last = WalkRaw(d, n,
        [&](u32 tick, u8 status, u8 d1, u8 d2) {
            int type = status & 0xF0; int idx = ((status & 0x0F) << 7) | d1;
            if (type == 0x90 && d2 > 0) {
                sc.q.push_back(Pend{ tick, status, d1, d2, 1, NEVER });
                if (!sc.isTouched[idx]) { sc.isTouched[idx] = 1; sc.touched.push_back((u16)idx); }
                sc.fifo[idx].push_back(base + sc.q.size() - 1);
            } else if (type == 0x80 || type == 0x90) {
                auto& f = sc.fifo[idx];
                while (!f.empty() && f.front() < base) f.pop_front();
                if (!f.empty()) { Pend& p = sc.q[f.front() - base]; p.end = tick; p.open = 0; f.pop_front(); }
            } else {
                sc.q.push_back(Pend{ tick, status, d1, d2, 0, 0 });
            }
            drain();
            if (sc.q.size() > MAX_PENDING) { sc.q.front().open = 0; sc.q.front().end = NEVER; drain(); }
        },
        [&](u32 tick, u32 us) { out.tempos.emplace_back(tick, us); });
    for (auto& p : sc.q) p.open = 0;
    drain();
    pk.flush();
    pk.ck.back().maxEnd = (std::max)(pk.ck.back().maxEnd, pk.curMax);
    out.refs = std::move(pk.refs); out.ck = std::move(pk.ck); out.endTick = last; out.notes = pk.notes; out.firstNote = pk.firstNote;
}

// Calls cb(tick, status, d1, d2, dur) for the records of checkpoint interval j of a track.
// Stops early (returns false) when cb returns false.
template <class F>
inline bool DecodeInterval(const PackedSong& ps, size_t track, size_t j, F&& cb)
{
    const auto& refs = ps.refs[track]; const auto& ck = ps.ckpts[track];
    u32 refBegin = ck[j].ref, refEnd = (j + 1 < ck.size()) ? ck[j + 1].ref : (u32)refs.size();
    u32 tick = ck[j].tick; u8 running = ck[j].running, vel = ck[j].vel; u32 dur = ck[j].dur;
    for (u32 r = refBegin; r < refEnd; r++) {
        u32 len; const u8* d = ps.ChunkData(refs[r], len);
        size_t p = 0;
        while (p < len) {
            u64 v = GetVlv(d, p);
            tick += (u32)(v >> 2);
            if (d[p] & 0x80) running = d[p++];
            u8 st = running;
            if ((st & 0xF0) == 0x90) {
                u8 key = d[p++];
                if (v & 1) vel = d[p++];
                if (v & 2) dur = (u32)GetVlv(d, p);
                if (!cb(tick, st, key, vel, dur)) return false;
            } else if ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) {
                if (!cb(tick, st, d[p], (u8)0, 0u)) return false; p += 1;
            } else {
                if (!cb(tick, st, d[p], d[p + 1], 0u)) return false; p += 2;
            }
        }
    }
    return true;
}
} // namespace detail

// Packs a whole MIDI file. progress (optional) counts finished tracks.
inline bool PackFile(const std::wstring& path, PackedSong& ps, std::atomic<uint64_t>* progress = nullptr, uint64_t* totalTracks = nullptr)
{
    using namespace detail;
    LARGE_INTEGER t0q, t1q, fq; QueryPerformanceCounter(&t0q); QueryPerformanceFrequency(&fq);
    HANDLE hf = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(hf, &sz); size_t fsize = (size_t)sz.QuadPart;
    if (fsize < 22) { CloseHandle(hf); return false; }
    HANDLE hm = CreateFileMappingW(hf, NULL, PAGE_READONLY, 0, 0, NULL);
    const u8* view = hm ? (const u8*)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0) : nullptr;
    if (hm) CloseHandle(hm);
    CloseHandle(hf);
    if (!view) return false;
    struct Unmap { const u8* v; ~Unmap() { UnmapViewOfFile(v); } } unmap{ view };

    // page the file in sequentially so the workers don't fault on a cold disk at random offsets
    {
        typedef BOOL(WINAPI * PF)(HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG);
        PF pf = (PF)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "PrefetchVirtualMemory");
        const size_t W = (size_t)64 << 20; volatile u8 sink = 0;
        auto pre = [&](size_t off) { if (!pf || off >= fsize) return; WIN32_MEMORY_RANGE_ENTRY r{ (void*)(view + off), (std::min)(W, fsize - off) }; pf(GetCurrentProcess(), 1, &r, 0); };
        pre(0);
        for (size_t off = 0; off < fsize; off += W) { pre(off + W); size_t e = (std::min)(off + W, fsize); for (size_t p = off; p < e; p += 4096) sink = sink + view[p]; }
    }

    if (memcmp(view, "MThd", 4)) return false;
    u32 hdr = (view[4] << 24) | (view[5] << 16) | (view[6] << 8) | view[7];
    ps.divisionRaw = (u16)((view[12] << 8) | view[13]);
    if (!ps.divisionRaw) return false;
    struct TR { size_t off, size; };
    std::vector<TR> tracks;
    for (size_t p = 8 + (std::max<u32>)(hdr, 6); p + 8 <= fsize;) {
        if (memcmp(view + p, "MTrk", 4)) break;
        u32 sz4 = (view[p + 4] << 24) | (view[p + 5] << 16) | (view[p + 6] << 8) | view[p + 7];
        size_t s = (std::min<size_t>)(sz4, fsize - p - 8);
        tracks.push_back({ p + 8, s }); p += 8 + sz4;
    }
    if (tracks.empty()) return false;
    if (totalTracks) *totalTracks = tracks.size();

    std::vector<size_t> order(tracks.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return tracks[a].size > tracks[b].size; });
    std::vector<TrackOut> outs(tracks.size());
    std::atomic<size_t> next{ 0 };
    unsigned nThreads = std::thread::hardware_concurrency(); if (!nThreads) nThreads = 4;
    nThreads = (std::min)(nThreads, 16u);
    auto worker = [&]() {
        Scratch sc; size_t k;
        while ((k = next.fetch_add(1)) < order.size()) {
            size_t i = order[k];
            PackTrack(ps, view + tracks[i].off, tracks[i].size, sc, outs[i]);
            VirtualUnlock((LPVOID)(view + tracks[i].off), tracks[i].size);
            if (progress) progress->fetch_add(1);
        }
    };
    {
        std::vector<std::thread> th;
        try { for (unsigned i = 1; i < nThreads; i++) th.emplace_back(worker); } catch (...) {}
        worker();
        for (auto& t : th) t.join();
    }

    ps.refs.resize(tracks.size()); ps.ckpts.resize(tracks.size());
    for (size_t i = 0; i < tracks.size(); i++) {
        ps.refs[i] = std::move(outs[i].refs); ps.ckpts[i] = std::move(outs[i].ck);
        ps.noteCount += outs[i].notes; ps.endTick = (std::max)(ps.endTick, outs[i].endTick); ps.firstNoteTick = (std::min)(ps.firstNoteTick, outs[i].firstNote);
        for (auto& t : outs[i].tempos) ps.tempos.push_back(t);
    }
    std::stable_sort(ps.tempos.begin(), ps.tempos.end(), [](const std::pair<u32, u32>& a, const std::pair<u32, u32>& b) { return a.first < b.first; });

    // density histogram (note starts per bucket), so a window can be sized by note count
    {
        const u32 div = ps.divisionRaw & 0x7FFF;
        u64 buckets = ps.endTick / (std::max<u32>)(1, div / 16) + 1;
        ps.bucketTicks = (u32)(std::max<u64>)((std::max<u32>)(1, div / 16), (ps.endTick + (1u << 20) - 1) / (1u << 20));
        buckets = ps.endTick / ps.bucketTicks + 1;
        ps.hist.assign((size_t)buckets, 0);
        std::atomic<size_t> nx{ 0 };
        std::mutex mu;
        auto hw = [&]() {
            std::vector<u32> local(ps.hist.size(), 0); size_t i;
            while ((i = nx.fetch_add(1)) < ps.refs.size())
                for (size_t j = 0; j < ps.ckpts[i].size(); j++)
                    DecodeInterval(ps, i, j, [&](u32 tick, u8 st, u8, u8, u32) { if ((st & 0xF0) == 0x90) local[tick / ps.bucketTicks]++; return true; });
            std::lock_guard<std::mutex> lk(mu);
            for (size_t b = 0; b < local.size(); b++) ps.hist[b] += local[b];
        };
        std::vector<std::thread> th;
        try { for (unsigned i = 1; i < nThreads; i++) th.emplace_back(hw); } catch (...) {}
        hw();
        for (auto& t : th) t.join();
    }

    u64 store = 0, chunks = 0, refsN = 0, ck = 0;
    for (int s = 0; s < (1 << SHARD_BITS); s++) { store += ps.shards[s].bytes.size(); chunks += ps.shards[s].chunks.size(); ps.shards[s].index = {}; }
    for (auto& r : ps.refs) refsN += r.size();
    for (auto& c : ps.ckpts) ck += c.size();
    ps.packedBytes = store + chunks * sizeof(Chunk) + refsN * 4 + ck * sizeof(Ckpt);
    ps.fileBytes = fsize;
    QueryPerformanceCounter(&t1q);
    ps.packMs = (double)(t1q.QuadPart - t0q.QuadPart) * 1000.0 / (double)fq.QuadPart;
    ps.valid = true;
    PRE_DbgLog("PACKED: %llu notes in %zu tracks, file %.1f MB -> packed %.1f MB (%.3f B/note), %.0f ms",
        (unsigned long long)ps.noteCount, tracks.size(), fsize / 1048576.0, ps.packedBytes / 1048576.0,
        ps.noteCount ? (double)ps.packedBytes / ps.noteCount : 0.0, ps.packMs);
    return true;
}

// Microseconds <-> ticks through the song's tempo map.
inline u32 TickAtUs(const PackedSong& ps, long long us)
{
    const u32 div = ps.divisionRaw & 0x7FFF;
    if (us <= 0) return 0;
    long long accUs = 0; u32 lastTick = 0; u32 tempo = 500000;
    for (auto& t : ps.tempos) {
        long long segUs = (long long)((u64)(t.first - lastTick) * tempo / div);
        if (accUs + segUs > us) break;
        accUs += segUs; lastTick = t.first; tempo = t.second ? t.second : 1;
    }
    u64 tick = (u64)lastTick + (u64)(us - accUs) * div / tempo;
    return tick > 0xFFFFFFFEull ? 0xFFFFFFFEu : (u32)tick;
}
inline long long UsAtTick(const PackedSong& ps, u32 tick)
{
    const u32 div = ps.divisionRaw & 0x7FFF;
    long long accUs = 0; u32 lastTick = 0; u32 tempo = 500000;
    for (auto& t : ps.tempos) {
        if (t.first >= tick) break;
        accUs += (long long)((u64)(t.first - lastTick) * tempo / div);
        lastTick = t.first; tempo = t.second ? t.second : 1;
    }
    return accUs + (long long)((u64)(tick - lastTick) * tempo / div);
}

// First tick t1 > t0 such that [t0, t1) holds about maxNotes note starts (at least minTicks long).
inline u32 WindowEnd(const PackedSong& ps, u32 t0, u64 maxNotes, u32 minTicks)
{
    u64 acc = 0; size_t b = t0 / ps.bucketTicks;
    for (; b < ps.hist.size(); b++) {
        acc += ps.hist[b];
        if (acc >= maxNotes) break;
    }
    u64 t1 = (u64)(b + 1) * ps.bucketTicks;
    t1 = (std::max)(t1, (u64)t0 + minTicks);
    return t1 > 0xFFFFFFFEull ? 0xFFFFFFFEu : (u32)t1;
}

struct SynthStats { u64 notes = 0, others = 0; double ms = 0; };

// Expands the part of the song that touches [t0, t1) into a format-1 SMF image: every note that
// starts in the window, or started earlier and is still sounding at t0, plus the other channel
// events inside the window, plus the whole tempo map. Ticks are absolute, so times line up with
// the full song.
inline std::vector<u8> SynthWindow(const PackedSong& ps, u32 t0, u32 t1, SynthStats* stats = nullptr)
{
    using namespace detail;
    LARGE_INTEGER a, b, fq; QueryPerformanceCounter(&a); QueryPerformanceFrequency(&fq);
    const size_t nTracks = ps.refs.size();
    struct Ev { u32 tick, order, seq; u8 status, d1, d2; u32 aux; };
    std::vector<std::vector<u8>> outs(nTracks);
    std::atomic<size_t> nx{ 0 };
    std::atomic<u64> noteTotal{ 0 }, otherTotal{ 0 };
    unsigned nThreads = std::thread::hardware_concurrency(); if (!nThreads) nThreads = 4;
    nThreads = (std::min)(nThreads, 16u);
    auto worker = [&]() {
        std::vector<Ev> evs; size_t i;
        while ((i = nx.fetch_add(1)) < nTracks) {
            evs.clear(); u32 seq = 0; u64 notes = 0, others = 0;
            const auto& ck = ps.ckpts[i];
            for (size_t j = 0; j < ck.size(); j++) {
                if (ck[j].tick >= t1) break;
                const u32 nextTick = (j + 1 < ck.size()) ? ck[j + 1].tick : NEVER;
                if (!(nextTick >= t0 || ck[j].maxEnd >= t0)) continue;
                bool more = DecodeInterval(ps, i, j, [&](u32 tick, u8 st, u8 d1, u8 d2, u32 dur) {
                    if (tick >= t1) return false;
                    if ((st & 0xF0) == 0x90) {
                        const u32 end = dur ? tick + dur - 1 : NEVER;
                        if (end != NEVER && end < t0) return true;
                        evs.push_back(Ev{ tick, 1, seq++, st, d1, d2, 0 });
                        if (end != NEVER) evs.push_back(Ev{ end, (u32)(end == tick ? 2 : 0), seq++, (u8)(0x80 | (st & 0x0F)), d1, 64, 0 });
                        notes++;
                    } else if (tick >= t0) {
                        evs.push_back(Ev{ tick, 1, seq++, st, d1, d2, 0 });
                        others++;
                    }
                    return true;
                });
                if (!more) break;
            }
            if (i == 0)
                for (auto& t : ps.tempos) if (t.first < t1) evs.push_back(Ev{ t.first, 0, seq++, 0xFF, 0, 0, t.second });
            std::sort(evs.begin(), evs.end(), [](const Ev& x, const Ev& y) {
                if (x.tick != y.tick) return x.tick < y.tick;
                if (x.order != y.order) return x.order < y.order;
                return x.seq < y.seq;
            });
            std::vector<u8>& o = outs[i];
            o.reserve(evs.size() * 4 + 16);
            u32 last = 0; u8 running = 0;
            for (const Ev& e : evs) {
                PutVlv(o, e.tick - last); last = e.tick;
                if (e.status == 0xFF) {
                    o.push_back(0xFF); o.push_back(0x51); o.push_back(3);
                    o.push_back((u8)(e.aux >> 16)); o.push_back((u8)(e.aux >> 8)); o.push_back((u8)e.aux);
                    running = 0; continue;
                }
                if (e.status != running) { o.push_back(e.status); running = e.status; }
                o.push_back(e.d1);
                if ((e.status & 0xF0) != 0xC0 && (e.status & 0xF0) != 0xD0) o.push_back(e.d2);
            }
            o.push_back(0); o.push_back(0xFF); o.push_back(0x2F); o.push_back(0);
            noteTotal += notes; otherTotal += others;
        }
    };
    {
        std::vector<std::thread> th;
        try { for (unsigned i = 1; i < nThreads; i++) th.emplace_back(worker); } catch (...) {}
        worker();
        for (auto& t : th) t.join();
    }
    size_t total = 14;
    for (auto& o : outs) total += 8 + o.size();
    std::vector<u8> smf; smf.reserve(total);
    const u8 hd[14] = { 'M','T','h','d',0,0,0,6,0,1,(u8)(nTracks >> 8),(u8)nTracks,(u8)(ps.divisionRaw >> 8),(u8)ps.divisionRaw };
    smf.insert(smf.end(), hd, hd + 14);
    for (auto& o : outs) {
        const u8 th[8] = { 'M','T','r','k',(u8)(o.size() >> 24),(u8)(o.size() >> 16),(u8)(o.size() >> 8),(u8)o.size() };
        smf.insert(smf.end(), th, th + 8);
        smf.insert(smf.end(), o.begin(), o.end());
        std::vector<u8>().swap(o);
    }
    QueryPerformanceCounter(&b);
    if (stats) { stats->notes = noteTotal; stats->others = otherTotal; stats->ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)fq.QuadPart; }
    return smf;
}
} // namespace PGFAPacked

namespace PGFAPacked
{
// Handed to the MainScreen constructor on the thread that builds the next window (the constructor
// runs on whichever thread calls `new`), so a continuation reuses the already packed song.
struct Continuation
{
    bool active = false;
    std::shared_ptr<PackedSong> song;
    long long startUs = 0;
    long long minSpanUs = 0;
    uint64_t maxNotes = 12000000;
};
inline thread_local Continuation g_Cont;
}
