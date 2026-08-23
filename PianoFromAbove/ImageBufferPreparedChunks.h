#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ImageBufferOverlapIndex.h"

class Renderer;

static constexpr size_t ImageBufferPreparedDenseThreshold = 100000;
static constexpr int ImageBufferPreparedPreloadRadius = 10;
static constexpr int ImageBufferPreparedMaxRows = 1536; // 128 * 1536 = 196608 compact rectangles max
static constexpr size_t ImageBufferPreparedEntryLimit = 128;

struct ImageBufferPreparedParams {
    long long chunk = 0;
    long long timeSpan = 0;
    long long corruptionMargin = 0;
    float corruption = 0.0f;
    size_t trackCount = 0;
    int rows = 0;
    bool tickMode = false;
};

struct ImageBufferPreparedResult {
    std::vector<NoteData> notes;
    size_t rawVisited = 0;
    size_t cellsWritten = 0;
};

inline uint64_t ImageBufferPreparedMix(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

inline uint64_t ImageBufferPreparedSignature(
    const ImageBufferPreparedSource* source,
    long long timeSpan,
    bool tickMode,
    float corruption,
    size_t trackCount,
    int rows)
{
    uint32_t corruptionBits = 0;
    std::memcpy(&corruptionBits, &corruption, sizeof(corruptionBits));
    uint64_t h = ImageBufferPreparedMix((uint64_t)(uintptr_t)source);
    h ^= ImageBufferPreparedMix((uint64_t)timeSpan + 0x9e3779b97f4a7c15ull);
    h ^= ImageBufferPreparedMix((uint64_t)corruptionBits << 1);
    h ^= ImageBufferPreparedMix((uint64_t)trackCount << 17);
    h ^= ImageBufferPreparedMix((uint64_t)(unsigned)rows << 33);
    h ^= tickMode ? 0x6a09e667f3bcc909ull : 0xbb67ae8584caa73bull;
    return h;
}

inline void ImageBufferPreparedCorruptNote(
    float fCorrupt,
    uint64_t h,
    int& note,
    int& track,
    int& channel,
    long long& start,
    long long& length,
    long long timeSpan,
    size_t trackCount)
{
    if (fCorrupt <= 0.0f)
        return;

    const float scale = 1.0f / 1048576.0f;
    const float rPitch = (float)(h & 0xfffff) * scale;
    const float rTime = (float)((h >> 20) & 0xfffff) * scale;
    const float rLen = (float)((h >> 40) & 0xfffff) * scale;
    const uint64_t h2 = h * 0x9e3779b97f4a7c15ull;
    const float rCol = (float)(h2 & 0xfffff) * scale;

    note += (int)llroundf((rPitch * 2.0f - 1.0f) * 60.0f * fCorrupt);
    note = (std::min)((std::max)(note, 0), 127);

    start += (long long)((double)(rTime * 2.0f - 1.0f) * (double)timeSpan * 0.10 * (double)fCorrupt);
    length = (long long)((double)length * (1.0 + (double)(rLen * 2.0f - 1.0f) * 0.5 * (double)fCorrupt));

    if (trackCount > 0) {
        track += (int)llroundf((rCol * 2.0f - 1.0f) * (float)trackCount * 0.5f * fCorrupt);
        track = (std::min)((std::max)(track, 0), (int)trackCount - 1);
    }
    channel += (int)llroundf((rCol * 2.0f - 1.0f) * 16.0f * fCorrupt);
    channel = (std::min)((std::max)(channel, 0), 15);
}

inline size_t ImageBufferPreparedEstimateStarts(
    const ImageBufferPreparedSource& source,
    long long chunk,
    long long timeSpan,
    long long margin,
    bool tickMode)
{
    if (source.notes.empty() || timeSpan <= 0)
        return 0;

    const long long chunkStart = chunk * timeSpan;
    const long long loValue = ImageBufferOverlapSaturatingAdd(chunkStart, -margin);
    const long long hiValue = ImageBufferOverlapSaturatingAdd(
        ImageBufferOverlapSaturatingAdd(chunkStart, timeSpan), margin);

    auto startValue = [&](const ImageBufferPreparedRawNote& n) -> long long {
        return tickMode ? (long long)n.startTick : (long long)n.start100us * 100LL;
    };

    auto lo = std::lower_bound(source.notes.begin(), source.notes.end(), loValue,
        [&](const ImageBufferPreparedRawNote& n, long long value) { return startValue(n) < value; });
    auto hi = std::lower_bound(source.notes.begin(), source.notes.end(), hiValue,
        [&](const ImageBufferPreparedRawNote& n, long long value) { return startValue(n) < value; });
    return (size_t)(hi - lo);
}

inline ImageBufferPreparedResult ImageBufferPreparedBuild(
    const ImageBufferPreparedSource& source,
    const ImageBufferPreparedParams& params)
{
    ImageBufferPreparedResult result;
    if (source.notes.empty() || params.timeSpan <= 0 || params.rows <= 0)
        return result;

    const int rows = (std::min)((std::max)(params.rows, 1), ImageBufferPreparedMaxRows);
    const long long chunkStart = params.chunk * params.timeSpan;
    const long long chunkEnd = ImageBufferOverlapSaturatingAdd(chunkStart, params.timeSpan);
    const long long hiValue = ImageBufferOverlapSaturatingAdd(chunkEnd, params.corruptionMargin);
    const long long oldestUsefulEnd = ImageBufferOverlapSaturatingAdd(chunkStart, -params.corruptionMargin);

    auto startValue = [&](const ImageBufferPreparedRawNote& n) -> long long {
        return params.tickMode ? (long long)n.startTick : (long long)n.start100us * 100LL;
    };

    auto itHi = std::lower_bound(source.notes.begin(), source.notes.end(), hiValue,
        [&](const ImageBufferPreparedRawNote& n, long long value) { return startValue(n) < value; });
    const size_t hi = (size_t)(itHi - source.notes.begin());
    if (hi == 0)
        return result;

    // One ownership cell per key and vertical sample. Newest notes are visited
    // first; once a cell is claimed, older same-key notes can never change it.
    // The disjoint-set skips already-filled cells, so huge stacks of repeated
    // notes do not turn into rows * notes work.
    const uint32_t emptyStyle = 0xffffffffu;
    std::vector<uint32_t> cells((size_t)128 * rows, emptyStyle);
    std::vector<int> next((size_t)128 * (rows + 1));
    for (int key = 0; key < 128; ++key) {
        const size_t base = (size_t)key * (rows + 1);
        for (int r = 0; r <= rows; ++r)
            next[base + r] = r;
    }

    auto findNext = [&](int key, int row) {
        const size_t base = (size_t)key * (rows + 1);
        int root = row;
        while (next[base + root] != root)
            root = next[base + root];
        while (next[base + row] != row) {
            int old = row;
            row = next[base + row];
            next[base + old] = root;
        }
        return root;
    };

    size_t remainingCells = (size_t)128 * rows;
    size_t block = (hi - 1) / ImageBufferPreparedRawBlockNotes;
    for (;;) {
        const uint64_t blockMax = params.tickMode
            ? source.maxEndTick150[block]
            : source.maxEndTime150_100us[block] * 100ULL;

        if (blockMax >= (oldestUsefulEnd < 0 ? 0ULL : (uint64_t)oldestUsefulEnd)) {
            const size_t begin = block * ImageBufferPreparedRawBlockNotes;
            const size_t end = (std::min)(hi, begin + ImageBufferPreparedRawBlockNotes);
            for (size_t i = end; i != begin; ) {
                --i;
                const ImageBufferPreparedRawNote& raw = source.notes[i];
                ++result.rawVisited;

                int note = raw.key;
                int track = raw.track;
                int channel = raw.channel;
                long long start = params.tickMode
                    ? (long long)raw.startTick
                    : (long long)raw.start100us * 100LL;
                long long length = params.tickMode
                    ? (long long)raw.lengthTick
                    : (long long)raw.length100us * 100LL;

                ImageBufferPreparedCorruptNote(params.corruption, raw.seed,
                    note, track, channel, start, length,
                    params.timeSpan, params.trackCount);

                const long long relativeStart = start - chunkStart;
                const long long nonNegativeLength = (std::max)(length, 0LL);
                const long long relativeEnd = ImageBufferOverlapSaturatingAdd(relativeStart, nonNegativeLength);
                if (relativeStart >= params.timeSpan || relativeEnd < 0)
                    continue;

                const double clippedStart = (std::max)(0.0, (double)relativeStart);
                const double clippedEnd = (std::min)((double)params.timeSpan, (double)relativeEnd);
                int r0 = (int)std::floor(clippedStart * rows / (double)params.timeSpan);
                int r1 = (int)std::ceil(clippedEnd * rows / (double)params.timeSpan);
                r0 = (std::min)((std::max)(r0, 0), rows - 1);
                r1 = (std::min)((std::max)(r1, r0 + 1), rows);

                const uint32_t style = ((uint32_t)(uint16_t)track << 8) | (uint32_t)(uint8_t)channel;
                int r = findNext(note, r0);
                while (r < r1) {
                    const size_t cell = (size_t)note * rows + r;
                    cells[cell] = style;
                    ++result.cellsWritten;
                    --remainingCells;
                    const size_t base = (size_t)note * (rows + 1);
                    next[base + r] = findNext(note, r + 1);
                    r = next[base + r];
                }

                if (remainingCells == 0)
                    break;
            }
        }

        if (remainingCells == 0 || block == 0)
            break;
        --block;
    }

    result.notes.reserve((std::min)((size_t)128 * rows, result.cellsWritten));
    for (int key = 0; key < 128; ++key) {
        const size_t base = (size_t)key * rows;
        int r = 0;
        while (r < rows) {
            const uint32_t style = cells[base + r];
            if (style == emptyStyle) {
                ++r;
                continue;
            }
            const int begin = r;
            while (r < rows && cells[base + r] == style)
                ++r;
            const double pos = (double)begin * (double)params.timeSpan / (double)rows;
            const double end = (double)r * (double)params.timeSpan / (double)rows;
            result.notes.push_back(NoteData{
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

struct ImageBufferPreparedKey {
    const ImageBufferPreparedSource* source = nullptr;
    long long chunk = 0;
    uint64_t signature = 0;

    bool operator==(const ImageBufferPreparedKey& other) const {
        return source == other.source && chunk == other.chunk && signature == other.signature;
    }
};

struct ImageBufferPreparedKeyHash {
    size_t operator()(const ImageBufferPreparedKey& k) const {
        uint64_t h = ImageBufferPreparedMix((uint64_t)(uintptr_t)k.source);
        h ^= ImageBufferPreparedMix((uint64_t)k.chunk);
        h ^= ImageBufferPreparedMix(k.signature);
        return (size_t)h;
    }
};

class ImageBufferPreparedManager {
public:
    enum class State { Queued, Preparing, Ready, Failed };

    struct Entry {
        State state = State::Queued;
        std::shared_ptr<const ImageBufferPreparedSource> source;
        std::shared_ptr<const std::vector<NoteData>> ready;
        ImageBufferPreparedParams params;
        size_t estimate = 0;
        size_t rawVisited = 0;
        uint64_t lastUse = 0;
    };

    struct Job {
        ImageBufferPreparedKey key;
        std::shared_ptr<const ImageBufferPreparedSource> source;
        ImageBufferPreparedParams params;
        int priority = 0;
        uint64_t serial = 0;
    };

    struct JobLater {
        bool operator()(const Job& a, const Job& b) const {
            if (a.priority != b.priority)
                return a.priority > b.priority;
            return a.serial > b.serial;
        }
    };

    ImageBufferPreparedManager()
    {
        unsigned hc = std::thread::hardware_concurrency();
        unsigned workers = hc > 4 ? 4u : (std::max)(2u, hc == 0 ? 2u : hc);
        for (unsigned i = 0; i < workers; ++i)
            m_workers.emplace_back([this]() { WorkerMain(); });
    }

    ~ImageBufferPreparedManager()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        for (std::thread& worker : m_workers)
            if (worker.joinable())
                worker.join();
    }

    bool Has(const ImageBufferPreparedKey& key)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_entries.find(key) != m_entries.end();
    }

    std::shared_ptr<const std::vector<NoteData>> Ready(const ImageBufferPreparedKey& key)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_entries.find(key);
        if (it == m_entries.end() || it->second.state != State::Ready)
            return {};
        it->second.lastUse = ++m_useSerial;
        return it->second.ready;
    }

    void Schedule(const ImageBufferPreparedKey& key,
                  const std::shared_ptr<const ImageBufferPreparedSource>& source,
                  const ImageBufferPreparedParams& params,
                  int priority,
                  size_t estimate)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_entries.find(key);
        if (it != m_entries.end()) {
            it->second.lastUse = ++m_useSerial;
            return;
        }

        Entry entry;
        entry.state = State::Queued;
        entry.source = source;
        entry.params = params;
        entry.estimate = estimate;
        entry.lastUse = ++m_useSerial;
        m_entries.emplace(key, std::move(entry));
        m_jobs.push(Job{ key, source, params, priority, ++m_jobSerial });
        EvictReadyLocked();
        m_cv.notify_one();
    }

    bool BeginPrime(const ImageBufferPreparedSource* source, uint64_t signature,
                    long long first, long long last)
    {
        const uint64_t key = ImageBufferPreparedMix((uint64_t)(uintptr_t)source) ^ signature;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& stamp = m_prime[key];
        if (stamp.first == first && stamp.second == last)
            return false;
        stamp = { first, last };
        return true;
    }

    void MarkPending(Renderer* renderer, long long chunk)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingRender.insert(PendingKey(renderer, chunk));
    }

    void ClearPending(Renderer* renderer, long long chunk)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingRender.erase(PendingKey(renderer, chunk));
    }

    bool IsPending(Renderer* renderer, long long chunk)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pendingRender.find(PendingKey(renderer, chunk)) != m_pendingRender.end();
    }

