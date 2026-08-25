from pathlib import Path

p = Path('PianoFromAbove/GameState.cpp')
s = p.read_text(encoding='utf-8')


def rep(old, new, label):
    global s
    n = s.count(old)
    if n != 1:
        raise RuntimeError(f'{label}: {n} matches')
    s = s.replace(old, new, 1)


rep('#include <deque>\n#include <chrono>\n',
    '#include <deque>\n#include <chrono>\n#include <unordered_map>\n',
    'unordered_map include')

rep('''    size_t cached = 0;
    size_t total = 0;
    std::vector<long long> chunks;
};

static std::mutex s_ImageBufferPrewarmGpuMutex;
static ImageBufferPrewarmGpuState s_ImageBufferPrewarmGpu;
''',
    '''    size_t cached = 0;
    size_t total = 0;
    size_t cpuPrepared = 0;
    size_t cpuTotal = 0;
    std::vector<long long> chunks;
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
    const void* owner, uint64_t signature, std::vector<long long> chunks)
{
    std::lock_guard<std::mutex> lock(s_ImageBufferHugePreparedMutex);
    s_ImageBufferHugePrepared.owner = owner;
    s_ImageBufferHugePrepared.signature = signature;
    s_ImageBufferHugePrepared.chunks = std::move(chunks);
    s_ImageBufferHugePrepared.next = 0;
    s_ImageBufferHugePrepared.ready.clear();
    s_ImageBufferHugePrepared.ready.reserve(s_ImageBufferHugePrepared.chunks.size());
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
''',
    'huge prepared state')

start = s.index('template <typename BuildFn, typename HiddenFn>\nstatic void ImageBufferDriveHugePrewarm(')
end = s.index('\n\n// Dense chunks use a separate CPU preparation path.', start)
new_fn = r'''template <typename BuildFn, typename HiddenFn>
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

        ImageBufferHugePreparedReset(owner, signature, std::move(cpuChunks));
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
'''
s = s[:start] + new_fn + s[end:]

rep('''            ImageBufferCollectHugeAdaptive(m_vEvents, m_MIDI, chunkNotes, imageBufferChunk, T, E, bTickMode, \\
                imageBufferHugeRows, fCorrupt <= 0.0f, m_vImageBufferMaxEndTime, m_vImageBufferMaxEndTick, \\
                m_vImageBufferPrefixEndTime, m_vImageBufferPrefixEndTick, \\
                [&](MIDIChannelEvent imageBufferNote, long long imageBufferChunkStart) { \\
                    return BuildChunkNoteData(imageBufferNote, imageBufferChunkStart); \\
                }, \\
                [&](uint16_t imageBufferTrack, uint8_t imageBufferChannel) { \\
                    return imageBufferTrack < m_vTrackSettings.size() && \\
                        m_vTrackSettings[imageBufferTrack].aChannels[imageBufferChannel].bHidden; \\
                }); \\
''',
    '''            if (auto imageBufferHugeReady = ImageBufferHugePreparedGet(this, imageBufferChunk)) { \\
                chunkNotes.assign(imageBufferHugeReady->begin(), imageBufferHugeReady->end()); \\
            } else { \\
                ImageBufferCollectHugeAdaptive(m_vEvents, m_MIDI, chunkNotes, imageBufferChunk, T, E, bTickMode, \\
                    imageBufferHugeRows, fCorrupt <= 0.0f, m_vImageBufferMaxEndTime, m_vImageBufferMaxEndTick, \\
                    m_vImageBufferPrefixEndTime, m_vImageBufferPrefixEndTick, \\
                    [&](MIDIChannelEvent imageBufferNote, long long imageBufferChunkStart) { \\
                        return BuildChunkNoteData(imageBufferNote, imageBufferChunkStart); \\
                    }, \\
                    [&](uint16_t imageBufferTrack, uint8_t imageBufferChannel) { \\
                        return imageBufferTrack < m_vTrackSettings.size() && \\
                            m_vTrackSettings[imageBufferTrack].aChannels[imageBufferChannel].bHidden; \\
                    }); \\
            } \\
''',
    'runtime huge prepared cache')

