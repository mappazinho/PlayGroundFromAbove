#include <algorithm>
#include <cmath>
#include <tchar.h>
#include <ppl.h>
#include <dwmapi.h>
#include <fstream>
#include <pdh.h>
#include <thread>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <type_traits>
#include <deque>
#include <chrono>
#include <unordered_map>
#include "Globals.h"
#include "GameState.h"
#include "Config.h"
#include "resource.h"
#include "ConfigProcs.h"
#include "MainProcs.h"
#include "MIDIPreRenderPlayer.h"
#include <d3d9types.h>
#include "ImageBufferMultipass.h"
#include "ImageBufferOverlapIndex.h"
#include "ImageBufferPreparedChunks.h"
#include "ImageBufferGpuPins.h"

static constexpr size_t ImageBufferFullPreparedMaxEvents = 64000000;
static constexpr size_t ImageBufferFullPreparedMaxNotes = 32000000;

struct ImageBufferPrewarmGpuState {
    const void* owner = nullptr;
    Renderer* renderer = nullptr;
    bool initialized = false;
    bool cacheRequired = false;
    bool hugeMode = false;
    uint64_t hugeSignature = 0;
    bool playRequested = false;
    size_t cached = 0;
    size_t total = 0;
    size_t cpuPrepared = 0;
    size_t cpuTotal = 0;
    std::vector<long long> chunks;
    bool restartKeepReady = false; // seek-restart: reuse already-prepared chunk data
};

struct ImageBufferHugePreparedState {
    const void* owner = nullptr;
    uint64_t signature = 0;
    std::vector<long long> chunks;
    size_t next = 0;
    std::unordered_map<long long, std::shared_ptr<const std::vector<NoteData>>> ready;
};

static std::mutex s_ImageBufferPrewarmGpuMutex;
static ImageBufferPrewarmGpuState s_ImageBufferPrewarmGpu;
static std::mutex s_ImageBufferHugePreparedMutex;
static ImageBufferHugePreparedState s_ImageBufferHugePrepared;

static void ImageBufferHugePreparedReset(
    const void* owner, uint64_t signature, std::vector<long long> chunks, bool preserveReady = false)
{
    std::lock_guard<std::mutex> lock(s_ImageBufferHugePreparedMutex);
    // Preserve freshly-valid prepared chunks across a seek-restart so playback
    // resumes as soon as anything actually missing has been rebuilt.
    std::unordered_map<long long, std::shared_ptr<const std::vector<NoteData>>> keep;
    if (preserveReady && s_ImageBufferHugePrepared.signature == signature)
        keep = std::move(s_ImageBufferHugePrepared.ready);
    s_ImageBufferHugePrepared.owner = owner;
    s_ImageBufferHugePrepared.signature = signature;
    s_ImageBufferHugePrepared.chunks = std::move(chunks);
    s_ImageBufferHugePrepared.next = 0;
    s_ImageBufferHugePrepared.ready.clear();
    s_ImageBufferHugePrepared.ready.reserve(s_ImageBufferHugePrepared.chunks.size());
    for (const auto& it : keep) {
        if (std::binary_search(s_ImageBufferHugePrepared.chunks.begin(),
                s_ImageBufferHugePrepared.chunks.end(), it.first))
            s_ImageBufferHugePrepared.ready[it.first] = it.second;
    }
}

static void ImageBufferHugePreparedClear(const void* owner = nullptr)
{
    std::lock_guard<std::mutex> lock(s_ImageBufferHugePreparedMutex);
    if (owner && s_ImageBufferHugePrepared.owner != owner)
        return;
    s_ImageBufferHugePrepared = {};
}

static std::shared_ptr<const std::vector<NoteData>> ImageBufferHugePreparedGet(
    const void* owner, long long chunk)
{
    std::lock_guard<std::mutex> lock(s_ImageBufferHugePreparedMutex);
    if (s_ImageBufferHugePrepared.owner != owner)
        return {};
    const auto it = s_ImageBufferHugePrepared.ready.find(chunk);
    return it == s_ImageBufferHugePrepared.ready.end() ? nullptr : it->second;
}

static bool ImageBufferHugePreparedNext(
    const void* owner, uint64_t signature, long long& chunk, size_t& done, size_t& total)
{
    std::lock_guard<std::mutex> lock(s_ImageBufferHugePreparedMutex);
    if (s_ImageBufferHugePrepared.owner != owner ||
        s_ImageBufferHugePrepared.signature != signature) {
        done = total = 0;
        return false;
    }
    total = s_ImageBufferHugePrepared.chunks.size();
    done = s_ImageBufferHugePrepared.ready.size();
    while (s_ImageBufferHugePrepared.next < s_ImageBufferHugePrepared.chunks.size()) {
        const long long candidate = s_ImageBufferHugePrepared.chunks[s_ImageBufferHugePrepared.next++];
        if (s_ImageBufferHugePrepared.ready.find(candidate) == s_ImageBufferHugePrepared.ready.end()) {
            chunk = candidate;
            return true;
        }
    }
    return false;
}

static void ImageBufferHugePreparedPublish(
    const void* owner, uint64_t signature, long long chunk, std::vector<NoteData> notes,
    size_t& done, size_t& total)
{
    std::shared_ptr<const std::vector<NoteData>> published;
    try {
        published = std::make_shared<const std::vector<NoteData>>(std::move(notes));
    } catch (...) {
        published.reset();
    }
    std::lock_guard<std::mutex> lock(s_ImageBufferHugePreparedMutex);
    if (s_ImageBufferHugePrepared.owner != owner ||
        s_ImageBufferHugePrepared.signature != signature) {
        done = total = 0;
        return;
    }
    if (published)
        s_ImageBufferHugePrepared.ready[chunk] = std::move(published);
    total = s_ImageBufferHugePrepared.chunks.size();
    done = s_ImageBufferHugePrepared.ready.size();
}

static bool ImageBufferFullPreparedSupported(
    const std::vector<MIDIChannelEvent>& events,
    const MIDI& midi)
{
    return events.size() <= ImageBufferFullPreparedMaxEvents &&
        midi.GetInfo().iNoteCount <= ImageBufferFullPreparedMaxNotes;
}

static void ImageBufferLogFullPrewarmSkip(
    const void* owner, size_t events, size_t notes)
{
    static const void* lastOwner = nullptr;
    static size_t lastEvents = 0;
    static size_t lastNotes = 0;
    if (owner == lastOwner && events == lastEvents && notes == lastNotes)
        return;
    lastOwner = owner;
    lastEvents = events;
    lastNotes = notes;
    char log[160];
    sprintf_s(log, "imghuge:enabled events=%zu notes=%zu", events, notes);
    HeartbeatLog(log);
}

static constexpr size_t ImageBufferHugeBlockEvents = 4096;

static size_t ImageBufferHugeEstimateCandidates(
    const std::vector<MIDIChannelEvent>& events,
    MIDI& midi,
    long long chunk,
    long long timeSpan,
    long long corruptionMargin,
    bool tickMode,
    const std::vector<long long>& maxEndTime,
    const std::vector<long long>& maxEndTick,
    const std::vector<long long>& prefixEndTime,
    const std::vector<long long>& prefixEndTick)
{
    if (events.empty() || timeSpan <= 0)
        return 0;

    const long long chunkStart = chunk * timeSpan;
    const long long hiValue = ImageBufferOverlapSaturatingAdd(
        ImageBufferOverlapSaturatingAdd(chunkStart, timeSpan), corruptionMargin);
    const long long oldestUsefulEnd = ImageBufferOverlapSaturatingAdd(
        chunkStart, -corruptionMargin);
    auto lessTime = [&](MIDIChannelEvent event, long long value) {
        return (tickMode ? (long long)midi.GetEventAbsT(event) : midi.GetEventTime(event)) < value;
    };
    const size_t hi = (size_t)(std::lower_bound(
        events.begin(), events.end(), hiValue, lessTime) - events.begin());
    if (hi == 0)
        return 0;

    const auto& blockMax = tickMode ? maxEndTick : maxEndTime;
    const auto& prefixMax = tickMode ? prefixEndTick : prefixEndTime;
    if (blockMax.empty() || prefixMax.size() != blockMax.size())
        return hi;

    size_t total = 0;
    size_t block = (std::min)((hi - 1) / ImageBufferHugeBlockEvents, blockMax.size() - 1);
    for (;;) {
        if (prefixMax[block] < oldestUsefulEnd)
            break;
        if (blockMax[block] >= oldestUsefulEnd) {
            const size_t begin = block * ImageBufferHugeBlockEvents;
            const size_t end = (std::min)(hi, begin + ImageBufferHugeBlockEvents);
            if (end > begin) {
                const size_t add = end - begin;
                total = total > SIZE_MAX - add ? SIZE_MAX : total + add;
            }
        }
        if (block == 0)
            break;
        --block;
    }
    return total;
}

