#pragma once
// Song-wide image buffer note store for packed songs.
//
// A packed song only keeps a window of itself expanded into MIDI events, and every window used to
// start the image buffer over: prepared chunks, baked GPU chunks and the "wait for dense buffers"
// preparation were all tied to the window. This store prepares every chunk of the whole song once,
// straight from the packed data, on worker threads. Chunks are keyed by song time, so they stay
// valid across window swaps and seeks; only a layout change (note speed, notes area height, track
// count) starts it over.
//
// Each chunk is compacted the same way the prepared path compacts dense chunks: per key and pixel
// row only the newest note that covers it is kept, then runs of the same note are merged. A chunk
// is therefore at most 128 * rows notes however many notes the song has there, which keeps both
// memory and GPU baking bounded.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "PackedMIDI.h"

void HeartbeatLog(const char* tag);

namespace PGFADense {

using PGFAPacked::PackedSong;

struct Params {
    long long timeSpan = 0; // chunk length in microseconds (one screen of notes)
    int rows = 0;           // pixel rows of a chunk (notes area height)
    size_t trackCount = 0;
    bool operator==(const Params& o) const {
        return timeSpan == o.timeSpan && rows == o.rows && trackCount == o.trackCount;
    }
    bool operator!=(const Params& o) const { return !(*this == o); }
};

struct Progress {
    size_t done = 0;
    size_t total = 0;
    bool active = false;
    bool Complete() const { return active && done >= total; }
};

// Tick <-> microsecond conversion through the song's tempo map, with the same integer math as
// PGFAPacked::UsAtTick / TickAtUs but a binary search instead of a walk over every tempo event.
struct TempoMap {
    std::vector<uint32_t> tick;  // segment start tick
    std::vector<long long> us;   // microseconds at segment start
    std::vector<uint32_t> tempo; // microseconds per quarter in the segment
    uint32_t div = 480;