rep('''            if (imageBufferPrewarmSelf->m_bImageBufferNeedsInvalidate) { \\
                imageBufferPrewarmSelf->m_pRenderer->ImageBufferInvalidate(); \\
                imageBufferPrewarmSelf->m_bImageBufferNeedsInvalidate = false; \\
            } \\
''',
    '''            if (imageBufferPrewarmSelf->m_bImageBufferNeedsInvalidate) { \\
                ImageBufferHugePreparedClear(imageBufferPrewarmSelf); \\
                imageBufferPrewarmSelf->m_pRenderer->ImageBufferInvalidate(); \\
                imageBufferPrewarmSelf->m_bImageBufferNeedsInvalidate = false; \\
            } \\
''',
    'huge cache invalidation')

rep('''        if (s_ImageBufferPrewarmGpu.cacheRequired &&
            (hugeMode || (cpu.initialized && cpu.done >= cpu.total))) {
            gpuStage = true;
            done = s_ImageBufferPrewarmGpu.cached;
            total = s_ImageBufferPrewarmGpu.total;
            failed = 0;
        }
''',
    '''        if (hugeMode && s_ImageBufferPrewarmGpu.cpuPrepared < s_ImageBufferPrewarmGpu.cpuTotal) {
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
''',
    'progress CPU stage')

rep('''            s_ImageBufferPrewarmGpu.cached = 0;
            s_ImageBufferPrewarmGpu.total = 0;
            s_ImageBufferPrewarmGpu.chunks.clear();
''',
    '''            s_ImageBufferPrewarmGpu.cached = 0;
            s_ImageBufferPrewarmGpu.total = 0;
            s_ImageBufferPrewarmGpu.cpuPrepared = 0;
            s_ImageBufferPrewarmGpu.cpuTotal = 0;
            s_ImageBufferPrewarmGpu.chunks.clear();
''',
    'owner reset CPU progress')

rep('''VOID ImageBufferPrewarmPlaybackRequested(BOOL bPlaying)
{
    if (!bPlaying) {
        {
            std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
            s_ImageBufferPrewarmGpu.playRequested = false;
            ImageBufferClearPinnedChunks(s_ImageBufferPrewarmGpu.renderer);
        }
        ImageBufferPreparedCancelPlaybackGate();
        return;
    }
''',
    '''VOID ImageBufferPrewarmPlaybackRequested(BOOL bPlaying)
{
    if (!bPlaying) {
        {
            std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);
            s_ImageBufferPrewarmGpu.playRequested = false;
            ImageBufferClearPinnedChunks(s_ImageBufferPrewarmGpu.renderer);
        }
        ImageBufferHugePreparedClear();
        ImageBufferPreparedCancelPlaybackGate();
        return;
    }
''',
    'stop clears huge CPU cache')

rep('''        if (s_ImageBufferPrewarmGpu.cacheRequired) {
            s_ImageBufferPrewarmGpu.cached = CountImageBufferPrewarmCached(
                s_ImageBufferPrewarmGpu.renderer, s_ImageBufferPrewarmGpu.chunks);
            if (s_ImageBufferPrewarmGpu.cached < s_ImageBufferPrewarmGpu.total)
                return TRUE;
        }
''',
    '''        if (s_ImageBufferPrewarmGpu.hugeMode &&
            s_ImageBufferPrewarmGpu.cpuPrepared < s_ImageBufferPrewarmGpu.cpuTotal)
            return TRUE;
        if (s_ImageBufferPrewarmGpu.cacheRequired) {
            s_ImageBufferPrewarmGpu.cached = CountImageBufferPrewarmCached(
                s_ImageBufferPrewarmGpu.renderer, s_ImageBufferPrewarmGpu.chunks);
            if (s_ImageBufferPrewarmGpu.cached < s_ImageBufferPrewarmGpu.total)
                return TRUE;
        }
''',
    'hold CPU stage')

p.write_text(s, encoding='utf-8')