private:
    static uint64_t PendingKey(Renderer* renderer, long long chunk)
    {
        return ImageBufferPreparedMix((uint64_t)(uintptr_t)renderer) ^
               ImageBufferPreparedMix((uint64_t)chunk + 0x517cc1b727220a95ull);
    }

    void EvictReadyLocked()
    {
        while (m_entries.size() > ImageBufferPreparedEntryLimit) {
            auto victim = m_entries.end();
            for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
                if (it->second.state != State::Ready && it->second.state != State::Failed)
                    continue;
                if (victim == m_entries.end() || it->second.lastUse < victim->second.lastUse)
                    victim = it;
            }
            if (victim == m_entries.end())
                break;
            m_entries.erase(victim);
        }
    }

    void WorkerMain()
    {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [&]() { return m_stop || !m_jobs.empty(); });
                if (m_stop)
                    return;
                job = m_jobs.top();
                m_jobs.pop();
                auto it = m_entries.find(job.key);
                if (it == m_entries.end() || it->second.state != State::Queued)
                    continue;
                it->second.state = State::Preparing;
            }

            const auto begin = std::chrono::steady_clock::now();
            ImageBufferPreparedResult built;
            bool ok = true;
            try {
                built = ImageBufferPreparedBuild(*job.source, job.params);
            } catch (...) {
                ok = false;
            }
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - begin).count();

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_entries.find(job.key);
                if (it == m_entries.end())
                    continue;
                if (!ok) {
                    it->second.state = State::Failed;
                } else {
                    it->second.ready = std::make_shared<const std::vector<NoteData>>(std::move(built.notes));
                    it->second.rawVisited = built.rawVisited;
                    it->second.state = State::Ready;
                    it->second.lastUse = ++m_useSerial;
                }
                EvictReadyLocked();
            }

            char log[192];
            sprintf_s(log, "imgprep:done chunk=%lld visited=%zu compact=%zu ms=%.1f",
                job.params.chunk, built.rawVisited,
                ok ? (size_t)(ReadySize(job.key)) : 0, ms);
            HeartbeatLog(log);
        }
    }

    size_t ReadySize(const ImageBufferPreparedKey& key)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_entries.find(key);
        if (it == m_entries.end() || !it->second.ready)
            return 0;
        return it->second.ready->size();
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
    uint64_t m_jobSerial = 0;
    uint64_t m_useSerial = 0;
    std::vector<std::thread> m_workers;
    std::priority_queue<Job, std::vector<Job>, JobLater> m_jobs;
    std::unordered_map<ImageBufferPreparedKey, Entry, ImageBufferPreparedKeyHash> m_entries;
    std::unordered_map<uint64_t, std::pair<long long, long long>> m_prime;
    std::unordered_set<uint64_t> m_pendingRender;
};