template <typename BuildFn>
static void ImageBufferCollectHugeIndexedExact(
    const std::vector<MIDIChannelEvent>& events,
    MIDI& midi,
    std::vector<NoteData>& out,
    long long chunk,
    long long timeSpan,
    long long corruptionMargin,
    bool tickMode,
    const std::vector<long long>& maxEndTime,
    const std::vector<long long>& maxEndTick,
    const std::vector<long long>& prefixEndTime,
    const std::vector<long long>& prefixEndTick,
    BuildFn&& buildNote)
{
    out.clear();
    if (events.empty() || timeSpan <= 0)
        return;

    const long long chunkStart = chunk * timeSpan;
    const long long hiValue = ImageBufferOverlapSaturatingAdd(
        ImageBufferOverlapSaturatingAdd(chunkStart, timeSpan), corruptionMargin);
    const long long oldestUsefulEnd = ImageBufferOverlapSaturatingAdd(
        chunkStart, -corruptionMargin);
    auto lessTime = [&](MIDIChannelEvent event, long long value) {
        return (tickMode ? (long long)midi.GetEventAbsT(event) : midi.GetEventTime(event)) < value;
    };
    const size_t hi = (size_t)(std::lower_bound(
        events.begin(), events.end(), hiValue, lessTime) - events.begin());
    if (hi == 0)
        return;

    const auto& blockMax = tickMode ? maxEndTick : maxEndTime;
    const auto& prefixMax = tickMode ? prefixEndTick : prefixEndTime;
    if (blockMax.empty() || prefixMax.size() != blockMax.size())
        return;

    size_t block = (std::min)((hi - 1) / ImageBufferHugeBlockEvents, blockMax.size() - 1);
    for (;;) {
        if (prefixMax[block] < oldestUsefulEnd)
            break;
        if (blockMax[block] >= oldestUsefulEnd) {
            const size_t begin = block * ImageBufferHugeBlockEvents;
            const size_t end = (std::min)(hi, begin + ImageBufferHugeBlockEvents);
            for (size_t i = end; i != begin; ) {
                --i;
                const MIDIChannelEvent event = events[i];
                if (midi.GetEventChannelEventType(event) != MIDI::NoteOn ||
                    midi.GetEventParam2(event) <= 0 || !midi.EventHasSister(event))
                    continue;
                NoteData data = buildNote(event, chunkStart);
                if (data.pos < (float)timeSpan &&
                    data.pos + (std::max)(data.length, 0.0f) >= 0.0f)
                    out.push_back(data);
            }
        }
        if (block == 0)
            break;
        --block;
    }
}

template <typename BuildFn, typename HiddenFn>
static void ImageBufferCollectHugeIndexedCompact(
    const std::vector<MIDIChannelEvent>& events,
    MIDI& midi,
    std::vector<NoteData>& out,
    long long chunk,
    long long timeSpan,
    long long corruptionMargin,
    bool tickMode,
    int rows,
    bool stablePitch,
    const std::vector<long long>& maxEndTime,
    const std::vector<long long>& maxEndTick,
    const std::vector<long long>& prefixEndTime,
    const std::vector<long long>& prefixEndTick,
    BuildFn&& buildNote,
    HiddenFn&& isHidden)
{
    out.clear();
    if (events.empty() || timeSpan <= 0)
        return;

    rows = (std::min)((std::max)(rows, 64), ImageBufferPreparedMaxRows);
    std::vector<int> next((size_t)128 * (size_t)(rows + 1));
    for (int key = 0; key < 128; ++key) {
        const size_t base = (size_t)key * (size_t)(rows + 1);
        for (int row = 0; row <= rows; ++row)
            next[base + row] = row;
    }
    auto findNext = [&](int key, int row) {
        const size_t base = (size_t)key * (size_t)(rows + 1);
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

    const long long chunkStart = chunk * timeSpan;
    const long long hiValue = ImageBufferOverlapSaturatingAdd(
        ImageBufferOverlapSaturatingAdd(chunkStart, timeSpan), corruptionMargin);
    const long long oldestUsefulEnd = ImageBufferOverlapSaturatingAdd(
        chunkStart, -corruptionMargin);
    auto lessTime = [&](MIDIChannelEvent event, long long value) {
        return (tickMode ? (long long)midi.GetEventAbsT(event) : midi.GetEventTime(event)) < value;
    };
    const size_t hi = (size_t)(std::lower_bound(
        events.begin(), events.end(), hiValue, lessTime) - events.begin());
    if (hi == 0)
        return;

    const auto& blockMax = tickMode ? maxEndTick : maxEndTime;
    const auto& prefixMax = tickMode ? prefixEndTick : prefixEndTime;
    if (blockMax.empty() || prefixMax.size() != blockMax.size())
        return;

    int firstKey = 0;
    int lastKey = 127;
    if (stablePitch && midi.GetInfo().iMinNote >= 0 &&
        midi.GetInfo().iMaxNote >= midi.GetInfo().iMinNote) {
        firstKey = (std::min)((std::max)((int)midi.GetInfo().iMinNote, 0), 127);
        lastKey = (std::min)((std::max)((int)midi.GetInfo().iMaxNote, firstKey), 127);
    }
    int remainingByKey[128] = {};
    for (int key = firstKey; key <= lastKey; ++key)
        remainingByKey[key] = rows;
    size_t remaining = (size_t)(lastKey - firstKey + 1) * (size_t)rows;
    size_t block = (std::min)((hi - 1) / ImageBufferHugeBlockEvents, blockMax.size() - 1);
    for (;;) {
        if (prefixMax[block] < oldestUsefulEnd)
            break;
        if (blockMax[block] >= oldestUsefulEnd) {
            const size_t begin = block * ImageBufferHugeBlockEvents;
            const size_t end = (std::min)(hi, begin + ImageBufferHugeBlockEvents);
            for (size_t i = end; i != begin && remaining > 0; ) {
                --i;
                const MIDIChannelEvent event = events[i];
                if (midi.GetEventChannelEventType(event) != MIDI::NoteOn ||
                    midi.GetEventParam2(event) <= 0 || !midi.EventHasSister(event))
                    continue;
                if (stablePitch) {
                    const int sourceKey = midi.GetEventParam1(event);
                    if (sourceKey < firstKey || sourceKey > lastKey ||
                        remainingByKey[sourceKey] == 0)
                        continue;
                }

                NoteData data = buildNote(event, chunkStart);
                if (data.key >= 128 || data.key < firstKey || data.key > lastKey ||
                    remainingByKey[data.key] == 0 || isHidden(data.track, data.channel))
                    continue;
                const double relStart = (double)data.pos;
                const double relEnd = relStart + (std::max)(0.0, (double)data.length);
                if (relStart >= (double)timeSpan || relEnd < 0.0)
                    continue;

                const double clippedStart = (std::max)(0.0, relStart);
                const double clippedEnd = (std::min)((double)timeSpan, relEnd);
                int row0 = (int)std::floor(clippedStart * rows / (double)timeSpan);
                int row1 = (int)std::ceil(clippedEnd * rows / (double)timeSpan);
                row0 = (std::min)((std::max)(row0, 0), rows - 1);
                row1 = (std::min)((std::max)(row1, row0 + 1), rows);

                int row = findNext((int)data.key, row0);
                const size_t base = (size_t)data.key * (size_t)(rows + 1);
                while (row < row1 && remaining > 0) {
                    const int runStart = row;
                    int runEnd = row;
                    for (;;) {
                        const int current = row;
                        const int following = findNext((int)data.key, current + 1);
                        next[base + current] = following;
                        --remaining;
                        --remainingByKey[data.key];
                        runEnd = current + 1;
                        row = following;
                        if (row >= row1 || row != current + 1)
                            break;
                    }

                    NoteData segment = data;
                    segment.pos = (float)((double)runStart * (double)timeSpan / (double)rows);
                    segment.length = (float)((double)(runEnd - runStart) *
                        (double)timeSpan / (double)rows);
                    out.push_back(segment);
                }
            }
        }
        if (remaining == 0 || block == 0)
            break;
        --block;
    }
}

template <typename BuildFn, typename HiddenFn>
static void ImageBufferCollectHugeAdaptive(
    const std::vector<MIDIChannelEvent>& events,
    MIDI& midi,
    std::vector<NoteData>& out,
    long long chunk,
    long long timeSpan,
    long long corruptionMargin,
    bool tickMode,
    int rows,
    bool stablePitch,
    const std::vector<long long>& maxEndTime,
    const std::vector<long long>& maxEndTick,
    const std::vector<long long>& prefixEndTime,
    const std::vector<long long>& prefixEndTick,
    BuildFn&& buildNote,
    HiddenFn&& isHidden)
{
    const size_t estimate = ImageBufferHugeEstimateCandidates(
        events, midi, chunk, timeSpan, corruptionMargin, tickMode,
        maxEndTime, maxEndTick, prefixEndTime, prefixEndTick);
    if (estimate < ImageBufferPreparedDenseThreshold) {
        ImageBufferCollectHugeIndexedExact(
            events, midi, out, chunk, timeSpan, corruptionMargin, tickMode,
            maxEndTime, maxEndTick, prefixEndTime, prefixEndTick,
            std::forward<BuildFn>(buildNote));
        return;
    }
    const bool logDense = g_bLoggingEnabled.load(std::memory_order_relaxed);
    const auto compactStart = logDense ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    ImageBufferCollectHugeIndexedCompact(
        events, midi, out, chunk, timeSpan, corruptionMargin, tickMode, rows, stablePitch,
        maxEndTime, maxEndTick, prefixEndTime, prefixEndTick,
        std::forward<BuildFn>(buildNote), std::forward<HiddenFn>(isHidden));
    if (logDense) {
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - compactStart).count();
        static std::atomic<unsigned long long> lastHugeCompactLog{ 0 };
        const unsigned long long nowMs = GetTickCount64();
        unsigned long long lastMs = lastHugeCompactLog.load(std::memory_order_relaxed);
        if (nowMs - lastMs >= 1000 &&
            lastHugeCompactLog.compare_exchange_strong(lastMs, nowMs,
                std::memory_order_relaxed)) {
            char log[192];
            sprintf_s(log, "imghuge:compact chunk=%lld candidates=%zu compact=%zu ms=%.1f",
                chunk, estimate, out.size(), ms);
            HeartbeatLog(log);
        }
    }
}

