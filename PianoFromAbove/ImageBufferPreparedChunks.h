#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "MIDI.h"
#include "ImageBufferOverlapIndex.h"

class Renderer;

static constexpr size_t ImageBufferPreparedDenseThreshold = 15000;
static constexpr int ImageBufferPreparedPreloadRadius = 10;
static constexpr int ImageBufferPreparedMaxRows = 1536;
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

struct ImageBufferPreparedProgress {
    size_t done = 0;
    size_t total = 0;
    size_t failed = 0;
    bool initialized = false;
    bool unsupported = false;

    bool Complete() const {
        return initialized && (unsupported || done >= total);
    }
};

inline std::atomic<bool>& ImageBufferPreparedWaitOptionStorage()
{
    static std::atomic<bool> enabled{ false };
    return enabled;
}

inline bool ImageBufferPreparedGetWaitBeforePlayback()
{
    return ImageBufferPreparedWaitOptionStorage().load(std::memory_order_relaxed);
}

inline void ImageBufferPreparedSetWaitBeforePlayback(bool enabled)
{
    ImageBufferPreparedWaitOptionStorage().store(enabled, std::memory_order_relaxed);
}

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

    start += (long long)((double)(rTime * 2.0f - 1.0f) *
        (double)timeSpan * 0.10 * (double)fCorrupt);
    length = (long long)((double)length *
        (1.0 + (double)(rLen * 2.0f - 1.0f) * 0.5 * (double)fCorrupt));

    if (trackCount > 0) {
        track += (int)llroundf((rCol * 2.0f - 1.0f) *
            (float)trackCount * 0.5f * fCorrupt);
        track = (std::min)((std::max)(track, 0), (int)trackCount - 1);
    }
    channel += (int)llroundf((rCol * 2.0f - 1.0f) * 16.0f * fCorrupt);
    channel = (std::min)((std::max)(channel, 0), 15);
}

inline long long ImageBufferPreparedStartValue(
    const ImageBufferPreparedRawNote& note, bool tickMode)
{
    return tickMode ? (long long)note.startTick : (long long)note.start100us * 100LL;
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
    const long long chunkEnd = ImageBufferOverlapSaturatingAdd(chunkStart, timeSpan);
    const long long hiValue = ImageBufferOverlapSaturatingAdd(chunkEnd, margin);
    const long long oldestUsefulEnd = ImageBufferOverlapSaturatingAdd(chunkStart, -margin);
    const uint64_t oldest = oldestUsefulEnd < 0 ? 0ULL : (uint64_t)oldestUsefulEnd;

    auto itHi = std::lower_bound(source.notes.begin(), source.notes.end(), hiValue,
        [&](const ImageBufferPreparedRawNote& n, long long value) {
            return ImageBufferPreparedStartValue(n, tickMode) < value;
        });
    const size_t hi = (size_t)(itHi - source.notes.begin());
    if (hi == 0)
        return 0;

    if (source.maxEndTime150_100us.empty() || source.prefixMaxEndTime150_100us.empty())
        return hi;

    size_t candidateCount = 0;
    size_t block = (hi - 1) / ImageBufferPreparedRawBlockNotes;
    for (;;) {
        const uint64_t prefixMax = tickMode
            ? source.prefixMaxEndTick150[block]
            : source.prefixMaxEndTime150_100us[block] * 100ULL;
        if (prefixMax < oldest)
            break;

        const uint64_t blockMax = tickMode
            ? source.maxEndTick150[block]
            : source.maxEndTime150_100us[block] * 100ULL;
        if (blockMax >= oldest) {
            const size_t begin = block * ImageBufferPreparedRawBlockNotes;
            const size_t end = (std::min)(hi, begin + ImageBufferPreparedRawBlockNotes);
            size_t subBlock = (end - 1) / ImageBufferPreparedRawSubBlockNotes;
            const size_t firstSubBlock = begin / ImageBufferPreparedRawSubBlockNotes;
            for (;;) {
                const uint64_t subMax = tickMode
                    ? source.subMaxEndTick150[subBlock]
                    : source.subMaxEndTime150_100us[subBlock] * 100ULL;
                if (subMax >= oldest) {
                    const size_t subBegin = (std::max)(begin,
                        subBlock * ImageBufferPreparedRawSubBlockNotes);
                    const size_t subEnd = (std::min)(end,
                        (subBlock + 1) * ImageBufferPreparedRawSubBlockNotes);
                    candidateCount += (subEnd - subBegin);
                }
                if (subBlock == firstSubBlock)
                    break;
                --subBlock;
            }
        }

        if (block == 0)
            break;
        --block;
    }
    return candidateCount;
}