inline ImageBufferPreparedManager& ImageBufferPreparedGet()
{
    static ImageBufferPreparedManager manager;
    return manager;
}

inline void ImageBufferPreparedPrime(
    Renderer* renderer,
    const std::shared_ptr<const ImageBufferPreparedSource>& source,
    long long first,
    long long last,
    long long timeSpan,
    long long margin,
    bool tickMode,
    float corruption,
    size_t trackCount,
    int rows,
    uint64_t signature)
{
    if (!source || first > last)
        return;
    auto& manager = ImageBufferPreparedGet();
    if (!manager.BeginPrime(source.get(), signature, first, last))
        return;

    std::vector<std::pair<long long, size_t>> dense;
    dense.reserve((size_t)(last - first + 1));
    for (long long k = first; k <= last; ++k) {
        const size_t estimate = ImageBufferPreparedEstimateStarts(*source, k, timeSpan, margin, tickMode);
        if (estimate >= ImageBufferPreparedDenseThreshold)
            dense.push_back({ k, estimate });
    }

    std::unordered_set<long long> scheduled;
    for (const auto& center : dense) {
        const long long lo = (std::max)(first, center.first - ImageBufferPreparedPreloadRadius);
        const long long hi = (std::min)(last, center.first + ImageBufferPreparedPreloadRadius);
        for (long long k = lo; k <= hi; ++k) {
            if (!scheduled.insert(k).second)
                continue;
            ImageBufferPreparedParams params;
            params.chunk = k;
            params.timeSpan = timeSpan;
            params.corruptionMargin = margin;
            params.corruption = corruption;
            params.trackCount = trackCount;
            params.rows = rows;
            params.tickMode = tickMode;
            const ImageBufferPreparedKey key{ source.get(), k, signature };
            const size_t estimate = ImageBufferPreparedEstimateStarts(*source, k, timeSpan, margin, tickMode);
            const int priority = 100 + (int)(std::max)(0LL, k - first);
            manager.Schedule(key, source, params, priority, estimate);
        }
    }

    if (!dense.empty()) {
        char log[160];
        sprintf_s(log, "imgprep:prime dense=%zu jobs=%zu horizon=%lld..%lld",
            dense.size(), scheduled.size(), first, last);
        HeartbeatLog(log);
    }
}