static std::vector<long long> ImageBufferSelectHugePrewarmChunks(
    const std::vector<MIDIChannelEvent>& events,
    MIDI& midi,
    long long timeSpan,
    long long corruptionMargin,
    bool tickMode,
    const std::vector<long long>& maxEndTime,
    const std::vector<long long>& maxEndTick,
    size_t limit)
{
    std::vector<long long> result;
    if (events.empty() || timeSpan <= 0 || limit == 0)
        return result;

    const auto& blockMax = tickMode ? maxEndTick : maxEndTime;
    if (blockMax.empty())
        return result;

    const long long firstValue = tickMode
        ? (long long)midi.GetEventAbsT(events.front()) : midi.GetEventTime(events.front());
    const long long lastValue = tickMode
        ? (long long)midi.GetEventAbsT(events.back()) : midi.GetEventTime(events.back());
    const long long first = ImageBufferPreparedFloorDiv(
        ImageBufferOverlapSaturatingAdd(firstValue, -corruptionMargin), timeSpan) - 1;
    const long long last = ImageBufferPreparedFloorDiv(
        ImageBufferOverlapSaturatingAdd(lastValue, corruptionMargin), timeSpan) + 1;

    struct ActiveBlock {
        long long end = 0;
        size_t block = 0;
        bool operator>(const ActiveBlock& other) const { return end > other.end; }
    };
    std::priority_queue<ActiveBlock, std::vector<ActiveBlock>, std::greater<ActiveBlock>> active;
    size_t nextBlock = 0;
    size_t activeEvents = 0;
    std::vector<std::pair<size_t, long long>> scored;
    scored.reserve((size_t)(std::max)(0LL, last - first + 1));

    auto lessTime = [&](MIDIChannelEvent event, long long value) {
        return (tickMode ? (long long)midi.GetEventAbsT(event) : midi.GetEventTime(event)) < value;
    };

    for (long long chunk = first; chunk <= last; ++chunk) {
        const long long chunkStart = chunk * timeSpan;
        const long long hiValue = ImageBufferOverlapSaturatingAdd(
            ImageBufferOverlapSaturatingAdd(chunkStart, timeSpan), corruptionMargin);
        const long long oldest = ImageBufferOverlapSaturatingAdd(
            chunkStart, -corruptionMargin);
        const size_t hi = (size_t)(std::lower_bound(
            events.begin(), events.end(), hiValue, lessTime) - events.begin());
        const size_t lastBlock = hi == 0 ? 0 : (hi - 1) / ImageBufferHugeBlockEvents;

        while (nextBlock < blockMax.size() && hi > 0 && nextBlock <= lastBlock) {
            const size_t begin = nextBlock * ImageBufferHugeBlockEvents;
            const size_t count = (std::min)(events.size(), begin + ImageBufferHugeBlockEvents) - begin;
            active.push(ActiveBlock{ blockMax[nextBlock], nextBlock });
            activeEvents = activeEvents > SIZE_MAX - count ? SIZE_MAX : activeEvents + count;
            ++nextBlock;
        }
        while (!active.empty() && active.top().end < oldest) {
            const size_t begin = active.top().block * ImageBufferHugeBlockEvents;
            const size_t count = (std::min)(events.size(), begin + ImageBufferHugeBlockEvents) - begin;
            activeEvents = activeEvents >= count ? activeEvents - count : 0;
            active.pop();
        }

        if (activeEvents >= ImageBufferPreparedDenseThreshold)
            scored.push_back({ activeEvents, chunk });
    }

    if (scored.size() > limit) {
        std::nth_element(scored.begin(), scored.begin() + limit, scored.end(),
            [](const auto& a, const auto& b) {
                if (a.first != b.first)
                    return a.first > b.first;
                return a.second < b.second;
            });
        scored.resize(limit);
    }
    std::sort(scored.begin(), scored.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });
    result.reserve(scored.size());
    for (const auto& item : scored)
        result.push_back(item.second);
    return result;
}

static size_t CountImageBufferPrewarmCached(
    Renderer* renderer, const std::vector<long long>& chunks)
{
    if (!renderer)
        return 0;
    size_t cached = 0;
    for (long long chunk : chunks)
        if (renderer->ImageBufferChunkCached(chunk))
            ++cached;
    return cached;
}

static void UpdateImageBufferPrewarmGpuProgress(
    const void* owner,
    Renderer* renderer,
    long long timeSpan,
    long long margin,
    bool tickMode,
    long long tStart = 0)
{
    if (!owner || !renderer || !ImageBufferPreparedGetWaitBeforePlayback())
        return;

    const ImageBufferPreparedProgress cpu = ImageBufferPreparedGetFullProgress();
    if (!cpu.initialized)
        return;

    if (cpu.unsupported) {
        {
            std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
            if (s_ImageBufferPrewarmGpu.owner == owner && s_ImageBufferPrewarmGpu.hugeMode) {
                s_ImageBufferPrewarmGpu.renderer = renderer;
                s_ImageBufferPrewarmGpu.cached = CountImageBufferPrewarmCached(
                    renderer, s_ImageBufferPrewarmGpu.chunks);
                return;
            }
        }
        ImageBufferClearPinnedChunks(renderer);
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        s_ImageBufferPrewarmGpu.owner = owner;
        s_ImageBufferPrewarmGpu.renderer = renderer;
        s_ImageBufferPrewarmGpu.initialized = true;
        s_ImageBufferPrewarmGpu.cacheRequired = false;
        s_ImageBufferPrewarmGpu.hugeMode = false;
        s_ImageBufferPrewarmGpu.hugeSignature = 0;
        s_ImageBufferPrewarmGpu.cached = 0;
        s_ImageBufferPrewarmGpu.total = 0;
        s_ImageBufferPrewarmGpu.chunks.clear();
        return;
    }

    if (cpu.done < cpu.total)
        return;

    auto& overlap = ImageBufferOverlapIndexGet();
    const auto source = overlap.preparedSource;
    if (overlap.owner != owner || !source || source->notes.empty() || timeSpan <= 0)
        return;

    std::vector<long long> prewarmChunks;
    const long long firstStart = ImageBufferPreparedStartValue(source->notes.front(), tickMode);
    const long long lastStart = ImageBufferPreparedStartValue(source->notes.back(), tickMode);
    const long long first = ImageBufferPreparedFloorDiv(
        ImageBufferOverlapSaturatingAdd(firstStart, -margin), timeSpan) - 1;
    const long long last = ImageBufferPreparedFloorDiv(
        ImageBufferOverlapSaturatingAdd(lastStart, margin), timeSpan) + 1;

    for (long long center = first; center <= last; ++center) {
        const size_t estimate = ImageBufferPreparedEstimateStarts(
            *source, center, timeSpan, margin, tickMode);
        if (estimate < ImageBufferPreparedDenseThreshold)
            continue;

        const long long lo = (std::max)(first,
            center - (long long)ImageBufferPreparedPreloadRadius);
        const long long hi = (std::min)(last,
            center + (long long)ImageBufferPreparedPreloadRadius);
        long long appendFrom = lo;
        if (!prewarmChunks.empty())
            appendFrom = (std::max)(appendFrom, prewarmChunks.back() + 1);
        for (long long chunk = appendFrom; chunk <= hi; ++chunk)
            prewarmChunks.push_back(chunk);
    }

    static constexpr size_t kImageBufferRuntimeReserveSlots = 8;
    const size_t gpuCapacity = (size_t)Renderer::ChunkPoolSize - kImageBufferRuntimeReserveSlots;
    const long long startChunk = timeSpan > 0 ? ImageBufferPreparedFloorDiv(tStart, timeSpan) : first;

    if (prewarmChunks.size() > gpuCapacity) {
        std::sort(prewarmChunks.begin(), prewarmChunks.end(), [&](long long a, long long b) {
            long long distA = a >= startChunk - 2 ? (a - startChunk + 2) : (startChunk - a + 1000000);
            long long distB = b >= startChunk - 2 ? (b - startChunk + 2) : (startChunk - b + 1000000);
            return distA < distB;
        });
        prewarmChunks.resize(gpuCapacity);
        std::sort(prewarmChunks.begin(), prewarmChunks.end());
    }

    const bool requireGpu = !prewarmChunks.empty() && cpu.failed == 0;
    const size_t cached = requireGpu
        ? CountImageBufferPrewarmCached(renderer, prewarmChunks)
        : 0;

    if (requireGpu)
        ImageBufferSetPinnedChunks(renderer, prewarmChunks);
    else
        ImageBufferClearPinnedChunks(renderer);

    std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
    const bool keepPlayRequest = s_ImageBufferPrewarmGpu.playRequested;
    s_ImageBufferPrewarmGpu.owner = owner;
    s_ImageBufferPrewarmGpu.renderer = renderer;
    s_ImageBufferPrewarmGpu.initialized = true;
    s_ImageBufferPrewarmGpu.cacheRequired = requireGpu && !prewarmChunks.empty();
    s_ImageBufferPrewarmGpu.hugeMode = false;
    s_ImageBufferPrewarmGpu.hugeSignature = 0;
    s_ImageBufferPrewarmGpu.cached = cached;
    s_ImageBufferPrewarmGpu.total = prewarmChunks.size();
    s_ImageBufferPrewarmGpu.chunks = std::move(prewarmChunks);
    s_ImageBufferPrewarmGpu.playRequested = keepPlayRequest;
}