    void Build(const PackedSong& ps)
    {
        div = (ps.divisionRaw & 0x7FFF) ? (ps.divisionRaw & 0x7FFF) : 480;
        tick.assign(1, 0); us.assign(1, 0); tempo.assign(1, 500000);
        for (const auto& t : ps.tempos) {
            const uint32_t lastTick = tick.back();
            const long long seg = (long long)((uint64_t)(t.first - lastTick) * tempo.back() / div);
            if (t.first == lastTick) {
                tempo.back() = t.second ? t.second : 1;
                continue;
            }
            tick.push_back(t.first);
            us.push_back(us.back() + seg);
            tempo.push_back(t.second ? t.second : 1);
        }
    }
    long long UsAt(uint32_t t) const
    {
        size_t i = (size_t)(std::upper_bound(tick.begin(), tick.end(), t) - tick.begin());
        i = i ? i - 1 : 0;
        return us[i] + (long long)((uint64_t)(t - tick[i]) * tempo[i] / div);
    }
    uint32_t TickAt(long long u) const
    {
        if (u <= 0) return 0;
        size_t i = (size_t)(std::upper_bound(us.begin(), us.end(), u) - us.begin());
        i = i ? i - 1 : 0;
        const uint64_t t = (uint64_t)tick[i] + (uint64_t)(u - us[i]) * div / tempo[i];
        return t > 0xFFFFFFFEull ? 0xFFFFFFFEu : (uint32_t)t;
    }
};

// Per track and checkpoint interval: first and last record tick and the latest note end, from one
// decoding pass over the song. Lets a chunk decode only the intervals that can touch it.
struct IntervalInfo {
    uint32_t firstTick = 0;
    uint32_t reach = 0;          // max(last record tick, latest note end) in the interval
    uint32_t prefixReach = 0;    // max reach over intervals [0, j]
};

struct SongIndex {
    std::vector<std::vector<IntervalInfo>> tracks;
    TempoMap tempo;
    long long firstNoteUs = 0;
    long long lastNoteEndUs = 0;
};

inline bool BuildSongIndex(const PackedSong& ps, SongIndex& out)
{
    out.tempo.Build(ps);
    const size_t nTracks = ps.refs.size();
    out.tracks.assign(nTracks, {});
    std::atomic<size_t> next{ 0 };
    std::atomic<uint64_t> lastEndTick{ 0 };
    auto worker = [&]() {
        uint64_t localLastEnd = 0;
        for (size_t i; (i = next.fetch_add(1)) < nTracks; ) {
            const auto& ck = ps.ckpts[i];
            auto& info = out.tracks[i];
            info.resize(ck.size());
            uint32_t prefix = 0;
            for (size_t j = 0; j < ck.size(); ++j) {
                uint32_t first = 0xFFFFFFFFu, reach = ck[j].tick;
                PGFAPacked::detail::DecodeInterval(ps, i, j, [&](uint32_t tick, uint8_t st, uint8_t, uint8_t vel, uint32_t dur) {
                    if (first == 0xFFFFFFFFu) first = tick;
                    reach = (std::max)(reach, tick);
                    if ((st & 0xF0) == 0x90 && vel > 0 && dur > 0) {
                        const uint64_t end = (uint64_t)tick + dur - 1;
                        const uint32_t end32 = end > 0xFFFFFFFEull ? 0xFFFFFFFEu : (uint32_t)end;
                        reach = (std::max)(reach, end32);
                        localLastEnd = (std::max)(localLastEnd, (uint64_t)end32);
                    }
                    return true;
                });
                info[j].firstTick = first == 0xFFFFFFFFu ? ck[j].tick : first;
                info[j].reach = reach;
                prefix = (std::max)(prefix, reach);
                info[j].prefixReach = prefix;
            }
        }
        uint64_t cur = lastEndTick.load();
        while (localLastEnd > cur && !lastEndTick.compare_exchange_weak(cur, localLastEnd)) {}
    };
    unsigned n = std::thread::hardware_concurrency();
    n = (std::max)(1u, (std::min)(n ? n : 4u, 16u));
    std::vector<std::thread> th;
    try { for (unsigned k = 1; k < n; ++k) th.emplace_back(worker); } catch (...) {}
    worker();
    for (auto& t : th) t.join();
    out.firstNoteUs = ps.firstNoteTick == 0xFFFFFFFFu ? 0 : out.tempo.UsAt(ps.firstNoteTick);
    out.lastNoteEndUs = out.tempo.UsAt((uint32_t)(std::min)(lastEndTick.load(), (uint64_t)0xFFFFFFFEu));
    return true;
}

struct RawNote {
    long long start;  // song microseconds
    long long end;    // song microseconds (note-off time)
    uint32_t seq;     // decode order within the track, the last tie-break
    uint16_t track;
    uint8_t key;
    uint8_t channel;
};

// Collects every note that overlaps [t0, t0 + T) from the packed song.
inline void CollectChunkNotes(const PackedSong& ps, const SongIndex& idx, long long t0, long long T,
                              std::vector<RawNote>& out)
{
    out.clear();
    const long long t1 = t0 + T;
    // Conservative tick bounds; exact filtering below is done in microseconds.
    const uint32_t tickLo = idx.tempo.TickAt(t0) > 0 ? idx.tempo.TickAt(t0) - 1 : 0;
    const uint32_t tickHiRaw = idx.tempo.TickAt(t1);
    const uint32_t tickHi = tickHiRaw >= 0xFFFFFFFDu ? 0xFFFFFFFEu : tickHiRaw + 2;
    for (size_t i = 0; i < ps.refs.size(); ++i) {
        const auto& ck = ps.ckpts[i];
        const auto& info = idx.tracks[i];
        // Intervals starting at or after tickHi cannot contain anything that starts before t1.
        size_t jEnd = (size_t)(std::lower_bound(ck.begin(), ck.end(), tickHi,
            [](const PGFAPacked::Ckpt& c, uint32_t t) { return c.tick < t; }) - ck.begin());
        uint32_t seq = 0;
        for (size_t j = jEnd; j-- > 0; ) {
            if (info[j].prefixReach < tickLo)
                break; // nothing at or before this interval reaches the chunk
            if (info[j].reach < tickLo || info[j].firstTick >= tickHi)
                continue;
            PGFAPacked::detail::DecodeInterval(ps, i, j, [&](uint32_t tick, uint8_t st, uint8_t key, uint8_t vel, uint32_t dur) {
                if (tick >= tickHi)
                    return false;
                if ((st & 0xF0) != 0x90 || vel == 0 || dur == 0)
                    return true; // velocity-0 and never-released notes have no note-off: not drawn
                const uint64_t endTick = (uint64_t)tick + dur - 1;
                if (endTick < tickLo)
                    return true;
                const long long s = idx.tempo.UsAt(tick);
                if (s >= t1)
                    return true;
                const long long e = idx.tempo.UsAt((uint32_t)(std::min)(endTick, (uint64_t)0xFFFFFFFEu));
                if (e < t0)
                    return true;
                out.push_back(RawNote{ s, e, seq++, (uint16_t)i, key, (uint8_t)(st & 0x0F) });
                return true;
            });
        }
    }
}

// Same per-key, per-pixel-row compaction as ImageBufferPreparedBuild: notes are applied newest
// first (the order the regular path draws them, so the newest wins under depth LESS) and each cell
// keeps the first note that covers it; runs of one note are then merged back into a single quad.
inline std::vector<NoteData> CompactChunk(std::vector<RawNote>& notes, long long t0, long long T, int rows)
{
    std::vector<NoteData> result;
    if (notes.empty() || T <= 0 || rows <= 0)
        return result;
    std::sort(notes.begin(), notes.end(), [](const RawNote& a, const RawNote& b) {
        if (a.start != b.start) return a.start > b.start;
        if (a.track != b.track) return a.track > b.track;
        return a.seq > b.seq;
    });

    const uint32_t emptyStyle = 0xffffffffu;
    std::vector<uint32_t> cells((size_t)128 * rows, emptyStyle);
    std::vector<uint32_t> owners((size_t)128 * rows, 0xffffffffu);
    std::vector<int> next((size_t)128 * (rows + 1));
    for (int key = 0; key < 128; ++key) {
        const size_t base = (size_t)key * (rows + 1);
        for (int row = 0; row <= rows; ++row)
            next[base + row] = row;
    }
    auto findNext = [&](int key, int row) {
        const size_t base = (size_t)key * (rows + 1);
        int root = row;
        while (next[base + root] != root)
            root = next[base + root];
        while (next[base + row] != row) {
            const int old = row;
            row = next[base + row];
            next[base + old] = root;
        }
        return root;
    };

    size_t remaining = (size_t)128 * rows;
    for (size_t i = 0; i < notes.size() && remaining; ++i) {
        const RawNote& n = notes[i];
        const long long relStart = n.start - t0;
        const long long relEnd = n.end - t0;
        const double clippedStart = (std::max)(0.0, (double)relStart);
        const double clippedEnd = (std::min)((double)T, (double)relEnd);
        int row0 = (int)std::floor(clippedStart * rows / (double)T);
        int row1 = (int)std::ceil(clippedEnd * rows / (double)T);
        row0 = (std::min)((std::max)(row0, 0), rows - 1);
        row1 = (std::min)((std::max)(row1, row0 + 1), rows);
        const uint32_t style = ((uint32_t)n.track << 8) | (uint32_t)n.channel;
        const int key = n.key & 0x7F;
        int row = findNext(key, row0);
        while (row < row1) {
            const size_t cell = (size_t)key * rows + row;
            cells[cell] = style;
            owners[cell] = (uint32_t)i;
            --remaining;
            const size_t base = (size_t)key * (rows + 1);
            next[base + row] = findNext(key, row + 1);
            row = next[base + row];
        }
    }

    for (int key = 0; key < 128; ++key) {
        const size_t base = (size_t)key * rows;
        int row = 0;
        while (row < rows) {
            const uint32_t style = cells[base + row];
            if (style == emptyStyle) { ++row; continue; }
            const uint32_t owner = owners[base + row];
            const int begin = row;
            while (row < rows && cells[base + row] == style && owners[base + row] == owner)
                ++row;
            const double pos = (double)begin * (double)T / (double)rows;
            const double end = (double)row * (double)T / (double)rows;
            result.push_back(NoteData{
                .key = (uint8_t)key,
                .channel = (uint8_t)(style & 0xff),
                .track = (uint16_t)(style >> 8),
                .pos = (float)pos,
                .length = (float)(end - pos),
            });
        }
    }
    return result;
}

class Store {
public:
    static Store& Get() { static Store s; return s; }