// Returns true when the prepared path owns this collection request. `out` is
// either the ready compact geometry or empty while a worker is still building.
// Returns false for ordinary sparse chunks so the caller can use the exact
// synchronous collector unchanged.
template <typename MidiT>
inline bool ImageBufferPreparedTryCollect(
    const void* owner,
    Renderer* renderer,
    const std::vector<MIDIChannelEvent>& events,
    MidiT& midi,
    std::vector<NoteData>& out,
    long long chunk,
    long long first,
    long long last,
    long long preloadLast,
    long long timeSpan,
    long long margin,
    bool tickMode,
    float corruption,
    size_t trackCount,
    int rows)
{
    auto& overlap = ImageBufferOverlapEnsureIndex(owner, events, midi);
    const auto source = overlap.preparedSource;
    if (!source || source->notes.empty() || corruption > 1.0f ||
        (tickMode ? source->tickOverflow : source->timeOverflow)) {
        ImageBufferPreparedGet().ClearPending(renderer, chunk);
        return false;
    }

    rows = (std::min)((std::max)(rows, 64), ImageBufferPreparedMaxRows);
    const uint64_t signature = ImageBufferPreparedSignature(
        source.get(), timeSpan, tickMode, corruption, trackCount, rows);

    ImageBufferPreparedPrime(renderer, source, first, preloadLast,
        timeSpan, margin, tickMode, corruption, trackCount, rows, signature);

    const ImageBufferPreparedKey key{ source.get(), chunk, signature };
    auto& manager = ImageBufferPreparedGet();
    const size_t estimate = ImageBufferPreparedEstimateStarts(*source, chunk, timeSpan, margin, tickMode);
    const bool prepared = manager.Has(key) || estimate >= ImageBufferPreparedDenseThreshold;
    if (!prepared) {
        manager.ClearPending(renderer, chunk);
        return false;
    }

    if (auto ready = manager.Ready(key)) {
        out.assign(ready->begin(), ready->end());
        manager.ClearPending(renderer, chunk);
        return true;
    }

    ImageBufferPreparedParams params;
    params.chunk = chunk;
    params.timeSpan = timeSpan;
    params.corruptionMargin = margin;
    params.corruption = corruption;
    params.trackCount = trackCount;
    params.rows = rows;
    params.tickMode = tickMode;
    const int priority = (chunk >= first && chunk <= last) ? 0 : 50 + (int)(std::max)(0LL, chunk - first);
    manager.Schedule(key, source, params, priority, estimate);
    out.clear();
    manager.MarkPending(renderer, chunk);
    return true;
}

inline bool ImageBufferPreparedRenderPending(Renderer* renderer, long long chunk)
{
    return ImageBufferPreparedGet().IsPending(renderer, chunk);
}