static void DrawImageBufferPrewarmProgress(
    Renderer* renderer, float notesX, float notesCX, float keyboardY)
{
    if (!renderer || !ImageBufferPreparedGetWaitBeforePlayback())
        return;

    const ImageBufferPreparedProgress cpu = ImageBufferPreparedGetFullProgress();

    bool playRequested = false;
    bool gpuStage = false;
    bool initializing = false;
    bool hugeMode = false;
    size_t done = cpu.done;
    size_t total = cpu.total;
    size_t failed = cpu.failed;
    {
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        playRequested = s_ImageBufferPrewarmGpu.playRequested;
        hugeMode = s_ImageBufferPrewarmGpu.hugeMode;
        if (hugeMode && s_ImageBufferPrewarmGpu.cpuPrepared < s_ImageBufferPrewarmGpu.cpuTotal) {
            done = s_ImageBufferPrewarmGpu.cpuPrepared;
            total = s_ImageBufferPrewarmGpu.cpuTotal;
            failed = 0;
        } else if (s_ImageBufferPrewarmGpu.cacheRequired &&
            (hugeMode || (cpu.initialized && cpu.done >= cpu.total))) {
            gpuStage = true;
            done = s_ImageBufferPrewarmGpu.cached;
            total = s_ImageBufferPrewarmGpu.total;
            failed = 0;
        }
    }

    // Never leave a stale 0/N prewarm bar on screen after the Play
    // request has been cancelled or the gate has legitimately opened.
    if (!playRequested)
        return;

    if (!cpu.initialized && !hugeMode) {
        initializing = true;
        done = 0;
        total = 1;
        failed = 0;
    } else if (cpu.unsupported && !gpuStage) {
        return;
    }

    if (!initializing && (total == 0 || done >= total))
        return;

    const float scale = (std::max)(Config::GetConfig().GetVizSettings().fUIScale, 0.5f);
    const float bufferW = (float)renderer->GetBufferWidth();
    float x0 = (std::max)(12.0f * scale, notesX);
    float x1 = (std::min)(bufferW - 12.0f * scale, notesX + notesCX);
    if (x1 - x0 < 160.0f * scale) {
        x0 = 12.0f * scale;
        x1 = bufferW - 12.0f * scale;
    }

    const float barH = 8.0f * scale;
    const float y1 = keyboardY - 8.0f * scale;
    const float y0 = y1 - barH;
    const float fraction = (float)done / (float)(std::max)((size_t)1, total);
    auto* draw = renderer->GetDrawList();
    if (!draw)
        return;

    const float rounding = barH * 0.5f;
    draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 180), rounding);
    draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + (x1 - x0) * fraction, y1),
        IM_COL32(235, 235, 235, 230), rounding);

    char text[128];
    if (initializing) {
        // Fold whatever startup work is still running into this bar so there is
        // continuous feedback from the moment the song screen appears.
        const MIDILoadingProgress& ld = g_LoadingProgress;
        static const char* stageNames[] = { "reading", "decompressing", "parsing",
            "connecting notes", "sorting events", "finalizing" };
        if (ld.stage != MIDILoadingProgress::Stage::Done && ld.max > 0 &&
            ld.stage <= MIDILoadingProgress::Stage::Finalize) {
            unsigned long long prog = ld.progress.load(std::memory_order_relaxed);
            sprintf_s(text, "Preparing dense visual buffers - %s  %llu / %llu",
                stageNames[(int)ld.stage], prog, (unsigned long long)ld.max);
        } else {
            sprintf_s(text, "Initializing dense visual buffers...");
        }
    } else if (gpuStage) {
            sprintf_s(text, "Prewarming dense visual buffers  %zu / %zu", done, total);
    } else if (failed > 0) {
        sprintf_s(text, "Preparing dense visual buffers  %zu / %zu  (%zu fallback)",
            done, total, failed);
    } else {
        sprintf_s(text, "Preparing dense visual buffers  %zu / %zu", done, total);
    }
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float tx = x0 + ((x1 - x0) - textSize.x) * 0.5f;
    const float ty = y0 - textSize.y - 3.0f * scale;
    draw->AddText(ImVec2(tx + 1.0f, ty + 1.0f), IM_COL32(0, 0, 0, 220), text);
    draw->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, 255), text);
}

// RenderText() resets the same ImDrawList used by the note pass. Draw the
// indicator only after EndText so the reset cannot erase it before Present.
static void DrawImageBufferPrewarmProgressLate(Renderer* renderer)
{
    if (!renderer)
        return;
    const float bufferW = (float)renderer->GetBufferWidth();
    const float bufferH = (float)renderer->GetBufferHeight();
    DrawImageBufferPrewarmProgress(renderer, 0.0f, bufferW, bufferH * 0.75f);
}

static void DrawFrameTimeGraphLate(Renderer* renderer);

static bool ImageBufferPrewarmPlayRequestedFor(const void* owner)
{
    std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
    return s_ImageBufferPrewarmGpu.playRequested &&
        (!s_ImageBufferPrewarmGpu.owner || s_ImageBufferPrewarmGpu.owner == owner);
}

