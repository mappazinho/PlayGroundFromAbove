from pathlib import Path
import re


def sub_once(path, pattern, repl, label, flags=0):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    new, n = re.subn(pattern, repl, text, count=1, flags=flags)
    if n != 1:
        raise RuntimeError(f"{label}: expected 1 match, got {n}")
    p.write_text(new, encoding="utf-8")


def replace_once(path, old, new, label):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    n = text.count(old)
    if n != 1:
        raise RuntimeError(f"{label}: expected 1 match, got {n}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "PianoFromAbove/GameState.cpp",
    "    bool cacheRequired = false;\n    bool playRequested = false;",
    "    bool cacheRequired = false;\n    bool hugeMode = false;\n    uint64_t hugeSignature = 0;\n    bool playRequested = false;",
    "prewarm huge state")

sub_once(
    "PianoFromAbove/GameState.cpp",
    r"static constexpr size_t ImageBufferHugeBlockEvents = 4096;\n.*?\nstatic size_t CountImageBufferPrewarmCached\(",
    r'''static constexpr size_t ImageBufferHugeBlockEvents = 4096;

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

    size_t remaining = (size_t)128 * (size_t)rows;
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

                NoteData data = buildNote(event, chunkStart);
                if (data.key >= 128 || isHidden(data.track, data.channel))
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
    ImageBufferCollectHugeIndexedCompact(
        events, midi, out, chunk, timeSpan, corruptionMargin, tickMode, rows,
        maxEndTime, maxEndTick, prefixEndTime, prefixEndTick,
        std::forward<BuildFn>(buildNote), std::forward<HiddenFn>(isHidden));
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

static size_t CountImageBufferPrewarmCached(''',
    "replace huge collectors",
    flags=re.S)

sub_once(
    "PianoFromAbove/GameState.cpp",
    r'''    if \(cpu\.unsupported\) \{\n        ImageBufferClearPinnedChunks\(renderer\);\n        std::lock_guard<std::mutex> lock\(s_ImageBufferPrewarmGpuMutex\);\n        s_ImageBufferPrewarmGpu\.owner = owner;\n        s_ImageBufferPrewarmGpu\.renderer = renderer;\n        s_ImageBufferPrewarmGpu\.initialized = true;\n        s_ImageBufferPrewarmGpu\.cacheRequired = false;\n        s_ImageBufferPrewarmGpu\.cached = 0;\n        s_ImageBufferPrewarmGpu\.total = 0;\n        s_ImageBufferPrewarmGpu\.chunks\.clear\(\);\n        return;\n    \}''',
    r'''    if (cpu.unsupported) {
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
    }''',
    "preserve huge prewarm")

replace_once(
    "PianoFromAbove/GameState.cpp",
    "    s_ImageBufferPrewarmGpu.cacheRequired = requireGpu && !prewarmChunks.empty();\n    s_ImageBufferPrewarmGpu.cached = cached;",
    "    s_ImageBufferPrewarmGpu.cacheRequired = requireGpu && !prewarmChunks.empty();\n    s_ImageBufferPrewarmGpu.hugeMode = false;\n    s_ImageBufferPrewarmGpu.hugeSignature = 0;\n    s_ImageBufferPrewarmGpu.cached = cached;",
    "normal prewarm clears huge mode")

replace_once(
    "PianoFromAbove/GameState.cpp",
    "    bool initializing = false;\n    size_t done = cpu.done;",
    "    bool initializing = false;\n    bool hugeMode = false;\n    size_t done = cpu.done;",
    "draw huge mode local")

replace_once(
    "PianoFromAbove/GameState.cpp",
    "        playRequested = s_ImageBufferPrewarmGpu.playRequested;\n        if (cpu.initialized && cpu.done >= cpu.total && s_ImageBufferPrewarmGpu.cacheRequired) {",
    "        playRequested = s_ImageBufferPrewarmGpu.playRequested;\n        hugeMode = s_ImageBufferPrewarmGpu.hugeMode;\n        if (s_ImageBufferPrewarmGpu.cacheRequired &&\n            (hugeMode || (cpu.initialized && cpu.done >= cpu.total))) {",
    "draw huge gpu condition")

replace_once(
    "PianoFromAbove/GameState.cpp",
    "    if (!cpu.initialized) {\n        initializing = true;",
    "    if (!cpu.initialized && !hugeMode) {\n        initializing = true;",
    "draw initializing huge")

replace_once(
    "PianoFromAbove/GameState.cpp",
    "    } else if (cpu.unsupported) {\n        return;\n    }",
    "    } else if (cpu.unsupported && !gpuStage) {\n        return;\n    }",
    "draw unsupported huge")

