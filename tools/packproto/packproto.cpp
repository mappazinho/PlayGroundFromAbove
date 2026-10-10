// Prototype: pack a MIDI file the way Kimrashi does (per-track record streams, content-defined
// chunks, every distinct chunk stored once) and report size / speed / memory. Optionally verifies
// that decoding gives back exactly the notes and events that were in the file.
//
//   proto.exe file.mid [--verify] [--threads N] [--nodedup]
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using std::vector;
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;

static const u32 NEVER = 0xFFFFFFFFu;
static const size_t MAX_PENDING = (size_t)1 << 24;

static double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void Mem(const char* tag) {
    PROCESS_MEMORY_COUNTERS_EX p{ sizeof(p) }; GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&p, sizeof(p));
    printf("  [%s] ws=%.0f MB peakWS=%.0f MB private=%.0f MB\n", tag, p.WorkingSetSize / 1048576.0, p.PeakWorkingSetSize / 1048576.0, p.PrivateUsage / 1048576.0);
}
static inline u64 mix(u64 x) { x += 0x9E3779B97F4A7C15ull; x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull; x = (x ^ (x >> 27)) * 0x94D049BB133111EBull; return x ^ (x >> 31); }

// ---- variable length quantities (MIDI style, big-endian 7-bit groups) -----------------------
static inline void putVlv(vector<u8>& out, u64 v) {
    u8 tmp[10]; int n = 0; tmp[n++] = v & 0x7F; v >>= 7;
    while (v) { tmp[n++] = 0x80 | (v & 0x7F); v >>= 7; }
    while (n) out.push_back(tmp[--n]);
}
static inline u64 getVlv(const u8* d, size_t& pos) {
    u64 v = 0; u8 b;
    do { b = d[pos++]; v = (v << 7) | (b & 0x7F); } while (b & 0x80);
    return v;
}