template <typename BuildFn, typename HiddenFn>
static void ImageBufferDriveHugePrewarm(
    const void* owner,
    Renderer* renderer,
    const std::vector<MIDIChannelEvent>& events,
    MIDI& midi,
    long long timeSpan,
    long long corruptionMargin,
    bool tickMode,
    float corruption,
    int rows,
    const std::vector<long long>& maxEndTime,
    const std::vector<long long>& maxEndTick,
    const std::vector<long long>& prefixEndTime,
    const std::vector<long long>& prefixEndTick,
    BuildFn&& buildNote,
    HiddenFn&& isHidden)
{
    if (!owner || !renderer || !ImageBufferPreparedGetWaitBeforePlayback())
        return;

    ImageBufferPreparedMarkPrewarmUnavailable(owner);
    uint32_t corruptionBits = 0;
    std::memcpy(&corruptionBits, &corruption, sizeof(corruptionBits));
    uint64_t signature = ImageBufferPreparedMix((uint64_t)timeSpan);
    signature ^= ImageBufferPreparedMix((uint64_t)corruptionMargin);
    signature ^= ImageBufferPreparedMix((uint64_t)rows << 17);
    signature ^= ImageBufferPreparedMix((uint64_t)corruptionBits << 33);
    signature ^= tickMode ? 0x9e3779b97f4a7c15ull : 0x6a09e667f3bcc909ull;

    bool initialize = false;
    bool keepReady = false;
    {
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        initialize = s_ImageBufferPrewarmGpu.owner != owner ||
            !s_ImageBufferPrewarmGpu.initialized ||
            !s_ImageBufferPrewarmGpu.hugeMode ||
            s_ImageBufferPrewarmGpu.hugeSignature != signature;
        // A seek-restart keeps the signature; reuse whatever was already prepared.
        keepReady = s_ImageBufferPrewarmGpu.restartKeepReady &&
            s_ImageBufferPrewarmGpu.hugeSignature == signature;
        s_ImageBufferPrewarmGpu.restartKeepReady = false;
    }

    if (initialize) {
        // With the wait option enabled, prepare every chunk that can take the dense
        // compact path. The GPU cache remains bounded to its existing 64 slots.
        std::vector<long long> cpuChunks = ImageBufferSelectHugePrewarmChunks(
            events, midi, timeSpan, corruptionMargin, tickMode,
            maxEndTime, maxEndTick, (std::numeric_limits<size_t>::max)());
        static constexpr size_t reserveSlots = 8;
        const size_t gpuLimit = (size_t)Renderer::ChunkPoolSize - reserveSlots;
        std::vector<long long> gpuChunks = ImageBufferSelectHugePrewarmChunks(
            events, midi, timeSpan, corruptionMargin, tickMode,
            maxEndTime, maxEndTick, gpuLimit);

        ImageBufferHugePreparedReset(owner, signature, std::move(cpuChunks), keepReady);
        ImageBufferSetPinnedChunks(renderer, gpuChunks);
        const size_t cached = CountImageBufferPrewarmCached(renderer, gpuChunks);

        size_t cpuTotal = 0;
        {
            std::lock_guard<std::mutex> cpuLock(s_ImageBufferHugePreparedMutex);
            cpuTotal = s_ImageBufferHugePrepared.chunks.size();
        }
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        const bool keepPlay = s_ImageBufferPrewarmGpu.playRequested;
        s_ImageBufferPrewarmGpu.owner = owner;
        s_ImageBufferPrewarmGpu.renderer = renderer;
        s_ImageBufferPrewarmGpu.initialized = true;
        s_ImageBufferPrewarmGpu.cacheRequired = !gpuChunks.empty();
        s_ImageBufferPrewarmGpu.hugeMode = true;
        s_ImageBufferPrewarmGpu.hugeSignature = signature;
        s_ImageBufferPrewarmGpu.cached = cached;
        s_ImageBufferPrewarmGpu.total = gpuChunks.size();
        s_ImageBufferPrewarmGpu.cpuPrepared = 0;
        s_ImageBufferPrewarmGpu.cpuTotal = cpuTotal;
        s_ImageBufferPrewarmGpu.chunks = std::move(gpuChunks);
        s_ImageBufferPrewarmGpu.playRequested = keepPlay;
        {
            char b[96];
            sprintf_s(b, "prewarm:huge-init cpuChunks=%zu gpuChunks=%zu",
                cpuTotal, gpuChunks.size());
            HeartbeatLog(b);
        }
    }

    // Do one dense CPU chunk per held frame so the wait UI stays responsive.
    long long prepareChunk = Renderer::ImageBufferInvalidChunk;
    size_t cpuDone = 0;
    size_t cpuTotal = 0;
    if (ImageBufferHugePreparedNext(owner, signature, prepareChunk, cpuDone, cpuTotal)) {
        std::vector<NoteData> notes;
        ImageBufferCollectHugeAdaptive(
            events, midi, notes, prepareChunk, timeSpan, corruptionMargin, tickMode, rows,
            corruption <= 0.0f, maxEndTime, maxEndTick, prefixEndTime, prefixEndTick,
            buildNote, isHidden);
        ImageBufferHugePreparedPublish(
            owner, signature, prepareChunk, std::move(notes), cpuDone, cpuTotal);
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        if (s_ImageBufferPrewarmGpu.owner == owner && s_ImageBufferPrewarmGpu.hugeMode &&
            s_ImageBufferPrewarmGpu.hugeSignature == signature) {
            s_ImageBufferPrewarmGpu.cpuPrepared = cpuDone;
            s_ImageBufferPrewarmGpu.cpuTotal = cpuTotal;
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        if (s_ImageBufferPrewarmGpu.owner == owner && s_ImageBufferPrewarmGpu.hugeMode &&
            s_ImageBufferPrewarmGpu.hugeSignature == signature) {
            s_ImageBufferPrewarmGpu.cpuPrepared = cpuTotal;
            s_ImageBufferPrewarmGpu.cpuTotal = cpuTotal;
        }
    }

    std::vector<long long> pinnedChunks;
    {
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        pinnedChunks = s_ImageBufferPrewarmGpu.chunks;
    }
    ImageBufferSetPinnedChunks(renderer, pinnedChunks);

    long long bakeChunk = Renderer::ImageBufferInvalidChunk;
    {
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        s_ImageBufferPrewarmGpu.cached = CountImageBufferPrewarmCached(
            renderer, s_ImageBufferPrewarmGpu.chunks);
        for (long long chunk : s_ImageBufferPrewarmGpu.chunks) {
            if (!renderer->ImageBufferChunkCached(chunk)) {
                bakeChunk = chunk;
                break;
            }
        }
    }
    if (bakeChunk == Renderer::ImageBufferInvalidChunk)
        return;

    if (auto ready = ImageBufferHugePreparedGet(owner, bakeChunk)) {
        renderer->ImageBufferRenderChunk(
            bakeChunk, ready->empty() ? nullptr : ready->data(), (unsigned)ready->size());
    } else {
        // Defensive fallback: selected GPU chunks should already be CPU-prepared.
        std::vector<NoteData> notes;
        ImageBufferCollectHugeAdaptive(
            events, midi, notes, bakeChunk, timeSpan, corruptionMargin, tickMode, rows,
            corruption <= 0.0f, maxEndTime, maxEndTick, prefixEndTime, prefixEndTick,
            buildNote, isHidden);
        renderer->ImageBufferRenderChunk(
            bakeChunk, notes.empty() ? nullptr : notes.data(), (unsigned)notes.size());
    }

    std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
    s_ImageBufferPrewarmGpu.cached = CountImageBufferPrewarmCached(
        renderer, s_ImageBufferPrewarmGpu.chunks);
}


// Dense chunks use a separate CPU preparation path. Sparse chunks keep the exact overlap
// collector and existing multipass behavior. Prepared chunks are built from an immutable
#define CollectChunk(k) ([&]() { \
    const bool imageBufferPreparedAllowed = ImageBufferFullPreparedSupported(m_vEvents, m_MIDI); \
    auto ImageBufferExactCollector = [&](long long imageBufferChunk) { \
        if (imageBufferPreparedAllowed) { \
            ImageBufferOverlapCollect(this, m_vEvents, m_MIDI, chunkNotes, imageBufferChunk, T, E, bTickMode, \
                [&](MIDIChannelEvent imageBufferNote, long long imageBufferChunkStart) { \
                    return BuildChunkNoteData(imageBufferNote, imageBufferChunkStart); \
                }); \
        } else { \
            const int imageBufferHugeRows = (int)std::ceil(std::fabs(notesCY)); \
            if (auto imageBufferHugeReady = ImageBufferHugePreparedGet(this, imageBufferChunk)) { \
                chunkNotes.assign(imageBufferHugeReady->begin(), imageBufferHugeReady->end()); \
            } else { \
                ImageBufferCollectHugeAdaptive(m_vEvents, m_MIDI, chunkNotes, imageBufferChunk, T, E, bTickMode, \
                    imageBufferHugeRows, fCorrupt <= 0.0f, m_vImageBufferMaxEndTime, m_vImageBufferMaxEndTick, \
                    m_vImageBufferPrefixEndTime, m_vImageBufferPrefixEndTick, \
                    [&](MIDIChannelEvent imageBufferNote, long long imageBufferChunkStart) { \
                        return BuildChunkNoteData(imageBufferNote, imageBufferChunkStart); \
                    }, \
                    [&](uint16_t imageBufferTrack, uint8_t imageBufferChannel) { \
                        return imageBufferTrack < m_vTrackSettings.size() && \
                            m_vTrackSettings[imageBufferTrack].aChannels[imageBufferChannel].bHidden; \
                    }); \
            } \
        } \
    }; \
    bool imageBufferAnyHidden = false; \
    for (const auto& imageBufferTrack : m_vTrackSettings) { \
        for (int imageBufferChannel = 0; imageBufferChannel < 16; ++imageBufferChannel) { \
            if (imageBufferTrack.aChannels[imageBufferChannel].bHidden) { \
                imageBufferAnyHidden = true; \
                break; \
            } \
        } \
        if (imageBufferAnyHidden) break; \
    } \
    if (imageBufferAnyHidden || !imageBufferPreparedAllowed) \
        ImageBufferPreparedGet().ClearPending(m_pRenderer, (k)); \
    if ((imageBufferAnyHidden || !imageBufferPreparedAllowed) && (k) == kFirst) \
        ImageBufferPreparedMarkPrewarmUnavailable(this); \
    const int imageBufferPrepRows = (int)std::ceil(std::fabs(notesCY)); \
    const bool imageBufferPreparedHandled = imageBufferPreparedAllowed && !imageBufferAnyHidden && ImageBufferPreparedTryCollect( \
        this, m_pRenderer, m_vEvents, m_MIDI, chunkNotes, (k), \
        kFirst, kLast, kMax, T, E, bTickMode, fCorrupt, \
        m_vTrackSettings.size(), imageBufferPrepRows); \
    if ((k) == kFirst) \
        UpdateImageBufferPrewarmGpuProgress(this, m_pRenderer, T, E, bTickMode, tStart); \
    if (!imageBufferPreparedHandled) \
        ImageBufferMPCollectDispatch(m_pRenderer, chunkNotes, ImageBufferExactCollector, (k)); \
}())