    // Starts (or keeps) preparing every chunk of `song` for `params`. Cheap when nothing changed.
    void Configure(const std::shared_ptr<PackedSong>& song, const Params& params, long long playheadUs)
    {
        if (!song || params.timeSpan <= 0 || params.rows <= 0)
            return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_song == song && m_params == params) {
                m_playhead = playheadUs;
                return;
            }
        }
        StopWorkers();

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_song != song)
            m_index.reset(); // the index belongs to the song, not to the layout
        m_song = song;
        m_params = params;
        m_playhead = playheadUs;
        m_ready.clear();
        m_claimed.clear();
        m_doneCount = 0;
        m_bytes = 0;
        m_first = 0;
        m_last = -1;
        m_started = std::chrono::steady_clock::now();
        m_loggedComplete = false;
        ++m_generation;
        m_stop = false;
        if (m_index) {
            BeginChunksLocked();
            return;
        }
        // The song index is one decoding pass over the whole song: build it off the game thread,
        // then start preparing chunks.
        m_indexing = true;
        const unsigned long long generation = m_generation;
        try {
            m_workers.emplace_back([this, song, generation]() {
                const auto t0 = std::chrono::steady_clock::now();
                auto index = std::make_shared<SongIndex>();
                try { BuildSongIndex(*song, *index); } catch (...) { index.reset(); }
                char b[160];
                sprintf_s(b, "dense:index tracks=%zu notes=%llu ms=%.0f%s", song->refs.size(),
                    (unsigned long long)song->noteCount,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                    index ? "" : " FAILED");
                HeartbeatLog(b);
                std::lock_guard<std::mutex> lock(m_mutex);
                if (generation != m_generation || m_stop)
                    return;
                m_indexing = false;
                if (!index) {
                    m_song.reset(); // nothing to serve: callers fall back to the regular path
                    return;
                }
                m_index = std::move(index);
                BeginChunksLocked();
            });
        } catch (...) {
            m_indexing = false;
            m_song.reset();
        }
    }

    // Drops everything (song closed, settings changed). Blocks until workers stop.
    void Reset()
    {
        StopWorkers();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_song.reset();
        m_index.reset();
        m_params = {};
        m_ready.clear();
        m_claimed.clear();
        m_doneCount = 0;
        m_indexing = false;
        ++m_generation;
    }

    bool ActiveFor(const PackedSong* song, const Params& params) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_song.get() == song && song && m_params == params;
    }

    // Same song and chunk length, whatever the row count: the prepared notes are still at the right
    // times, only merged at a slightly different row resolution. Used while the layout animates.
    bool ServesSong(const PackedSong* song, long long timeSpan) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_song.get() == song && song && m_params.timeSpan == timeSpan;
    }

    bool HasSong() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return (bool)m_song;
    }

    bool HoldsSong(const PackedSong* song) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return song && m_song.get() == song;
    }

    // Prepared notes of a chunk, or null if it is not prepared yet. Chunks outside the song are
    // reported as ready and empty.
    std::shared_ptr<const std::vector<NoteData>> Ready(long long chunk) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_song || m_indexing) return {};
        if (chunk < m_first || chunk > m_last) return Empty();
        const auto it = m_ready.find(chunk);
        return it == m_ready.end() ? nullptr : it->second;
    }

    void SetPlayhead(long long us)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_playhead = us;
    }

    Progress GetProgress() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Progress p;
        p.active = (bool)m_song;
        p.total = !m_song ? 0 : m_indexing ? 1 : (size_t)(m_last - m_first + 1);
        p.done = m_doneCount;
        return p;
    }

    ~Store() { StopWorkers(); }