// ---- raw track walker ------------------------------------------------------------------------
// Calls ch(tick,status,d1,d2) for channel events and tempo(tick,us) for Set Tempo. Returns last tick.
template <class FC, class FT>
static u32 walkRaw(const u8* d, size_t n, FC&& ch, FT&& tempo) {
    size_t p = 0; u32 tick = 0; int run = 0;
    while (p < n) {
        // delta
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
            // system common / realtime: no data we care about
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

// ---- chunk store ------------------------------------------------------------------------------
struct Chunk { u64 start; u32 len; };
struct Shard {
    std::mutex m;
    std::unordered_map<u64, u32> index;
    vector<Chunk> chunks;
    vector<u8> bytes;
};
static const int SHARD_BITS = 6;
static Shard g_shards[1 << SHARD_BITS];
static bool g_dedup = true;
static std::atomic<u64> g_chunksTotal{0};

static u64 hashChunk(const u8* c, size_t n) {
    u64 h = 0x9E3779B97F4A7C15ull ^ n; size_t i = 0;
    for (; i + 8 <= n; i += 8) { u64 w; memcpy(&w, c + i, 8); h = (h ^ w) * 0xFF51AFD7ED558CCDull; h ^= h >> 32; }
    u64 t = 0; for (size_t j = n; j > i; j--) t = (t << 8) | c[j - 1];
    h = (h ^ t) * 0xC4CEB9FE1A85EC53ull; return h ^ (h >> 29);
}
static u32 insertChunk(const u8* c, size_t n) {
    g_chunksTotal++;
    u64 h = hashChunk(c, n);
    int s = (int)(h & ((1 << SHARD_BITS) - 1));
    std::lock_guard<std::mutex> lk(g_shards[s].m);
    Shard& sh = g_shards[s];
    if (g_dedup) {
        auto it = sh.index.find(h);
        if (it != sh.index.end()) {
            const Chunk& k = sh.chunks[it->second];
            if (k.len == n && !memcmp(&sh.bytes[k.start], c, n)) return (it->second << SHARD_BITS) | s;
        }
    }
    u32 local = (u32)sh.chunks.size();
    sh.chunks.push_back(Chunk{ sh.bytes.size(), (u32)n });
    sh.bytes.insert(sh.bytes.end(), c, c + n);
    if (g_dedup) sh.index.emplace(h, local);
    return (local << SHARD_BITS) | s;
}
static inline const u8* chunkData(u32 id, u32& len) {
    Shard& sh = g_shards[id & ((1 << SHARD_BITS) - 1)]; const Chunk& c = sh.chunks[id >> SHARD_BITS];
    len = c.len; return sh.bytes.data() + c.start;
}

static u64 GEAR[256];
static void initGear() { u64 x = 0x9E3779B97F4A7C15ull; for (int i = 0; i < 256; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; GEAR[i] = x; } }

// ---- packing ----------------------------------------------------------------------------------
struct Pend { u32 tick; u8 status, d1, d2, open; u32 end; };
struct Stats { u64 notes = 0, twoByte = 0, newVel = 0, newDur = 0, longDelta = 0, statusByte = 0, otherBytes = 0, durBytes = 0, logical = 0, forced = 0; };

struct Packer {
    vector<u8> data;
    vector<u32> refs;
    Stats st;
    u32 lastTick = 0; u8 running = 0, runVel = 0; u32 runDur = 0; u64 hash = 0;

    void flush() {
        if (!data.empty()) { st.logical += data.size(); refs.push_back(insertChunk(data.data(), data.size())); data.clear(); }
        hash = 0;
    }
    void cutAfter(size_t start) {
        for (size_t i = start; i < data.size(); i++) hash = (hash << 1) + GEAR[data[i]];
        if ((data.size() >= 64 && (hash & 255) == 0) || data.size() >= 1024) flush();
    }
    void emit(const Pend& p) {
        u64 delta = (u64)(u32)(p.tick - lastTick) << 2; lastTick = p.tick;
        size_t start = data.size();
        if ((p.status & 0xF0) != 0x90) {
            putVlv(data, delta);
            if (p.status != running) { data.push_back(p.status); running = p.status; }
            data.push_back(p.d1);
            if ((p.status & 0xF0) != 0xC0 && (p.status & 0xF0) != 0xD0) data.push_back(p.d2);
            st.otherBytes += data.size() - start;
            cutAfter(start); return;
        }
        u8 vel = p.d2;
        u32 dur = p.end == NEVER ? 0 : p.end - p.tick + 1;
        bool nv = vel != runVel, nd = dur != runDur;
        putVlv(data, delta | (u64)nv | (u64)nd << 1);
        bool longDelta = data.size() - start > 1;
        bool ns = p.status != running;
        if (ns) { data.push_back(p.status); running = p.status; }
        data.push_back(p.d1);
        if (nv) { data.push_back(vel); runVel = vel; }
        if (nd) { size_t b = data.size(); putVlv(data, dur); st.durBytes += data.size() - b; runDur = dur; }
        st.notes++; st.newVel += nv; st.newDur += nd; st.longDelta += longDelta; st.statusByte += ns; st.twoByte += (data.size() - start == 2);
        cutAfter(start);
    }
};

struct Scratch {
    std::deque<Pend> q;
    vector<std::deque<u64>> fifo{ 16 * 128 };
    vector<u16> touched; vector<u8> isTouched = vector<u8>(16 * 128, 0);
    void reset() { for (u16 t : touched) { fifo[t].clear(); isTouched[t] = 0; } touched.clear(); q.clear(); }
};

struct PackedTrack { vector<u32> refs; Stats st; vector<std::pair<u32, u32>> tempos; u32 endTick = 0; };

static PackedTrack packTrack(const u8* d, size_t n, Scratch& sc) {
    sc.reset();
    Packer pk; PackedTrack out;
    u64 base = 0;
    auto drain = [&]() {
        while (!sc.q.empty() && !sc.q.front().open) { pk.emit(sc.q.front()); sc.q.pop_front(); base++; }
    };
    u32 last = walkRaw(d, n,
        [&](u32 tick, u8 status, u8 d1, u8 d2) {
            int type = status & 0xF0; int idx = ((status & 0x0F) << 7) | d1;
            if (type == 0x90 && d2 > 0) {
                sc.q.push_back(Pend{ tick, status, d1, d2, 1, NEVER });
                if (!sc.isTouched[idx]) { sc.isTouched[idx] = 1; sc.touched.push_back((u16)idx); }
                sc.fifo[idx].push_back(base + sc.q.size() - 1);
            } else if (type == 0x80 || type == 0x90) {
                auto& f = sc.fifo[idx];
                while (!f.empty() && f.front() < base) f.pop_front(); // note already force-closed
                if (!f.empty()) { Pend& p = sc.q[f.front() - base]; p.end = tick; p.open = 0; f.pop_front(); }
            } else {
                sc.q.push_back(Pend{ tick, status, d1, d2, 0, 0 });
            }
            drain();
            if (sc.q.size() > MAX_PENDING) { sc.q.front().open = 0; sc.q.front().end = NEVER; pk.st.forced++; drain(); }
        },
        [&](u32 tick, u32 us) { out.tempos.emplace_back(tick, us); });
    for (auto& p : sc.q) p.open = 0;
    drain();
    pk.flush();
    out.refs = std::move(pk.refs); out.st = pk.st; out.endTick = last;
    return out;
}

// ---- decoding ---------------------------------------------------------------------------------
struct Reader { u32 tick = 0; u8 running = 0, vel = 0; u32 dur = 0; };
// cb(tick, status, d1, d2or0, dur) ; dur only for notes: 0 = never released
template <class F>
static void decodeTrack(const vector<u32>& refs, F&& cb) {
    Reader r;
    for (u32 id : refs) {
        u32 len; const u8* d = chunkData(id, len);
        size_t p = 0;
        while (p < len) {
            u64 v = getVlv(d, p);
            r.tick += (u32)(v >> 2);
            if (d[p] & 0x80) r.running = d[p++];
            u8 st = r.running;
            if ((st & 0xF0) == 0x90) {
                u8 key = d[p++];
                if (v & 1) r.vel = d[p++];
                if (v & 2) r.dur = (u32)getVlv(d, p);
                cb(r.tick, st, key, r.vel, r.dur);
            } else if ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) {
                cb(r.tick, st, d[p], (u8)0, 0u); p += 1;
            } else {
                cb(r.tick, st, d[p], d[p + 1], 0u); p += 2;
            }
        }
    }
}

// ---- driver -----------------------------------------------------------------------------------
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { printf("usage: proto file.mid [--verify] [--threads N] [--nodedup]\n"); return 1; }
    bool verify = false; unsigned nThreads = std::thread::hardware_concurrency();
    for (int i = 2; i < argc; i++) {
        if (!wcscmp(argv[i], L"--verify")) verify = true;
        else if (!wcscmp(argv[i], L"--nodedup")) g_dedup = false;
        else if (!wcscmp(argv[i], L"--threads") && i + 1 < argc) nThreads = _wtoi(argv[++i]);
    }
    if (!nThreads) nThreads = 4;
    initGear();
    double t0 = NowMs();
    HANDLE hf = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) { printf("open failed\n"); return 1; }
    LARGE_INTEGER sz; GetFileSizeEx(hf, &sz); size_t fsize = (size_t)sz.QuadPart;
    HANDLE hm = CreateFileMappingW(hf, NULL, PAGE_READONLY, 0, 0, NULL);
    const u8* view = (const u8*)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    if (!view) { printf("map failed\n"); return 1; }
    // sequential page-in so the worker threads don't fault randomly on a cold file
    {
        typedef BOOL(WINAPI* PF)(HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG);
        PF pf = (PF)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "PrefetchVirtualMemory");
        const size_t W = (size_t)64 << 20; volatile u8 sink = 0;
        auto pre = [&](size_t off) { if (!pf || off >= fsize) return; WIN32_MEMORY_RANGE_ENTRY r{ (void*)(view + off), std::min(W, fsize - off) }; pf(GetCurrentProcess(), 1, &r, 0); };
        pre(0);
        for (size_t off = 0; off < fsize; off += W) { pre(off + W); size_t e = std::min(off + W, fsize); for (size_t p = off; p < e; p += 4096) sink = sink + view[p]; }
    }
    double tMap = NowMs();
    if (fsize < 14 || memcmp(view, "MThd", 4)) { printf("not a MIDI file\n"); return 1; }
    u32 hdr = (view[4] << 24) | (view[5] << 16) | (view[6] << 8) | view[7];
    u16 division = (view[12] << 8) | view[13];
    struct TR { size_t off, size; };
    vector<TR> tracks;
    for (size_t p = 8 + std::max<u32>(hdr, 6); p + 8 <= fsize;) {
        if (memcmp(view + p, "MTrk", 4)) break;
        u32 sz4 = (view[p + 4] << 24) | (view[p + 5] << 16) | (view[p + 6] << 8) | view[p + 7];
        size_t sz = std::min<size_t>(sz4, fsize - p - 8);
        tracks.push_back({ p + 8, sz }); p += 8 + sz4;
    }
    printf("file %.2f MB, division %u, tracks %zu, threads %u, dedup %s\n", fsize / 1048576.0, division, tracks.size(), nThreads, g_dedup ? "on" : "off");
    Mem("mapped");

    // ---- pack ----
    vector<size_t> order(tracks.size()); for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return tracks[a].size > tracks[b].size; });
    vector<PackedTrack> packed(tracks.size());
    std::atomic<size_t> next{ 0 };
    auto worker = [&]() {
        Scratch sc; size_t k;
        while ((k = next.fetch_add(1)) < order.size()) {
            size_t i = order[k];
            packed[i] = packTrack(view + tracks[i].off, tracks[i].size, sc);
            VirtualUnlock((LPVOID)(view + tracks[i].off), tracks[i].size); // drop the file pages from our working set
        }
    };
    { vector<std::thread> th; for (unsigned i = 1; i < nThreads; i++) th.emplace_back(worker); worker(); for (auto& t : th) t.join(); }
    double tPack = NowMs();
    Mem("packed");

    Stats tot; u64 refsTotal = 0, tempoCount = 0;
    for (auto& p : packed) {
        tot.notes += p.st.notes; tot.twoByte += p.st.twoByte; tot.newVel += p.st.newVel; tot.newDur += p.st.newDur; tot.longDelta += p.st.longDelta;
        tot.statusByte += p.st.statusByte; tot.otherBytes += p.st.otherBytes; tot.durBytes += p.st.durBytes; tot.logical += p.st.logical; tot.forced += p.st.forced;
        refsTotal += p.refs.size(); tempoCount += p.tempos.size();
    }
    u64 storeBytes = 0, distinct = 0;
    for (auto& s : g_shards) { storeBytes += s.bytes.size(); distinct += s.chunks.size(); }
    u64 packedBytes = storeBytes + distinct * sizeof(Chunk) + refsTotal * 4;
    printf("\nnotes            %llu\n", (unsigned long long)tot.notes);
    printf("2-byte notes     %.1f%%   new vel %.1f%%   new dur %.1f%%   long delta %.1f%%   status byte %.1f%%\n",
        100.0 * tot.twoByte / std::max<u64>(1, tot.notes), 100.0 * tot.newVel / std::max<u64>(1, tot.notes), 100.0 * tot.newDur / std::max<u64>(1, tot.notes),
        100.0 * tot.longDelta / std::max<u64>(1, tot.notes), 100.0 * tot.statusByte / std::max<u64>(1, tot.notes));
    printf("record bytes     %.1f MB (%.2f B/note, before chunk dedup)\n", tot.logical / 1048576.0, (double)tot.logical / std::max<u64>(1, tot.notes));
    printf("chunks           %llu total, %llu distinct (%.1f%% unique)\n", (unsigned long long)g_chunksTotal.load(), (unsigned long long)distinct, 100.0 * distinct / std::max<u64>(1, g_chunksTotal.load()));
    printf("PACKED           %.1f MB  = %.3f B/note  (store %.1f MB + chunk table %.1f MB + refs %.1f MB)   ratio to file %.3f\n",
        packedBytes / 1048576.0, (double)packedBytes / std::max<u64>(1, tot.notes), storeBytes / 1048576.0, distinct * sizeof(Chunk) / 1048576.0, refsTotal * 4 / 1048576.0, (double)packedBytes / fsize);
    printf("force-closed     %llu   tempo events %llu\n", (unsigned long long)tot.forced, (unsigned long long)tempoCount);
    printf("TIMES  map/prefetch %.0f ms   pack %.0f ms   total load %.0f ms   (%.1f M notes/s)\n", tMap - t0, tPack - tMap, tPack - t0, tot.notes / 1000.0 / std::max(1.0, tPack - t0));

    // ---- decode throughput (what playback has to do: walk every track's records) ----
    {
        std::atomic<size_t> nx{ 0 }; std::atomic<u64> cnt{ 0 };
        auto dec = [&]() { u64 c = 0, sum = 0; size_t i; while ((i = nx.fetch_add(1)) < packed.size()) decodeTrack(packed[i].refs, [&](u32 tick, u8 st, u8 a, u8 b, u32 dur) { c++; sum += tick + a + b + dur; }); cnt += c; if (sum == 0xDEADBEEF) putchar(0); };
        double a = NowMs(); vector<std::thread> th; for (unsigned i = 1; i < nThreads; i++) th.emplace_back(dec); dec(); for (auto& t : th) t.join();
        double b = NowMs();
        printf("DECODE all tracks, %u threads: %llu records in %.0f ms (%.0f M rec/s)\n", nThreads, (unsigned long long)cnt.load(), b - a, cnt.load() / 1000.0 / std::max(1.0, b - a));
        double a1 = NowMs(); nx = 0; cnt = 0; dec(); double b1 = NowMs(); (void)a1; (void)b1;
    }

    // ---- verify ----
    if (verify) {
        std::atomic<size_t> nx{ 0 }; std::atomic<u64> bad{ 0 };
        auto vw = [&]() {
            size_t i;
            vector<int> open(16 * 128, 0);
            while ((i = nx.fetch_add(1)) < tracks.size()) {
                std::fill(open.begin(), open.end(), 0);
                u64 rOn = 0, rEnd = 0, rOth = 0, rNotes = 0;
                walkRaw(view + tracks[i].off, tracks[i].size,
                    [&](u32 tick, u8 st, u8 d1, u8 d2) {
                        int type = st & 0xF0; int idx = ((st & 0x0F) << 7) | d1;
                        if (type == 0x90 && d2 > 0) { open[idx]++; rOn += mix(((u64)tick << 32) | ((u64)(st & 15) << 16) | ((u64)d1 << 8) | d2); rNotes++; }
                        else if (type == 0x80 || type == 0x90) { if (open[idx] > 0) { open[idx]--; rEnd += mix(((u64)tick << 32) | ((u64)(st & 15) << 16) | ((u64)d1 << 8) | 0x1000); } }
                        else rOth += mix(((u64)tick << 32) | ((u64)st << 16) | ((u64)d1 << 8) | d2 | 0x2000000000000ull);
                    }, [](u32, u32) {});
                u64 dOn = 0, dEnd = 0, dOth = 0, dNotes = 0;
                decodeTrack(packed[i].refs, [&](u32 tick, u8 st, u8 a, u8 b, u32 dur) {
                    if ((st & 0xF0) == 0x90) {
                        dOn += mix(((u64)tick << 32) | ((u64)(st & 15) << 16) | ((u64)a << 8) | b); dNotes++;
                        if (dur) dEnd += mix(((u64)(tick + dur - 1) << 32) | ((u64)(st & 15) << 16) | ((u64)a << 8) | 0x1000);
                    } else dOth += mix(((u64)tick << 32) | ((u64)st << 16) | ((u64)a << 8) | b | 0x2000000000000ull);
                });
                // notes still open at the end of the track have no end in either view
                if (rOn != dOn || rEnd != dEnd || rOth != dOth || rNotes != dNotes) { if (!bad.fetch_add(1)) printf("  MISMATCH track %zu: notes %llu/%llu on %d end %d other %d\n", i, (unsigned long long)rNotes, (unsigned long long)dNotes, rOn == dOn, rEnd == dEnd, rOth == dOth); }
            }
        };
        double a = NowMs(); vector<std::thread> th; for (unsigned i = 1; i < nThreads; i++) th.emplace_back(vw); vw(); for (auto& t : th) t.join();
        printf("VERIFY %s  (%llu bad tracks of %zu, %.0f ms)\n", bad ? "FAILED" : "OK: every note, note end and other event round-trips", (unsigned long long)bad.load(), tracks.size(), NowMs() - a);
    }
    Mem("end");
    return 0;
}