#define ClearAndBeginScene(...) ClearAndBeginScene(__VA_ARGS__); \
([&](auto* imageBufferPrewarmSelf) { \
    using ImageBufferPrewarmSelfT = std::remove_pointer_t<decltype(imageBufferPrewarmSelf)>; \
    if constexpr (std::is_same_v<ImageBufferPrewarmSelfT, MainScreen>) { \
        const bool imageBufferPrewarmActive = !imageBufferPrewarmSelf->m_bDiscarded && \
            ImageBufferPreparedGetWaitBeforePlayback() && \
            Config::GetConfig().GetVizSettings().bImageBufferNotes && \
            !g_bVideoRendering && \
            ImageBufferPrewarmPlayRequestedFor(imageBufferPrewarmSelf); \
        if (imageBufferPrewarmActive) { \
            if (imageBufferPrewarmSelf->m_bImageBufferNeedsInvalidate) { \
                ImageBufferHugePreparedClear(imageBufferPrewarmSelf); \
                imageBufferPrewarmSelf->m_pRenderer->ImageBufferInvalidate(); \
                imageBufferPrewarmSelf->m_bImageBufferNeedsInvalidate = false; \
            } \
            imageBufferPrewarmSelf->m_pRenderer->ImageBufferSetEventCount( \
                (unsigned long long)imageBufferPrewarmSelf->m_vEvents.size()); \
            if (!imageBufferPrewarmSelf->m_pRenderer->ImageBufferCanRender()) { \
                ImageBufferPreparedMarkPrewarmUnavailable(imageBufferPrewarmSelf); \
                UpdateImageBufferPrewarmGpuProgress( \
                    imageBufferPrewarmSelf, imageBufferPrewarmSelf->m_pRenderer, 1, 0, false, 0); \
            } else { \
                bool imageBufferPrewarmAnyHidden = false; \
                for (const auto& imageBufferPrewarmTrack : imageBufferPrewarmSelf->m_vTrackSettings) { \
                    for (int imageBufferPrewarmChannel = 0; imageBufferPrewarmChannel < 16; ++imageBufferPrewarmChannel) { \
                        if (imageBufferPrewarmTrack.aChannels[imageBufferPrewarmChannel].bHidden) { \
                            imageBufferPrewarmAnyHidden = true; \
                            break; \
                        } \
                    } \
                    if (imageBufferPrewarmAnyHidden) break; \
                } \
                if (imageBufferPrewarmAnyHidden && ImageBufferFullPreparedSupported( \
                    imageBufferPrewarmSelf->m_vEvents, imageBufferPrewarmSelf->m_MIDI)) { \
                    ImageBufferPreparedMarkPrewarmUnavailable(imageBufferPrewarmSelf); \
                } else { \
                    const long long imageBufferPrewarmT = imageBufferPrewarmSelf->m_llTimeSpan; \
                    if (imageBufferPrewarmT > 0 && !imageBufferPrewarmSelf->m_vEvents.empty()) { \
                        const bool imageBufferPrewarmTickMode = imageBufferPrewarmSelf->m_bTickMode; \
                        const float imageBufferPrewarmCorrupt = imageBufferPrewarmSelf->GetCorruptorAmount(); \
                        const long long imageBufferPrewarmE = 1 + (long long)std::ceil( \
                            (double)imageBufferPrewarmT * 0.10 * (double)imageBufferPrewarmCorrupt); \
                        int imageBufferPrewarmRows = (int)std::ceil(std::fabs(imageBufferPrewarmSelf->m_fNotesCY)); \
                        imageBufferPrewarmRows = (std::min)((std::max)(imageBufferPrewarmRows, 64), \
                            ImageBufferPreparedMaxRows); \
                        if (!ImageBufferFullPreparedSupported( \
                            imageBufferPrewarmSelf->m_vEvents, imageBufferPrewarmSelf->m_MIDI)) { \
                            ImageBufferLogFullPrewarmSkip(imageBufferPrewarmSelf, \
                                imageBufferPrewarmSelf->m_vEvents.size(), \
                                imageBufferPrewarmSelf->m_MIDI.GetInfo().iNoteCount); \
                            ImageBufferDriveHugePrewarm( \
                                imageBufferPrewarmSelf, imageBufferPrewarmSelf->m_pRenderer, \
                                imageBufferPrewarmSelf->m_vEvents, imageBufferPrewarmSelf->m_MIDI, \
                                imageBufferPrewarmT, imageBufferPrewarmE, imageBufferPrewarmTickMode, \
                                imageBufferPrewarmCorrupt, imageBufferPrewarmRows, \
                                imageBufferPrewarmSelf->m_vImageBufferMaxEndTime, \
                                imageBufferPrewarmSelf->m_vImageBufferMaxEndTick, \
                                imageBufferPrewarmSelf->m_vImageBufferPrefixEndTime, \
                                imageBufferPrewarmSelf->m_vImageBufferPrefixEndTick, \
                                [&](MIDIChannelEvent imageBufferNote, long long imageBufferChunkStart) { \
                                    return imageBufferPrewarmSelf->BuildChunkNoteData( \
                                        imageBufferNote, imageBufferChunkStart); \
                                }, \
                                [&](uint16_t imageBufferTrack, uint8_t imageBufferChannel) { \
                                    return imageBufferTrack < imageBufferPrewarmSelf->m_vTrackSettings.size() && \
                                        imageBufferPrewarmSelf->m_vTrackSettings[imageBufferTrack] \
                                            .aChannels[imageBufferChannel].bHidden; \
                                }); \
                        } else { \
                            auto& imageBufferPrewarmOverlap = ImageBufferOverlapEnsureIndex( \
                                imageBufferPrewarmSelf, imageBufferPrewarmSelf->m_vEvents, imageBufferPrewarmSelf->m_MIDI); \
                            const auto imageBufferPrewarmSource = imageBufferPrewarmOverlap.preparedSource; \
                            if (!imageBufferPrewarmSource || imageBufferPrewarmSource->notes.empty() || \
                                imageBufferPrewarmCorrupt > 1.0f || \
                                (imageBufferPrewarmTickMode ? imageBufferPrewarmSource->tickOverflow : imageBufferPrewarmSource->timeOverflow)) { \
                                ImageBufferPreparedMarkPrewarmUnavailable(imageBufferPrewarmSelf); \
                            } else { \
                                const uint64_t imageBufferPrewarmSignature = ImageBufferPreparedSignature( \
                                    imageBufferPrewarmSource.get(), imageBufferPrewarmT, imageBufferPrewarmTickMode, \
                                    imageBufferPrewarmCorrupt, imageBufferPrewarmSelf->m_vTrackSettings.size(), imageBufferPrewarmRows); \
                                auto& imageBufferPrewarmManager = ImageBufferPreparedGet(); \
                                imageBufferPrewarmManager.Activate(imageBufferPrewarmSource.get(), imageBufferPrewarmSignature); \
                                ImageBufferPreparedPrimeAllDense( \
                                    imageBufferPrewarmSelf, imageBufferPrewarmSource, imageBufferPrewarmT, imageBufferPrewarmE, \
                                    imageBufferPrewarmTickMode, imageBufferPrewarmCorrupt, \
                                    imageBufferPrewarmSelf->m_vTrackSettings.size(), imageBufferPrewarmRows, \
                                    imageBufferPrewarmSignature); \
                                UpdateImageBufferPrewarmGpuProgress( \
                                    imageBufferPrewarmSelf, imageBufferPrewarmSelf->m_pRenderer, \
                                    imageBufferPrewarmT, imageBufferPrewarmE, imageBufferPrewarmTickMode, imageBufferPrewarmSelf->m_llRndStartTime); \
                                long long imageBufferPrewarmBakeChunk = Renderer::ImageBufferInvalidChunk; \
                                { \
                                    std::lock_guard<std::mutex> imageBufferPrewarmLock(s_ImageBufferPrewarmGpuMutex); \
                                    if (s_ImageBufferPrewarmGpu.owner == imageBufferPrewarmSelf && \
                                        s_ImageBufferPrewarmGpu.cacheRequired) { \
                                        s_ImageBufferPrewarmGpu.cached = CountImageBufferPrewarmCached( \
                                            imageBufferPrewarmSelf->m_pRenderer, s_ImageBufferPrewarmGpu.chunks); \
                                        for (long long imageBufferPrewarmChunk : s_ImageBufferPrewarmGpu.chunks) { \
                                            if (!imageBufferPrewarmSelf->m_pRenderer->ImageBufferChunkCached(imageBufferPrewarmChunk)) { \
                                                imageBufferPrewarmBakeChunk = imageBufferPrewarmChunk; \
                                                break; \
                                            } \
                                        } \
                                    } \
                                } \
                                if (imageBufferPrewarmBakeChunk != Renderer::ImageBufferInvalidChunk) { \
                                    const ImageBufferPreparedKey imageBufferPrewarmBakeKey{ \
                                        imageBufferPrewarmSource.get(), imageBufferPrewarmBakeChunk, imageBufferPrewarmSignature }; \
                                    if (auto imageBufferPrewarmReady = imageBufferPrewarmManager.Ready(imageBufferPrewarmBakeKey)) { \
                                        imageBufferPrewarmSelf->m_pRenderer->ImageBufferRenderChunk( \
                                            imageBufferPrewarmBakeChunk, imageBufferPrewarmReady->data(), \
                                            (unsigned)imageBufferPrewarmReady->size()); \
                                    } \
                                } \
                            } \
                        } \
                    } \
                } \
            } \
        } \
    } \
}(this))
#define EndText(...) EndText(__VA_ARGS__); DrawImageBufferPrewarmProgressLate(m_pRenderer); DrawFrameTimeGraphLate(m_pRenderer)
// Compile the original game-state implementation under private legacy method names.
// PlaybackAudioThread.inc supplies the public wrappers so visual logic can remain frame-driven
#define Logic LogicLegacy
#define MsgProc MsgProcLegacy
#define Discard DiscardLegacy
#include "GameStateLegacy.inc"
#undef Discard
#undef MsgProc
#undef Logic
#undef EndText
#undef ClearAndBeginScene
#undef CollectChunk