marker = '''static bool ImageBufferPrewarmPlayRequestedFor(const void* owner)
{
    std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
    return s_ImageBufferPrewarmGpu.playRequested &&
        (!s_ImageBufferPrewarmGpu.owner || s_ImageBufferPrewarmGpu.owner == owner);
}
'''
driver = marker + r'''
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
    {
        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        initialize = s_ImageBufferPrewarmGpu.owner != owner ||
            !s_ImageBufferPrewarmGpu.initialized ||
            !s_ImageBufferPrewarmGpu.hugeMode ||
            s_ImageBufferPrewarmGpu.hugeSignature != signature;
    }

    if (initialize) {
        static constexpr size_t reserveSlots = 8;
        const size_t limit = (size_t)Renderer::ChunkPoolSize - reserveSlots;
        std::vector<long long> chunks = ImageBufferSelectHugePrewarmChunks(
            events, midi, timeSpan, corruptionMargin, tickMode,
            maxEndTime, maxEndTick, limit);
        ImageBufferSetPinnedChunks(renderer, chunks);
        const size_t cached = CountImageBufferPrewarmCached(renderer, chunks);

        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
        const bool keepPlay = s_ImageBufferPrewarmGpu.playRequested;
        s_ImageBufferPrewarmGpu.owner = owner;
        s_ImageBufferPrewarmGpu.renderer = renderer;
        s_ImageBufferPrewarmGpu.initialized = true;
        s_ImageBufferPrewarmGpu.cacheRequired = !chunks.empty();
        s_ImageBufferPrewarmGpu.hugeMode = true;
        s_ImageBufferPrewarmGpu.hugeSignature = signature;
        s_ImageBufferPrewarmGpu.cached = cached;
        s_ImageBufferPrewarmGpu.total = chunks.size();
        s_ImageBufferPrewarmGpu.chunks = std::move(chunks);
        s_ImageBufferPrewarmGpu.playRequested = keepPlay;
    }

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

    std::vector<NoteData> notes;
    auto collector = [&](long long chunk) {
        ImageBufferCollectHugeAdaptive(
            events, midi, notes, chunk, timeSpan, corruptionMargin, tickMode, rows,
            maxEndTime, maxEndTick, prefixEndTime, prefixEndTick,
            buildNote, isHidden);
    };
    ImageBufferMPCollectDispatch(renderer, notes, collector, bakeChunk);
    renderer->ImageBufferRenderChunk(
        bakeChunk, notes.empty() ? nullptr : notes.data(), (unsigned)notes.size());

    std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
    s_ImageBufferPrewarmGpu.cached = CountImageBufferPrewarmCached(
        renderer, s_ImageBufferPrewarmGpu.chunks);
}
'''
replace_once("PianoFromAbove/GameState.cpp", marker, driver, "insert huge prewarm driver")