private:
    static long long FloorDiv(long long v, long long d)
    {
        long long q = v / d;
        if ((v % d) != 0 && ((v < 0) != (d < 0))) --q;
        return q;
    }
    static std::shared_ptr<const std::vector<NoteData>> Empty()
    {
        static const auto empty = std::make_shared<const std::vector<NoteData>>();
        return empty;
    }

    // Picks the unclaimed chunk nearest after the playhead (then the rest, then the past). Called
    // with the mutex held.
    bool ClaimLocked(long long& chunk)
    {
        const long long n = m_last - m_first + 1;
        if (n <= 0) return false;
        long long start = FloorDiv(m_playhead, m_params.timeSpan) - 1;
        start = (std::min)((std::max)(start, m_first), m_last);
        // Ahead of the playhead first: that is what playback needs next.
        for (long long k = start; k <= m_last; ++k) {
            if (!m_claimed[(size_t)(k - m_first)]) {
                m_claimed[(size_t)(k - m_first)] = 1;
                chunk = k;
                return true;
            }
        }
        for (long long k = m_first; k <= m_last; ++k) {
            if (!m_claimed[(size_t)(k - m_first)]) {
                m_claimed[(size_t)(k - m_first)] = 1;
                chunk = k;
                return true;
            }
        }
        return false;
    }

    void WorkerMain(unsigned long long generation)
    {
        std::vector<RawNote> raw;
        for (;;) {
            long long chunk = 0;
            std::shared_ptr<PackedSong> song;
            std::shared_ptr<SongIndex> index;
            Params params;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stop || generation != m_generation || !ClaimLocked(chunk))
                    return;
                song = m_song;
                index = m_index;
                params = m_params;
            }
            std::shared_ptr<const std::vector<NoteData>> notes;
            try {
                const long long t0 = chunk * params.timeSpan;
                CollectChunkNotes(*song, *index, t0, params.timeSpan, raw);
                auto compact = CompactChunk(raw, t0, params.timeSpan, params.rows);
                notes = std::make_shared<const std::vector<NoteData>>(std::move(compact));
                if (raw.capacity() > (size_t)16 << 20)
                    std::vector<RawNote>().swap(raw); // do not keep a huge scratch buffer around
            } catch (...) {
                notes = Empty(); // out of memory: draw nothing rather than stall playback
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            if (generation != m_generation)
                return;
            m_bytes += notes->size() * sizeof(NoteData);
            m_ready[chunk] = std::move(notes);
            ++m_doneCount;
            if (!m_loggedComplete && m_doneCount >= (size_t)(m_last - m_first + 1)) {
                m_loggedComplete = true;
                char b[160];
                sprintf_s(b, "dense:complete chunks=%zu memory=%.1fMB ms=%.0f", m_doneCount, m_bytes / 1048576.0,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_started).count());
                HeartbeatLog(b);
            }
        }
    }

    // The index is known: lay out the song's chunks and start the workers. Mutex held.
    void BeginChunksLocked()
    {
        const long long T = m_params.timeSpan;
        m_first = FloorDiv(m_index->firstNoteUs, T);
        m_last = FloorDiv((std::max)(m_index->firstNoteUs, m_index->lastNoteEndUs), T);
        m_ready.reserve((size_t)(m_last - m_first + 1));
        m_claimed.assign((size_t)(m_last - m_first + 1), 0);
        m_started = std::chrono::steady_clock::now();
        StartWorkersLocked();
        char b[160];
        sprintf_s(b, "dense:configure chunks=%lld..%lld T=%lld rows=%d", m_first, m_last, T, m_params.rows);
        HeartbeatLog(b);
    }

    void StartWorkersLocked()
    {
        m_stop = false;
        unsigned n = std::thread::hardware_concurrency();
        // Leave a core for the game and audio threads.
        n = (std::max)(2u, (std::min)(n > 2 ? n - 1 : 2u, 8u));
        const unsigned long long generation = m_generation;
        for (unsigned i = 0; i < n; ++i) {
            try { m_workers.emplace_back([this, generation]() { WorkerMain(generation); }); }
            catch (...) { break; }
        }
    }

    void StopWorkers()
    {
        std::vector<std::thread> workers;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
            ++m_generation;
            workers.swap(m_workers);
        }
        for (auto& t : workers)
            if (t.joinable()) t.join();
    }

    mutable std::mutex m_mutex;
    std::vector<std::thread> m_workers;
    bool m_stop = false;
    unsigned long long m_generation = 0;
    std::shared_ptr<PackedSong> m_song;
    std::shared_ptr<SongIndex> m_index;
    Params m_params;
    long long m_playhead = 0;
    long long m_first = 0, m_last = -1;
    std::vector<uint8_t> m_claimed;
    std::unordered_map<long long, std::shared_ptr<const std::vector<NoteData>>> m_ready;
    size_t m_doneCount = 0;
    size_t m_bytes = 0;
    bool m_loggedComplete = false;
    bool m_indexing = false;
    std::chrono::steady_clock::time_point m_started;
};

} // namespace PGFADense