#include "PlaybackAudioThread.inc"

static void DrawFrameTimeGraphLate(Renderer* renderer)
{
    using Clock = std::chrono::steady_clock;
    static Clock::time_point s_last;
    static std::deque<float> s_history;
    static float s_rangeMs = 33.333f;

    const Clock::time_point now = Clock::now();
    float frameMs = 0.0f;
    if (s_last.time_since_epoch().count() != 0)
        frameMs = (float)std::chrono::duration<double, std::milli>(now - s_last).count();
    s_last = now;

    if (!renderer)
        return;

    const VizSettings& viz = Config::GetConfig().GetVizSettings();
    if (!viz.bSysStats || g_bVideoRendering)
        return;

    if (Config::GetConfig().GetPlaybackSettings().GetPlayMode() == GameState::Splash)
        return;

    const MainScreen* statsScreen = dynamic_cast<const MainScreen*>(g_pGameState);
    const float bounceScale = statsScreen ? statsScreen->GetStatsBounceScaleForOverlay() : 1.0f;

    if (frameMs > 0.05f && frameMs < 5000.0f) {
        s_history.push_back(frameMs);
        if (s_history.size() > 600)
            s_history.pop_front();
    }

    ImDrawList* dl = renderer->GetDrawList();
    if (!dl)
        return;
    const int frameTimeVtxStart = dl->VtxBuffer.Size;

    const float scale = (std::max)(viz.fUIScale, 0.5f);
    const float bh = (float)renderer->GetBufferHeight();
    const float graphH = 64.0f * scale;
    const float panelW = 250.0f * scale;
    const float textH = (6.0f + 16.0f * 4.0f) * scale;

    const float contentTop = ImGui::GetFrameHeight() + 35.0f;
    float toolbarBottom = contentTop + 10.0f;
    if (viz.bDualPianoRoll) {
        const float stripH = (std::max)(190.0f,
            (std::min)(bh * 0.45f, bh * 0.28f));
        toolbarBottom = 20.0f + 35.0f + stripH + 10.0f;
    }

    const float sysStatsH = textH + graphH + 10.0f * scale;
    const float panelLeft = 10.0f;
    const float panelTop = toolbarBottom + sysStatsH + 10.0f * scale;
    const float panelRight = panelLeft + panelW;
    const float panelBottom = panelTop + graphH + 26.0f * scale;
    DrawBlurPanel(renderer, panelLeft, panelTop, panelRight, panelBottom, 10.0f * scale);

    const ImVec2 g0(panelLeft + 6.0f * scale, panelTop + 20.0f * scale);
    const ImVec2 g1(panelRight - 6.0f * scale, g0.y + graphH);
    dl->AddRectFilled(g0, g1, 0x30000000);
    for (int i = 0; i <= 4; ++i) {
        const float y = g0.y + (g1.y - g0.y) * (float)i / 4.0f;
        dl->AddLine(ImVec2(g0.x, y), ImVec2(g1.x, y), 0x20FFFFFF);
    }

    float maxSeen = 0.0f;
    for (float ms : s_history)
        maxSeen = (std::max)(maxSeen, ms);
    const float targetRange = (std::max)(33.333f,
        (float)std::ceil((double)maxSeen / 5.0) * 5.0f);
    if (targetRange > s_rangeMs)
        s_rangeMs += (targetRange - s_rangeMs) * 0.35f;
    else
        s_rangeMs += (targetRange - s_rangeMs) * 0.10f;
    s_rangeMs = (std::max)(s_rangeMs, 33.333f);

    auto drawReference = [&](float ms, ImU32 color) {
        if (ms > s_rangeMs)
            return;
        const float y = g1.y - ms / s_rangeMs * (g1.y - g0.y);
        dl->AddLine(ImVec2(g0.x, y), ImVec2(g1.x, y), color);
    };
    drawReference(16.6667f, IM_COL32(255, 255, 255, 60));
    drawReference(33.3333f, IM_COL32(255, 255, 255, 38));

    const size_t n = s_history.size();
    if (n > 0) {
        const float step = (g1.x - g0.x) / 600.0f;
        ImVec2 pts[600];
        for (size_t i = 0; i < n; ++i) {
            const float x = g1.x - (float)(n - i) * step;
            const float ms = (std::min)(s_history[i], s_rangeMs);
            pts[i] = ImVec2(x, g1.y - ms / s_rangeMs * (g1.y - g0.y));
        }
        for (size_t i = 0; i + 1 < n; ++i)
            dl->AddRectFilled(ImVec2(pts[i].x, pts[i].y),
                ImVec2(pts[i + 1].x, g1.y), IM_COL32(255, 255, 255, 36));
        dl->AddRectFilled(ImVec2(pts[n - 1].x, pts[n - 1].y),
            ImVec2(g1.x, g1.y), IM_COL32(255, 255, 255, 36));
        dl->AddPolyline(pts, (int)n, IM_COL32(255, 255, 255, 220), 0, 2.0f * scale);
    }

    const float currentMs = n > 0 ? s_history.back() : 0.0f;
    const float currentFps = currentMs > 0.001f ? 1000.0f / currentMs : 0.0f;
    char label[96];
    snprintf(label, sizeof(label) - 1, "Frame %.1f ms / %.0f FPS", currentMs, currentFps);
    dl->AddText(ImVec2(panelLeft + 6.0f * scale, panelTop + 3.0f * scale),
        0xFF9A9A9A, label);

    char rangeLabel[48];
    snprintf(rangeLabel, sizeof(rangeLabel) - 1, "max %.0f ms", s_rangeMs);
    dl->AddText(ImVec2(g1.x - ImGui::CalcTextSize(rangeLabel).x,
        panelTop + 3.0f * scale), 0xFF9A9A9A, rangeLabel);

    // Match Sys Stats exactly: same yaw, same beat scale, same perspective.
    const int frameTimeVtxEnd = dl->VtxBuffer.Size;
    const ImVec2 frameTimeCenter((panelLeft + panelRight) * 0.5f,
        (panelTop + panelBottom) * 0.5f);
    Apply3DTilt(dl, frameTimeVtxStart, frameTimeVtxEnd, frameTimeCenter,
        0.20f, bounceScale, 750.0f);
}

VOID ImageBufferPrewarmRendererSeen(Renderer* pRenderer)
{
    if (!pRenderer)
        return;
    std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
    s_ImageBufferPrewarmGpu.renderer = pRenderer;
}

void PFASetWaitDenseBuffers(bool enabled)
{
    ImageBufferPreparedSetWaitBeforePlayback(enabled);
}

// Gate-state transition logging: fires once per state CHANGE so the heartbeat
// log shows exactly why playback was held or released without flooding it.
static void TrackGateState(int code, const char* msg)
{
    static std::atomic<int> s_lastGateState{ -1 };
    if (s_lastGateState.exchange(code, std::memory_order_relaxed) == code)
        return;
    HeartbeatLog(msg);
}