inline long long ImageBufferPreparedFloorDiv(long long value, long long divisor)
{
    if (divisor <= 0)
        return 0;
    long long q = value / divisor;
    const long long r = value % divisor;
    if (r < 0)
        --q;
    return q;
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
    const long long oldestUsefulEnd = ImageBufferOverlapSaturatingAdd(
        chunkStart, -params.corruptionMargin);

    auto itHi = std::lower_bound(source.notes.begin(), source.notes.end(), hiValue,
        [&](const ImageBufferPreparedRawNote& n, long long value) {
            return ImageBufferPreparedStartValue(n, params.tickMode) < value;
        });
    const size_t hi = (size_t)(itHi - source.notes.begin());
    if (hi == 0)
        return result;

    // Keep source-note ownership alongside style. Without the owner id, adjacent
    // repeated notes of the same color merge into one flat bar during compaction.
    const uint32_t emptyStyle = 0xffffffffu;
    const uint32_t emptyOwner = 0xffffffffu;
    std::vector<uint32_t> cells((size_t)128 * rows, emptyStyle);
    std::vector<uint32_t> owners((size_t)128 * rows, emptyOwner);
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

    size_t remainingCells = (size_t)128 * rows;
    size_t block = (hi - 1) / ImageBufferPreparedRawBlockNotes;
    for (;;) {
        const uint64_t blockMax = params.tickMode
            ? source.maxEndTick150[block]
            : source.maxEndTime150_100us[block] * 100ULL;
        const uint64_t oldest = oldestUsefulEnd < 0 ? 0ULL : (uint64_t)oldestUsefulEnd;
        const uint64_t prefixMax = params.tickMode
            ? source.prefixMaxEndTick150[block]
            : source.prefixMaxEndTime150_100us[block] * 100ULL;
        if (prefixMax < oldest)
            break;

        if (blockMax >= oldest) {
            const size_t begin = block * ImageBufferPreparedRawBlockNotes;
            const size_t end = (std::min)(hi, begin + ImageBufferPreparedRawBlockNotes);
            size_t subBlock = (end - 1) / ImageBufferPreparedRawSubBlockNotes;
            const size_t firstSubBlock = begin / ImageBufferPreparedRawSubBlockNotes;
            for (;;) {
                const uint64_t subMax = params.tickMode
                    ? source.subMaxEndTick150[subBlock]
                    : source.subMaxEndTime150_100us[subBlock] * 100ULL;
                if (subMax >= oldest) {
                    const size_t subBegin = (std::max)(begin,
                        subBlock * ImageBufferPreparedRawSubBlockNotes);
                    const size_t subEnd = (std::min)(end,
                        (subBlock + 1) * ImageBufferPreparedRawSubBlockNotes);
                    for (size_t i = subEnd; i != subBegin; ) {
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
                const long long relativeEnd = ImageBufferOverlapSaturatingAdd(
                    relativeStart, (std::max)(length, 0LL));
                if (relativeStart >= params.timeSpan || relativeEnd < 0)
                    continue;

                const double clippedStart = (std::max)(0.0, (double)relativeStart);
                const double clippedEnd = (std::min)((double)params.timeSpan, (double)relativeEnd);
                int row0 = (int)std::floor(clippedStart * rows / (double)params.timeSpan);
                int row1 = (int)std::ceil(clippedEnd * rows / (double)params.timeSpan);
                row0 = (std::min)((std::max)(row0, 0), rows - 1);
                row1 = (std::min)((std::max)(row1, row0 + 1), rows);

                const uint32_t style = ((uint32_t)(uint16_t)track << 8) |
                    (uint32_t)(uint8_t)channel;
                const uint32_t ownerId = (uint32_t)i;
                int row = findNext(note, row0);
                while (row < row1) {
                    const size_t cell = (size_t)note * rows + row;
                    cells[cell] = style;
                    owners[cell] = ownerId;
                    ++result.cellsWritten;
                    --remainingCells;
                    const size_t base = (size_t)note * (rows + 1);
                    next[base + row] = findNext(note, row + 1);
                    row = next[base + row];
                }

                        if (remainingCells == 0)
                            break;
                    }
                }
                if (remainingCells == 0 || subBlock == firstSubBlock)
                    break;
                --subBlock;
            }
        }

        if (remainingCells == 0 || block == 0)
            break;
        --block;
    }

    result.notes.reserve((std::min)((size_t)128 * rows, result.cellsWritten));
    for (int key = 0; key < 128; ++key) {
        const size_t base = (size_t)key * rows;
        int row = 0;
        while (row < rows) {
            const uint32_t style = cells[base + row];
            if (style == emptyStyle) {
                ++row;
                continue;
            }

            const uint32_t ownerId = owners[base + row];
            const int begin = row;
            while (row < rows &&
                   cells[base + row] == style &&
                   owners[base + row] == ownerId)
                ++row;

            const double pos = (double)begin * (double)params.timeSpan / (double)rows;
            const double end = (double)row * (double)params.timeSpan / (double)rows;
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
    size_t operator()(const ImageBufferPreparedKey& key) const {
        uint64_t h = ImageBufferPreparedMix((uint64_t)(uintptr_t)key.source);
        h ^= ImageBufferPreparedMix((uint64_t)key.chunk);
        h ^= ImageBufferPreparedMix(key.signature);
        return (size_t)h;
    }
};

class ImageBufferPreparedManager {
public:
    enum class State { Missing, Queued, Preparing, Ready, Failed };

    struct Entry {
        State state = State::Queued;
        std::shared_ptr<const ImageBufferPreparedSource> source;
        std::shared_ptr<const std::vector<NoteData>> ready;
        ImageBufferPreparedParams params;
        size_t estimate = 0;
        size_t rawVisited = 0;
        int priority = 1000;
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
        const unsigned hc = std::thread::hardware_concurrency();
        const unsigned workers = hc > 4 ? 4u : (std::max)(2u, hc == 0 ? 2u : hc);
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

    void Activate(const ImageBufferPreparedSource* source, uint64_t signature)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_activeSource == source && m_activeSignature == signature)
            return;

        m_activeSource = source;
        m_activeSignature = signature;
        for (auto it = m_entries.begin(); it != m_entries.end(); ) {
            if (it->first.source != source || it->first.signature != signature)
                it = m_entries.erase(it);
            else
                ++it;
        }

        std::priority_queue<Job, std::vector<Job>, JobLater> kept;
        while (!m_jobs.empty()) {
            Job job = m_jobs.top();
            m_jobs.pop();
            if (job.key.source == source && job.key.signature == signature)
                kept.push(std::move(job));
        }
        m_jobs.swap(kept);
        m_prime.clear();
        m_pendingRender.clear();
        m_fullOwner = nullptr;
        m_fullSource = nullptr;
        m_fullSignature = 0;
        m_fullInitialized = false;
        m_fullUnsupported = false;
        m_fullKeys.clear();
        m_pinned.clear();
    }

    State StateOf(const ImageBufferPreparedKey& key)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_entries.find(key);
        return it == m_entries.end() ? State::Missing : it->second.state;
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
                  size_t estimate,
                  bool pinForFullPrime = false)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (pinForFullPrime && m_pinned.insert(key).second)
            m_fullKeys.push_back(key);

        auto it = m_entries.find(key);
        if (it != m_entries.end()) {
            it->second.lastUse = ++m_useSerial;
            if (it->second.state == State::Queued && priority < it->second.priority) {
                it->second.priority = priority;
                m_jobs.push(Job{ key, source, params, priority, ++m_jobSerial });
                m_cv.notify_one();
            }
            return;
        }

        Entry entry;
        entry.state = State::Queued;
        entry.source = source;
        entry.params = params;
        entry.estimate = estimate;
        entry.priority = priority;
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
        const auto it = m_prime.find(key);
        if (it != m_prime.end() && it->second.first == first && it->second.second == last)
            return false;
        m_prime[key] = { first, last };
        return true;
    }

    bool BeginFullPrime(const void* owner,
                        const ImageBufferPreparedSource* source,
                        uint64_t signature)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_fullInitialized &&
            m_fullOwner == owner &&
            m_fullSource == source &&
            m_fullSignature == signature)
            return false;

        m_fullOwner = owner;
        m_fullSource = source;
        m_fullSignature = signature;
        m_fullInitialized = true;
        m_fullUnsupported = false;
        m_fullKeys.clear();
        m_pinned.clear();
        return true;
    }

    void MarkFullPrimeUnsupported(const void* owner)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_fullOwner = owner;
        m_fullSource = nullptr;
        m_fullSignature = 0;
        m_fullInitialized = true;
        m_fullUnsupported = true;
        m_fullKeys.clear();
        m_pinned.clear();
    }

    ImageBufferPreparedProgress FullProgress() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ImageBufferPreparedProgress progress;
        progress.initialized = m_fullInitialized;
        progress.unsupported = m_fullUnsupported;
        if (!m_fullInitialized || m_fullUnsupported)
            return progress;

        progress.total = m_fullKeys.size();
        for (const auto& key : m_fullKeys) {
            const auto it = m_entries.find(key);
            if (it == m_entries.end())
                continue;
            if (it->second.state == State::Ready) {
                ++progress.done;
            } else if (it->second.state == State::Failed) {
                ++progress.done;
                ++progress.failed;
            }
        }
        return progress;
    }

    void ArmPlaybackGate(const void* owner)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_playbackGateArmed = true;
        m_playbackGateOwner = owner;
    }

    void CancelPlaybackGate()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_playbackGateArmed = false;
        m_playbackGateOwner = nullptr;
    }

    bool ShouldHoldPlayback(const void* owner)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_playbackGateArmed)
            return false;
        if (!ImageBufferPreparedGetWaitBeforePlayback()) {
            m_playbackGateArmed = false;
            m_playbackGateOwner = nullptr;
            return false;
        }

        if (!m_playbackGateOwner)
            m_playbackGateOwner = owner;
        if (m_playbackGateOwner != owner)
            return false;

        if (!m_fullInitialized || m_fullOwner != owner)
            return true;

        if (m_fullUnsupported) {
            m_playbackGateArmed = false;
            m_playbackGateOwner = nullptr;
            return false;
        }

        for (const auto& key : m_fullKeys) {
            const auto it = m_entries.find(key);
            if (it == m_entries.end() ||
                (it->second.state != State::Ready && it->second.state != State::Failed))
                return true;
        }

        m_playbackGateArmed = false;
        m_playbackGateOwner = nullptr;
        return false;
    }

    bool PlaybackGateArmedFor(const void* owner) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_playbackGateArmed &&
            (!m_playbackGateOwner || m_playbackGateOwner == owner);
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
                if (m_pinned.find(it->first) != m_pinned.end())
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
                if (it == m_entries.end() || it->second.state != State::Queued ||
                    it->second.priority != job.priority)
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

            std::shared_ptr<const std::vector<NoteData>> published;
            if (ok) {
                try {
                    published = std::make_shared<const std::vector<NoteData>>(
                        std::move(built.notes));
                } catch (...) {
                    ok = false;
                }
            }

            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - begin).count();

            size_t compact = 0;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_entries.find(job.key);
                if (it == m_entries.end())
                    continue;

                if (!ok) {
                    it->second.ready.reset();
                    it->second.state = State::Failed;
                } else {
                    it->second.ready = std::move(published);
                    it->second.rawVisited = built.rawVisited;
                    it->second.state = State::Ready;
                    it->second.lastUse = ++m_useSerial;
                    compact = it->second.ready ? it->second.ready->size() : 0;
                }
                EvictReadyLocked();
            }

            char log[192];
            if (ok) {
                sprintf_s(log, "imgprep:done chunk=%lld visited=%zu compact=%zu ms=%.1f",
                    job.params.chunk, built.rawVisited, compact, ms);
            } else {
                sprintf_s(log, "imgprep:fail chunk=%lld ms=%.1f fallback=exact",
                    job.params.chunk, ms);
            }
            HeartbeatLog(log);
        }
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
    const ImageBufferPreparedSource* m_activeSource = nullptr;
    uint64_t m_activeSignature = 0;
    uint64_t m_jobSerial = 0;
    uint64_t m_useSerial = 0;
    std::vector<std::thread> m_workers;
    std::priority_queue<Job, std::vector<Job>, JobLater> m_jobs;
    std::unordered_map<ImageBufferPreparedKey, Entry, ImageBufferPreparedKeyHash> m_entries;
    std::unordered_map<uint64_t, std::pair<long long, long long>> m_prime;
    std::unordered_set<uint64_t> m_pendingRender;

    const void* m_fullOwner = nullptr;
    const ImageBufferPreparedSource* m_fullSource = nullptr;
    uint64_t m_fullSignature = 0;
    bool m_fullInitialized = false;
    bool m_fullUnsupported = false;
    std::vector<ImageBufferPreparedKey> m_fullKeys;
    std::unordered_set<ImageBufferPreparedKey, ImageBufferPreparedKeyHash> m_pinned;

    bool m_playbackGateArmed = false;
    const void* m_playbackGateOwner = nullptr;
};