old_call = r'''            ImageBufferCollectHugeIndexed(m_vEvents, m_MIDI, chunkNotes, imageBufferChunk, T, E, bTickMode, \
                m_vImageBufferMaxEndTime, m_vImageBufferMaxEndTick, \
                m_vImageBufferPrefixEndTime, m_vImageBufferPrefixEndTick, \
                [&](MIDIChannelEvent imageBufferNote, long long imageBufferChunkStart) { \
                    return BuildChunkNoteData(imageBufferNote, imageBufferChunkStart); \
                }); \'''
new_call = r'''            const int imageBufferHugeRows = (int)std::ceil(std::fabs(notesCY)); \
            ImageBufferCollectHugeAdaptive(m_vEvents, m_MIDI, chunkNotes, imageBufferChunk, T, E, bTickMode, \
                imageBufferHugeRows, m_vImageBufferMaxEndTime, m_vImageBufferMaxEndTick, \
                m_vImageBufferPrefixEndTime, m_vImageBufferPrefixEndTick, \
                [&](MIDIChannelEvent imageBufferNote, long long imageBufferChunkStart) { \
                    return BuildChunkNoteData(imageBufferNote, imageBufferChunkStart); \
                }, \
                [&](uint16_t imageBufferTrack, uint8_t imageBufferChannel) { \
                    return imageBufferTrack < m_vTrackSettings.size() && \
                        m_vTrackSettings[imageBufferTrack].aChannels[imageBufferChannel].bHidden; \
                }); \'''
replace_once("PianoFromAbove/GameState.cpp", old_call, new_call, "vertical huge adaptive call")

old_unsupported = r'''                        if (!ImageBufferFullPreparedSupported( \
                            imageBufferPrewarmSelf->m_vEvents, imageBufferPrewarmSelf->m_MIDI)) { \
                            ImageBufferLogFullPrewarmSkip(imageBufferPrewarmSelf, \
                                imageBufferPrewarmSelf->m_vEvents.size(), \
                                imageBufferPrewarmSelf->m_MIDI.GetInfo().iNoteCount); \
                            ImageBufferPreparedMarkPrewarmUnavailable(imageBufferPrewarmSelf); \
                            UpdateImageBufferPrewarmGpuProgress( \
                                imageBufferPrewarmSelf, imageBufferPrewarmSelf->m_pRenderer, 1, 0, false); \
                        } else { \'''
new_unsupported = r'''                        if (!ImageBufferFullPreparedSupported( \
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
                        } else { \'''
replace_once("PianoFromAbove/GameState.cpp", old_unsupported, new_unsupported, "huge prewarm branch")

replace_once(
    "PianoFromAbove/GameState.cpp",
    "            s_ImageBufferPrewarmGpu.cacheRequired = false;\n            s_ImageBufferPrewarmGpu.cached = 0;",
    "            s_ImageBufferPrewarmGpu.cacheRequired = false;\n            s_ImageBufferPrewarmGpu.hugeMode = false;\n            s_ImageBufferPrewarmGpu.hugeSignature = 0;\n            s_ImageBufferPrewarmGpu.cached = 0;",
    "owner reset huge state")

p = Path("PianoFromAbove/GameStateLegacy.inc")
text = p.read_text(encoding="utf-8")
old = '''                ImageBufferCollectHugeIndexed(m_vEvents, m_MIDI, chunkNotes, chunk, T, margin, tickMode,
                    m_vImageBufferMaxEndTime, m_vImageBufferMaxEndTick,
                    m_vImageBufferPrefixEndTime, m_vImageBufferPrefixEndTick,
                    [&](MIDIChannelEvent note, long long chunkStart) { return BuildChunkNoteData(note, chunkStart); });'''
new = '''                ImageBufferCollectHugeIndexedExact(m_vEvents, m_MIDI, chunkNotes, chunk, T, margin, tickMode,
                    m_vImageBufferMaxEndTime, m_vImageBufferMaxEndTick,
                    m_vImageBufferPrefixEndTime, m_vImageBufferPrefixEndTick,
                    [&](MIDIChannelEvent note, long long chunkStart) { return BuildChunkNoteData(note, chunkStart); });'''
count = text.count(old)
if count != 1:
    raise RuntimeError(f"strip exact helper: expected 1 match, got {count}")
text = text.replace(old, new, 1)

old_sister = '''                const MIDIChannelEvent sister =
                    m_vEvents[m_MIDI.GetEventSisterIdx(event)];
                const long long lengthTime = (std::max)(0LL, (long long)m_MIDI.GetEventLength(event));'''
new_sister = '''                const unsigned sisterIndex = m_MIDI.GetEventSisterIdx(event);
                if (sisterIndex >= m_vEvents.size())
                    continue;
                const MIDIChannelEvent sister = m_vEvents[sisterIndex];
                const long long lengthTime = (std::max)(0LL, (long long)m_MIDI.GetEventLength(event));'''
if text.count(old_sister) != 1:
    raise RuntimeError("sister guard marker missing")
text = text.replace(old_sister, new_sister, 1)

old_discard = '''    m_dNPSNotes.clear();
    m_dNPSHistory.clear();
    m_vImageData.clear();'''
new_discard = '''    m_dNPSNotes.clear();
    m_dNPSHistory.clear();
    vector<long long>().swap(m_vImageBufferMaxEndTime);
    vector<long long>().swap(m_vImageBufferMaxEndTick);
    vector<long long>().swap(m_vImageBufferPrefixEndTime);
    vector<long long>().swap(m_vImageBufferPrefixEndTick);
    m_vImageData.clear();'''
if text.count(old_discard) != 1:
    raise RuntimeError("discard index marker missing")
text = text.replace(old_discard, new_discard, 1)
p.write_text(text, encoding="utf-8")

p = Path("PianoFromAbove/RendererBase.cpp")
text = p.read_text(encoding="utf-8")
old = 'ImGui::SetTooltip("Prepares every dense image-buffer chunk before a requested playback start.");'
new = 'ImGui::SetTooltip("Prepares dense image-buffer chunks before a requested playback start.");'
if text.count(old) != 1:
    raise RuntimeError("wait tooltip marker missing")
p.write_text(text.replace(old, new, 1), encoding="utf-8")