VOID ImageBufferPrewarmPlaybackRequested(BOOL bPlaying)
{
    if (!bPlaying) {
        {
            std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
            s_ImageBufferPrewarmGpu.playRequested = false;
            ImageBufferClearPinnedChunks(s_ImageBufferPrewarmGpu.renderer);
        }
        ImageBufferHugePreparedClear();
        ImageBufferPreparedCancelPlaybackGate();
        TrackGateState(11, "prewarm:canceled(stop)");
        return;
    }

    const auto& viz = Config::GetConfig().GetVizSettings();
    if (!viz.bImageBufferNotes || !ImageBufferPreparedGetWaitBeforePlayback() || g_bVideoRendering) {
        {
            std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
            s_ImageBufferPrewarmGpu.playRequested = false;
            ImageBufferClearPinnedChunks(s_ImageBufferPrewarmGpu.renderer);
        }
        ImageBufferPreparedCancelPlaybackGate();
        TrackGateState(1, "prewarm:off(settings)");
        return;
    }

    const MainScreen* screen = dynamic_cast<const MainScreen*>(g_pGameState);
    const void* owner = nullptr;
    if (screen && !screen->IsFreePlay() && screen->IsValid() && !screen->IsDiscarded())
        owner = screen;

    const ImageBufferPreparedProgress cpu = ImageBufferPreparedGetFullProgress();
    {
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        const bool ownerChanged = s_ImageBufferPrewarmGpu.owner != owner;
        if (ownerChanged) {
            s_ImageBufferPrewarmGpu.owner = owner;
            s_ImageBufferPrewarmGpu.initialized = false;
            s_ImageBufferPrewarmGpu.cacheRequired = false;
            s_ImageBufferPrewarmGpu.hugeMode = false;
            s_ImageBufferPrewarmGpu.hugeSignature = 0;
            s_ImageBufferPrewarmGpu.cached = 0;
            s_ImageBufferPrewarmGpu.total = 0;
            s_ImageBufferPrewarmGpu.cpuPrepared = 0;
            s_ImageBufferPrewarmGpu.cpuTotal = 0;
            s_ImageBufferPrewarmGpu.chunks.clear();
        }
        s_ImageBufferPrewarmGpu.playRequested = true;
    }
    {
        char b[128];
        sprintf_s(b, "prewarm:armed owner=%p cpuInit=%d unsup=%d",
            owner, (int)cpu.initialized, (int)cpu.unsupported);
        TrackGateState(10, b);
    }
    ImageBufferPreparedArmPlaybackGate(owner);
}

// Re-runs dense generation after a seek/skip (bInvalidateData=FALSE: prepared
// chunks stay valid and are reused) or after a note-speed change
// (bInvalidateData=TRUE: visible time span changed -> everything regenerates).
// Playback holds via GetPaused() until the rebuild finishes, with the usual
// progress bar.
VOID ImageBufferPrewarmRestartAfterSeek(BOOL bInvalidateData)
{
    const auto& viz = Config::GetConfig().GetVizSettings();
    if (!viz.bImageBufferNotes || !ImageBufferPreparedGetWaitBeforePlayback() || g_bVideoRendering)
        return;
    MainScreen* screen = dynamic_cast<MainScreen*>(g_pGameState);
    if (!screen || screen->IsFreePlay() || !screen->IsValid() || screen->IsDiscarded())
        return;

    {
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        s_ImageBufferPrewarmGpu.owner = screen;
        s_ImageBufferPrewarmGpu.initialized = false;
        s_ImageBufferPrewarmGpu.cacheRequired = false;
        s_ImageBufferPrewarmGpu.hugeMode = false;
        s_ImageBufferPrewarmGpu.cached = 0;
        s_ImageBufferPrewarmGpu.total = 0;
        s_ImageBufferPrewarmGpu.cpuPrepared = 0;
        s_ImageBufferPrewarmGpu.cpuTotal = 0;
        s_ImageBufferPrewarmGpu.chunks.clear();
        s_ImageBufferPrewarmGpu.restartKeepReady = !bInvalidateData;
        s_ImageBufferPrewarmGpu.playRequested = true;
    }
    if (bInvalidateData)
        screen->RequestDenseRegeneration();
    ImageBufferPreparedArmPlaybackGate(screen);
    TrackGateState(12, bInvalidateData ? "prewarm:rearmed(regenerate)" : "prewarm:rearmed(seek)");
}

VOID ImageBufferPrewarmNotesSpeedChanged()
{
    // Note speed changes the visible time span, so every prepared signature is
    // invalid. Skipped while paused: scrubbing must not thrash preparation, and
    // the next Play re-arms fresh regardless.
    if (!Config::GetConfig().GetPlaybackSettings().GetPausedRaw())
        ImageBufferPrewarmRestartAfterSeek(TRUE);
}

BOOL ImageBufferPrewarmPlaybackHold()
{
    if (!ImageBufferPreparedGetWaitBeforePlayback()) {
        {
            std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
            s_ImageBufferPrewarmGpu.playRequested = false;
        }
        ImageBufferPreparedCancelPlaybackGate();
        TrackGateState(1, "prewarm:off(setting)");
        return FALSE;
    }

    const auto& viz = Config::GetConfig().GetVizSettings();
    if (!viz.bImageBufferNotes || g_bVideoRendering) {
        {
            std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
            s_ImageBufferPrewarmGpu.playRequested = false;
        }
        ImageBufferPreparedCancelPlaybackGate();
        TrackGateState(1, "prewarm:off(viz)");
        return FALSE;
    }

    const MainScreen* screen = dynamic_cast<const MainScreen*>(g_pGameState);
    if (!screen || screen->IsFreePlay() || !screen->IsValid() || screen->IsDiscarded()) {
        TrackGateState(2, "prewarm:idle(no active screen)");
        return FALSE;
    }

    {
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        if (!s_ImageBufferPrewarmGpu.playRequested) {
            TrackGateState(0, "prewarm:idle(not requested)");
            return FALSE;
        }
        if (!s_ImageBufferPrewarmGpu.owner) {
            s_ImageBufferPrewarmGpu.owner = screen;
        } else if (s_ImageBufferPrewarmGpu.owner != screen) {
            // Stale ownership (previous song's screen): adopt the new one and
            // start the preparation from scratch instead of dead-gating here.
            char b[96];
            sprintf_s(b, "prewarm:stale-owner-adopted %p->%p",
                s_ImageBufferPrewarmGpu.owner, (const void*)screen);
            s_ImageBufferPrewarmGpu.owner = screen;
            s_ImageBufferPrewarmGpu.initialized = false;
            s_ImageBufferPrewarmGpu.cacheRequired = false;
            s_ImageBufferPrewarmGpu.hugeMode = false;
            s_ImageBufferPrewarmGpu.hugeSignature = 0;
            s_ImageBufferPrewarmGpu.cached = 0;
            s_ImageBufferPrewarmGpu.total = 0;
            s_ImageBufferPrewarmGpu.cpuPrepared = 0;
            s_ImageBufferPrewarmGpu.cpuTotal = 0;
            s_ImageBufferPrewarmGpu.chunks.clear();
            TrackGateState(9, b);
        }
    }

    if (ImageBufferPreparedShouldHoldPlayback(screen)) {
        TrackGateState(4, "prewarm:hold(cpu-prime)");
        return TRUE;
    }

    {
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        // CPU preparation may have completed between Logic and the previous render. Hold one more frame
        // until the render path publishes whether a texture-cache stage is required for this song.
        if (!s_ImageBufferPrewarmGpu.initialized) {
            TrackGateState(5, "prewarm:hold(uninitialized)");
            return TRUE;
        }
        if (s_ImageBufferPrewarmGpu.hugeMode &&
            s_ImageBufferPrewarmGpu.cpuPrepared < s_ImageBufferPrewarmGpu.cpuTotal) {
            char b[96];
            sprintf_s(b, "prewarm:hold(huge-cpu %zu/%zu)",
                s_ImageBufferPrewarmGpu.cpuPrepared, s_ImageBufferPrewarmGpu.cpuTotal);
            TrackGateState(6, b);
            return TRUE;
        }
        if (s_ImageBufferPrewarmGpu.cacheRequired) {
            s_ImageBufferPrewarmGpu.cached = CountImageBufferPrewarmCached(
                s_ImageBufferPrewarmGpu.renderer, s_ImageBufferPrewarmGpu.chunks);
            if (s_ImageBufferPrewarmGpu.cached < s_ImageBufferPrewarmGpu.total) {
                char b[96];
                sprintf_s(b, "prewarm:hold(gpu-cache %zu/%zu)",
                    s_ImageBufferPrewarmGpu.cached, s_ImageBufferPrewarmGpu.total);
                TrackGateState(7, b);
                return TRUE;
            }
        }
        s_ImageBufferPrewarmGpu.playRequested = false;
    }
    TrackGateState(8, "prewarm:released");
    return FALSE;
}