inline ImageBufferPreparedManager& ImageBufferPreparedGet()
{
    static ImageBufferPreparedManager manager;
    return manager;
}

inline ImageBufferPreparedParams ImageBufferPreparedMakeParams(
    long long chunk,
    long long timeSpan,
    long long margin,
    bool tickMode,
    float corruption,
    size_t trackCount,
    int rows)
{
    ImageBufferPreparedParams params;
    params.chunk = chunk;
    params.timeSpan = timeSpan;
    params.corruptionMargin = margin;
    params.corruption = corruption;
    params.trackCount = trackCount;
    params.rows = rows;
    params.tickMode = tickMode;
    return params;
}

inline void ImageBufferPreparedPrime(
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
    for (long long chunk = first; chunk <= last; ++chunk) {
        const size_t estimate = ImageBufferPreparedEstimateStarts(
            *source, chunk, timeSpan, margin, tickMode);
        if (estimate >= ImageBufferPreparedDenseThreshold)
            dense.push_back({ chunk, estimate });
    }

    std::unordered_set<long long> scheduled;
    for (const auto& center : dense) {
        const long long lo = (std::max)(first, center.first - ImageBufferPreparedPreloadRadius);
        const long long hi = (std::min)(last, center.first + ImageBufferPreparedPreloadRadius);
        for (long long chunk = lo; chunk <= hi; ++chunk) {
            if (!scheduled.insert(chunk).second)
                continue;

            const size_t estimate = ImageBufferPreparedEstimateStarts(
                *source, chunk, timeSpan, margin, tickMode);
            const ImageBufferPreparedKey key{ source.get(), chunk, signature };
            const ImageBufferPreparedParams params = ImageBufferPreparedMakeParams(
                chunk, timeSpan, margin, tickMode, corruption, trackCount, rows);
            const int priority = 100 + (int)(std::max)(0LL, chunk - first);
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

inline void ImageBufferPreparedPrimeAllDense(
    const void* owner,
    const std::shared_ptr<const ImageBufferPreparedSource>& source,
    long long timeSpan,
    long long margin,
    bool tickMode,
    float corruption,
    size_t trackCount,
    int rows,
    uint64_t signature)
{
    if (!ImageBufferPreparedGetWaitBeforePlayback() || !source || source->notes.empty())
        return;

    auto& manager = ImageBufferPreparedGet();
    if (!manager.BeginFullPrime(owner, source.get(), signature))
        return;

    const long long firstStart = ImageBufferPreparedStartValue(source->notes.front(), tickMode);
    const long long lastStart = ImageBufferPreparedStartValue(source->notes.back(), tickMode);
    const long long first = ImageBufferPreparedFloorDiv(
        ImageBufferOverlapSaturatingAdd(firstStart, -margin), timeSpan) - 1;
    const long long last = ImageBufferPreparedFloorDiv(
        ImageBufferOverlapSaturatingAdd(lastStart, margin), timeSpan) + 1;

    std::vector<std::pair<long long, size_t>> dense;
    dense.reserve((size_t)(last - first + 1));
    for (long long center = first; center <= last; ++center) {
        const size_t estimate = ImageBufferPreparedEstimateStarts(
  *source, center, timeSpan, margin, tickMode);
        if (estimate >= ImageBufferPreparedDenseThreshold)
  dense.push_back({ center, estimate });
    }

    std::unordered_set<long long> scheduled;
    for (const auto& center : dense) {
        const long long lo = (std::max)(first,
  center.first - (long long)ImageBufferPreparedPreloadRadius);
        const long long hi = (std::min)(last,
  center.first + (long long)ImageBufferPreparedPreloadRadius);
        for (long long chunk = lo; chunk <= hi; ++chunk) {
  const bool firstSchedule = scheduled.insert(chunk).second;
  const size_t estimate = ImageBufferPreparedEstimateStarts(
      *source, chunk, timeSpan, margin, tickMode);
  const ImageBufferPreparedKey key{ source.get(), chunk, signature };
  const ImageBufferPreparedParams params = ImageBufferPreparedMakeParams(
      chunk, timeSpan, margin, tickMode, corruption, trackCount, rows);
  const int priority = 10 + (int)(std::min)(
      std::llabs(chunk - center.first), 1000000LL);
  // Schedule duplicates too: Schedule() is idempotent, but a chunk
  // reached from a nearer dense center may receive a better priority.
  manager.Schedule(key, source, params, priority, estimate, true);
  (void)firstSchedule;
        }
    }

    char log[176];
    sprintf_s(log, "imgprep:full-prime dense=%zu jobs=%zu horizon=%lld..%lld",
        dense.size(), scheduled.size(), first, last);
    HeartbeatLog(log);
}

template <typename MidiT>
inline bool ImageBufferPreparedTryCollect(
    const void* owner,
    Renderer* renderer,
    const std::vector<MIDIChannelEvent>& events,
    MidiT& midi,
    std::vector<NoteData>& out,
    long long chunk,
    long long firstVisible,
    long long lastVisible,
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
        if (ImageBufferPreparedGetWaitBeforePlayback())
            ImageBufferPreparedGet().MarkFullPrimeUnsupported(owner);
        return false;
    }

    rows = (std::min)((std::max)(rows, 64), ImageBufferPreparedMaxRows);
    const uint64_t signature = ImageBufferPreparedSignature(
        source.get(), timeSpan, tickMode, corruption, trackCount, rows);

    auto& manager = ImageBufferPreparedGet();
    manager.Activate(source.get(), signature);

    if (ImageBufferPreparedGetWaitBeforePlayback()) {
        ImageBufferPreparedPrimeAllDense(owner, source, timeSpan, margin,
            tickMode, corruption, trackCount, rows, signature);
    }

    ImageBufferPreparedPrime(source, firstVisible, preloadLast,
        timeSpan, margin, tickMode, corruption, trackCount, rows, signature);

    const ImageBufferPreparedKey key{ source.get(), chunk, signature };
    const size_t estimate = ImageBufferPreparedEstimateStarts(
        *source, chunk, timeSpan, margin, tickMode);
    ImageBufferPreparedManager::State state = manager.StateOf(key);
    const bool ownsChunk = state != ImageBufferPreparedManager::State::Missing ||
        estimate >= ImageBufferPreparedDenseThreshold;

    if (!ownsChunk || state == ImageBufferPreparedManager::State::Failed) {
        manager.ClearPending(renderer, chunk);
        return false;
    }

    if (state == ImageBufferPreparedManager::State::Ready) {
        if (auto ready = manager.Ready(key)) {
            out.assign(ready->begin(), ready->end());
            manager.ClearPending(renderer, chunk);
            return true;
        }
    }

    const bool visible = chunk >= firstVisible && chunk <= lastVisible;
    const int priority = visible ? 0 : 50 + (int)(std::max)(0LL, chunk - firstVisible);
    const ImageBufferPreparedParams params = ImageBufferPreparedMakeParams(
        chunk, timeSpan, margin, tickMode, corruption, trackCount, rows);
    manager.Schedule(key, source, params, priority, estimate);

    if (auto ready = manager.Ready(key)) {
        out.assign(ready->begin(), ready->end());
        manager.ClearPending(renderer, chunk);
        return true;
    }

    manager.ClearPending(renderer, chunk);
    return false;
}

inline bool ImageBufferPreparedRenderPending(Renderer* renderer, long long chunk)
{
    return ImageBufferPreparedGet().IsPending(renderer, chunk);
}

inline ImageBufferPreparedProgress ImageBufferPreparedGetFullProgress()
{
    return ImageBufferPreparedGet().FullProgress();
}

inline void ImageBufferPreparedMarkPrewarmUnavailable(const void* owner)
{
    if (ImageBufferPreparedGetWaitBeforePlayback())
        ImageBufferPreparedGet().MarkFullPrimeUnsupported(owner);
}

inline void ImageBufferPreparedArmPlaybackGate(const void* owner)
{
    ImageBufferPreparedGet().ArmPlaybackGate(owner);
}

inline void ImageBufferPreparedCancelPlaybackGate()
{
    ImageBufferPreparedGet().CancelPlaybackGate();
}

inline bool ImageBufferPreparedShouldHoldPlayback(const void* owner)
{
    return ImageBufferPreparedGet().ShouldHoldPlayback(owner);
}

inline bool ImageBufferPreparedPlaybackGateArmed(const void* owner)
{
    return ImageBufferPreparedGet().PlaybackGateArmedFor(owner);
}
