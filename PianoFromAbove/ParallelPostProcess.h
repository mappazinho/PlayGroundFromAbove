#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <chrono>

extern void PRE_DbgLog(const char* format, ...);

// Multicore MIDI post-process. Every track already stores its rows (and its folded note-offs) in
// tick order, so the global order is a k-way merge of those runs: no sort buffers, and the merge
// writes straight into the final event list.
namespace PGFAParallelPostProcess
{
    static inline unsigned PhysicalCoreCount()
    {
        DWORD bytes = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
        if (bytes > 0)
        {
            std::vector<unsigned char> storage(bytes);
            auto* first = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data());
            if (GetLogicalProcessorInformationEx(RelationProcessorCore, first, &bytes))
            {
                unsigned count = 0;
                unsigned char* p = storage.data();
                const unsigned char* end = storage.data() + bytes;
                while (p < end)
                {
                    auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(p);
                    if (info->Relationship == RelationProcessorCore)
                        ++count;
                    if (info->Size == 0)
                        break;
                    p += info->Size;
                }
                if (count > 0)
                    return count;
            }
        }
        const unsigned logical = std::thread::hardware_concurrency();
        return logical > 0 ? logical : 1u;
    }

    template <typename Fn>
    static inline void RunWorkers(unsigned workers, Fn&& fn)
    {
        // Worker 0 runs on the calling thread, and a worker whose thread can't be created
        // (std::system_error under memory/handle pressure) runs inline instead of
        // terminating the process.
        std::vector<std::thread> threads;
        threads.reserve(workers);
        for (unsigned worker = 1; worker < workers; ++worker)
        {
            try { threads.emplace_back([&, worker]() { fn(worker); }); }
            catch (...) { fn(worker); }
        }
        fn(0u);
        for (auto& thread : threads)
            thread.join();
    }

    struct TempoAnchor
    {
        int tick = 0;
        long long time = 0;
        uint32_t microSecsPerBeat = 500000;
    };
}

inline void MIDI::PostProcessParallel(
    vector<MIDIChannelEvent>& vChannelEvents,
    eventvec_t* vProgramChanges,
    vector<MIDIMetaEvent*>* vMetaEvents,
    eventvec_t* vTempo,
    eventvec_t* vSignature,
    eventvec_t* vMarkers)
{
    using namespace PGFAParallelPostProcess;
    static constexpr uint64_t ProgressUnits = 7000;

    size_t metaCount = 0;
    for (MIDITrack* trackPtr : m_vTracks)
        metaCount += trackPtr->m_vMetas.size();

    const size_t channelRows = (size_t)m_iFullRows + m_vThinOwners.size();
    const size_t totalEvents = channelRows + metaCount;
    if (channelRows >= (size_t)0xFFFFFFFFu || metaCount >= (size_t)0xFFFFFFFFu)
        throw std::runtime_error("MIDI event count exceeds compact parallel-postprocess reference range");

    if (totalEvents == 0)
    {
        g_LoadingProgress.stage = MIDILoadingProgress::Stage::SortEvents;
        g_LoadingProgress.progress.store(0, std::memory_order_release);
        g_LoadingProgress.max = 0;
        g_LoadingProgress.sortWorkerCount.store(0, std::memory_order_release);
        m_Info.llTotalMicroSecs = 0;
        m_Info.llFirstNote = 0;
        return;
    }

    unsigned workers = PhysicalCoreCount();
    workers = (std::min)(workers, MIDILoadingProgress::MaxSortWorkers);
    workers = (std::min)(workers, (unsigned)totalEvents);
    workers = (std::max)(workers, 1u);

    g_LoadingProgress.stage = MIDILoadingProgress::Stage::SortEvents;
    g_LoadingProgress.progress.store(0, std::memory_order_release);
    g_LoadingProgress.max = (uint64_t)workers * ProgressUnits;
    g_LoadingProgress.sortWorkerCount.store(workers, std::memory_order_release);
    for (unsigned i = 0; i < MIDILoadingProgress::MaxSortWorkers; ++i)
    {
        g_LoadingProgress.sortProgress[i].store(0, std::memory_order_relaxed);
        g_LoadingProgress.sortMax[i].store(i < workers ? ProgressUnits : 0, std::memory_order_relaxed);
    }

    auto setProgress = [&](unsigned worker, uint64_t value) {
        value = (std::min)(value, ProgressUnits);
        const uint64_t old = g_LoadingProgress.sortProgress[worker].exchange(value, std::memory_order_relaxed);
        if (value > old)
            g_LoadingProgress.progress.fetch_add(value - old, std::memory_order_relaxed);
    };

    auto passClock = std::chrono::steady_clock::now();
    auto passMark = [&](const char* name) {
        const auto now = std::chrono::steady_clock::now();
        PRE_DbgLog("LOAD [PostProcess %s] +%lldms", name,
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - passClock).count());
        passClock = now;
    };

    // Keep meta/sysex pointers in one compact table. Their index doubles as the
    // stable parse-order ordinal for same-tick/same-track ordering.
    std::vector<MIDIEvent*> metaRefs(metaCount);

    // Global meta order is (tick, track, parse order). The index is track-major in parse order, so
    // (tick, index) is the same order, and every track's metas are already in tick order: the
    // order is just a merge of one sorted run per track. (This used to be a full std::sort that
    // chased two pointers per comparison, ~0.3 s for 3 million tempo events.)
    struct MetaKey { int tick; uint32_t idx; };
    std::vector<MetaKey> metaOrder(metaCount);
    {
        auto keyLess = [](const MetaKey& a, const MetaKey& b) { return a.tick != b.tick ? a.tick < b.tick : a.idx < b.idx; };
        std::vector<size_t> runStart;
        runStart.reserve(m_vTracks.size() + 1);
        bool runsSorted = true;
        size_t out = 0;
        for (MIDITrack* trackPtr : m_vTracks)
        {
            runStart.push_back(out);
            int prevTick = (std::numeric_limits<int>::min)();
            for (MIDIEvent* event : trackPtr->m_vMetas)
            {
                const int tick = event->GetAbsT();
                if (tick < prevTick) runsSorted = false;
                prevTick = tick;
                metaRefs[out] = event;
                metaOrder[out] = { tick, (uint32_t)out };
                ++out;
            }
        }
        runStart.push_back(out);
        if (!runsSorted)
            std::sort(metaOrder.begin(), metaOrder.end(), keyLess);
        else
        {
            // bottom-up pairwise merge of the runs
            while (runStart.size() > 2)
            {
                std::vector<size_t> next;
                next.reserve(runStart.size() / 2 + 2);
                const size_t runs = runStart.size() - 1;
                for (size_t r = 0; r < runs; r += 2)
                {
                    next.push_back(runStart[r]);
                    if (r + 1 < runs)
                        std::inplace_merge(metaOrder.begin() + runStart[r], metaOrder.begin() + runStart[r + 1],
                                           metaOrder.begin() + runStart[r + 2], keyLess);
                }
                next.push_back(out);
                runStart.swap(next);
            }
        }
    }

    // Build tempo anchors once. Event timestamps can then be calculated
    // independently for any event with a local anchor lookup.
    std::vector<uint32_t> tempoMeta; // meta indices of the SetTempo events, in global order
    tempoMeta.reserve(metaCount / 32 + 8);
    for (const MetaKey& key : metaOrder)
    {
        MIDIEvent* event = metaRefs[key.idx];
        if (event && event->GetEventType() == MIDIEvent::MetaEvent &&
            reinterpret_cast<MIDIMetaEvent*>(event)->GetMetaEventType() == MIDIMetaEvent::SetTempo)
            tempoMeta.push_back(key.idx);
    }

    const bool standard = (m_Info.iDivision & 0x8000) == 0;
    const int ticksPerBeat = standard ? (int)m_Info.iDivision : 0;
    int ticksPerSecond = 0;
    if (!standard)
    {
        int framesPerSec = -((m_Info.iDivision | static_cast<int>(0xFFFF0000)) >> 8) * 100;
        if (framesPerSec == 2900) framesPerSec = 2997;
        ticksPerSecond = (m_Info.iDivision & 0xFF) * framesPerSec;
    }

    std::vector<TempoAnchor> anchors;
    anchors.reserve(tempoMeta.size() + 1);
    anchors.push_back({ 0, 0, 500000 });
    for (uint32_t index : tempoMeta)
    {
        MIDIMetaEvent* event = reinterpret_cast<MIDIMetaEvent*>(metaRefs[index]);
        const TempoAnchor prev = anchors.back();
        const int eventTick = event->GetAbsT();
        long long eventTime = prev.time;
        if (standard)
            eventTime += (static_cast<long long>(prev.microSecsPerBeat) * (eventTick - prev.tick)) / ticksPerBeat;
        else
            eventTime += (1000000LL * (eventTick - prev.tick)) / ticksPerSecond;

        uint32_t nextTempo = prev.microSecsPerBeat;
        if (event->GetDataLen() == 3)
            MIDI::Parse24Bit(event->GetData(), 3, &nextTempo);
        // Even malformed SetTempo events reset the legacy integer-rounding
        // anchor, so retain an anchor when the tempo payload itself is ignored.
        anchors.push_back({ eventTick, eventTime, nextTempo });
    }

    auto anchorForTick = [&](int eventTick) -> size_t {
        auto it = std::upper_bound(anchors.begin(), anchors.end(), eventTick,
            [](int value, const TempoAnchor& anchor) { return value < anchor.tick; });
        return it == anchors.begin() ? 0 : (size_t)(it - anchors.begin() - 1);
    };
    auto timeFromAnchor = [&](int eventTick, const TempoAnchor& anchor) -> long long {
        if (standard)
            return anchor.time + (static_cast<long long>(anchor.microSecsPerBeat) * (eventTick - anchor.tick)) / ticksPerBeat;
        return anchor.time + (1000000LL * (eventTick - anchor.tick)) / ticksPerSecond;
    };
    if (m_bComputedTimes)
    {
        // Row times aren't stored: install the tempo map the accessors derive them from.
        if (anchors.size() > MaxComputedAnchors)
            throw std::runtime_error("MIDI tempo map too large for computed times");
        m_bTimeStandard = standard;
        m_iTimeTicksPerBeat = ticksPerBeat;
        m_iTimeTicksPerSecond = ticksPerSecond;
        const int timeDiv = standard ? ticksPerBeat : ticksPerSecond;
        SetTimeDivisor(timeDiv);
        int aTick[MaxComputedAnchors];
        int64_t aTime[MaxComputedAnchors];
        uint32_t aMpb[MaxComputedAnchors];
        for (size_t i = 0; i < anchors.size(); ++i)
        {
            aTick[i] = anchors[i].tick;
            aTime[i] = anchors[i].time;
            aMpb[i] = anchors[i].microSecsPerBeat;
        }
        SetTimeAnchors(anchors.size(), aTick, aTime, aMpb);
    }

    // Per-thread cached anchor: ticks are mostly monotonic within a track, so the cached
    // anchor usually still applies and the binary search is skipped.
    auto timeOfTick = [&](int eventTick, size_t& ai) -> long long {
        if (eventTick < anchors[ai].tick || (ai + 1 < anchors.size() && eventTick >= anchors[ai + 1].tick))
            ai = anchorForTick(eventTick);
        return timeFromAnchor(eventTick, anchors[ai]);
    };

    passMark("setup (metas, tempo map)");
    // ---- Pass 1: timestamps of full rows and meta events (no ordering needed). ----
    const size_t fullRows = m_iFullRows;
    std::vector<long long> firstNote(workers, (std::numeric_limits<long long>::max)());
    RunWorkers(workers, [&](unsigned worker) {
        size_t ai = 0;
        const size_t begin = fullRows * (size_t)worker / workers;
        const size_t end = fullRows * (size_t)(worker + 1) / workers;
        const size_t span = (std::max)((size_t)1, end - begin);
        long long localFirst = (std::numeric_limits<long long>::max)();
        for (size_t row = begin; row < end; ++row)
        {
            const long long eventTime = timeOfTick((int)m_vTicks[row], ai);
            if (!m_bComputedTimes)
                m_vTimes[row] = eventTime;
            if (m_vSisters[row] != UINT32_MAX &&
                GetEventChannelEventType((MIDIChannelEvent)row) == NoteOn && GetEventParam2((MIDIChannelEvent)row) > 0)
                localFirst = (std::min)(localFirst, eventTime);
            if (((row - begin) & 0xFFFFu) == 0)
                setProgress(worker, (uint64_t)((row - begin) * 700 / span));
        }
        firstNote[worker] = localFirst;

        const size_t mbegin = metaCount * (size_t)worker / workers;
        const size_t mend = metaCount * (size_t)(worker + 1) / workers;
        ai = 0;
        for (size_t i = mbegin; i < mend; ++i)
            metaRefs[i]->SetAbsMicroSec(timeOfTick(metaRefs[i]->GetAbsT(), ai));
        setProgress(worker, 1000);
    });

    passMark("pass1 times");
    // ---- Pass 2: folded note-off lengths (need the owners' times from pass 1). ----
    // A note-off's length is its note-on's length, stored on the note-on row (unique owner per
    // note-off, so the writes never collide).
    const size_t thinCount = m_vThinOwners.size();
    RunWorkers(workers, [&](unsigned worker) {
        size_t ai = 0;
        const size_t begin = thinCount * (size_t)worker / workers;
        const size_t end = thinCount * (size_t)(worker + 1) / workers;
        const size_t span = (std::max)((size_t)1, end - begin);
        for (size_t thin = begin; thin < end; ++thin) {
            const int eventTick = (int)GetThinTick(thin);
            const long long eventTime = timeOfTick(eventTick, ai);
            const MIDIChannelEvent owner = (MIDIChannelEvent)(m_vThinOwners[thin] & ~THIN_OWNER_NOTEON_FLAG);
            if ((uint32_t)eventTick == m_vTicks[owner])
                m_vLengths[owner] = 0;
            else
                m_vLengths[owner] = (uint32_t)(std::max)(0LL, eventTime - GetPoolTime(owner));
            if (((thin - begin) & 0xFFFFu) == 0)
                setProgress(worker, 1000 + (uint64_t)((thin - begin) * 1000 / span));
        }
        setProgress(worker, 2000);
    });

    passMark("pass2 thin lengths");
    // ---- Pass 3: k-way merge of the per-track runs straight into the output list. ----
    // Final order is (tick, track, kind[full before thin], row index). Run r = 2*track is the
    // track's full rows, 2*track+1 its folded note-offs; a run is already ordered by tick and
    // its rows by index, so ties between runs are settled by the run number alone.
    const size_t nTracks = m_vTracks.size();
    const size_t nRuns = nTracks * 2;
    std::vector<uint32_t> runBegin(nRuns), runEnd(nRuns);
    for (size_t c = 0; c < nTracks; ++c)
    {
        runBegin[2 * c] = (uint32_t)m_vTracks[c]->m_iRowStart;
        runEnd[2 * c] = (uint32_t)m_vTracks[c]->m_iRowEnd;
        runBegin[2 * c + 1] = (uint32_t)m_vTracks[c]->m_iThinStart;
        runEnd[2 * c + 1] = (uint32_t)m_vTracks[c]->m_iThinEnd;
    }
    // Ticks as ordered keys (signed order, as the earlier sort used).
    auto keyAt = [&](size_t run, uint32_t pos) -> uint32_t {
        return ((run & 1) ? GetThinTick(pos) : m_vTicks[pos]) ^ 0x80000000u;
    };
    // First position in `run` whose key is >= bound (bound may be 2^32 = past everything).
    auto lowerPos = [&](size_t run, uint64_t bound) -> uint32_t {
        uint32_t lo = runBegin[run], hi = runEnd[run];
        if (bound > 0xFFFFFFFFull) return hi;
        while (lo < hi)
        {
            const uint32_t mid = lo + (hi - lo) / 2;
            if (keyAt(run, mid) < (uint32_t)bound) lo = mid + 1; else hi = mid;
        }
        return lo;
    };

    const size_t outputBase = vChannelEvents.size();
    vChannelEvents.resize(outputBase + channelRows);

    // Split the tick axis into chunks of roughly equal event counts using a sample of the keys.
    std::vector<uint64_t> bounds; // chunk k covers keys in [bounds[k-1], bounds[k]); bounds[-1]=0, last = 2^32
    {
        size_t wantChunks = channelRows < 8192 ? 1 : (size_t)workers * 8;
        if (wantChunks > 1)
        {
            const size_t stride = (std::max)((size_t)1, channelRows / 65536);
            std::vector<uint32_t> samples;
            samples.reserve(channelRows / stride + nRuns + 8);
            for (size_t run = 0; run < nRuns; ++run)
                for (size_t pos = runBegin[run]; pos < runEnd[run]; pos += stride)
                    samples.push_back(keyAt(run, (uint32_t)pos));
            std::sort(samples.begin(), samples.end());
            for (size_t k = 1; k < wantChunks && !samples.empty(); ++k)
            {
                const uint64_t b = samples[samples.size() * k / wantChunks];
                if (b > 0 && (bounds.empty() || b > bounds.back()))
                    bounds.push_back(b);
            }
        }
    }
    const size_t nChunks = bounds.size() + 1;
    auto chunkLo = [&](size_t k) -> uint64_t { return k == 0 ? 0 : bounds[k - 1]; };
    auto chunkHi = [&](size_t k) -> uint64_t { return k == bounds.size() ? (1ull << 32) : bounds[k]; };

    // Count each chunk's rows, then turn the counts into output offsets.
    std::vector<size_t> chunkBase(nChunks + 1, 0);
    {
        std::atomic<size_t> nextChunk(0);
        RunWorkers(workers, [&](unsigned) {
            size_t k;
            while ((k = nextChunk.fetch_add(1, std::memory_order_relaxed)) < nChunks)
            {
                size_t n = 0;
                const uint64_t lo = chunkLo(k), hi = chunkHi(k);
                for (size_t run = 0; run < nRuns; ++run)
                    n += lowerPos(run, hi) - lowerPos(run, lo);
                chunkBase[k + 1] = n;
            }
        });
        for (size_t k = 0; k < nChunks; ++k)
            chunkBase[k + 1] += chunkBase[k];
        if (chunkBase[nChunks] != channelRows)
            throw std::runtime_error("MIDI merge row count mismatch");
    }

    passMark("merge setup + chunk counts");
    std::vector<long long> chunkDelta(nChunks, 0); // net simultaneous-note change per chunk
    {
        std::atomic<size_t> nextChunk(0), chunksDone(0);
        RunWorkers(workers, [&](unsigned worker) {
            std::vector<uint32_t> cursor(nRuns), cursorEnd(nRuns);
            std::vector<uint64_t> heap;
            size_t k;
            while ((k = nextChunk.fetch_add(1, std::memory_order_relaxed)) < nChunks)
            {
                const uint64_t lo = chunkLo(k), hi = chunkHi(k);
                heap.clear();
                for (size_t run = 0; run < nRuns; ++run)
                {
                    const uint32_t b = lowerPos(run, lo), e = lowerPos(run, hi);
                    if (b >= e) continue;
                    cursor[run] = b;
                    cursorEnd[run] = e;
                    heap.push_back(((uint64_t)keyAt(run, b) << 32) | (uint64_t)run);
                }
                auto siftDown = [&](size_t i) {
                    const size_t n = heap.size();
                    const uint64_t v = heap[i];
                    for (;;)
                    {
                        size_t c = 2 * i + 1;
                        if (c >= n) break;
                        if (c + 1 < n && heap[c + 1] < heap[c]) ++c;
                        if (heap[c] >= v) break;
                        heap[i] = heap[c];
                        i = c;
                    }
                    heap[i] = v;
                };
                for (size_t i = heap.size() / 2; i-- > 0;)
                    siftDown(i);

                size_t out = outputBase + chunkBase[k];
                long long delta = 0;
                while (!heap.empty())
                {
                    const size_t run = (size_t)(heap[0] & 0xFFFFFFFFu);
                    // Anything from this run that sorts before the next-smallest run's head can be
                    // emitted without touching the heap again.
                    uint64_t limit = (std::numeric_limits<uint64_t>::max)();
                    if (heap.size() > 1)
                    {
                        limit = heap[1];
                        if (heap.size() > 2 && heap[2] < limit) limit = heap[2];
                    }
                    const bool thin = (run & 1) != 0;
                    uint32_t pos = cursor[run];
                    const uint32_t end = cursorEnd[run];
                    bool exhausted = false;
                    for (;;)
                    {
                        const MIDIChannelEvent row = thin ? (MIDIChannelEvent)(fullRows + pos) : (MIDIChannelEvent)pos;
                        vChannelEvents[out++] = row;
                        if (EventHasSister(row))
                            delta += (GetEventChannelEventType(row) == NoteOn && GetEventParam2(row) > 0) ? 1 : -1;
                        if (++pos == end) { exhausted = true; break; }
                        const uint64_t next = ((uint64_t)keyAt(run, pos) << 32) | (uint64_t)run;
                        if (next > limit) { heap[0] = next; break; }
                    }
                    cursor[run] = pos;
                    if (exhausted)
                    {
                        heap[0] = heap.back();
                        heap.pop_back();
                    }
                    if (!heap.empty())
                        siftDown(0);
                }
                chunkDelta[k] = delta;
                setProgress(worker, 2000 + (uint64_t)(chunksDone.fetch_add(1, std::memory_order_relaxed) + 1) * 2000 / nChunks);
            }
            setProgress(worker, 4000);
        });
    }

    passMark("merge");
    // ---- Pass 4: the legacy pre-partition simultaneous-note counter (prefix over the order). ----
    std::vector<long long> chunkSimult(nChunks, 0);
    {
        long long running = 0;
        for (size_t k = 0; k < nChunks; ++k)
        {
            chunkSimult[k] = running;
            running += chunkDelta[k];
        }
        std::atomic<size_t> nextChunk(0);
        RunWorkers(workers, [&](unsigned worker) {
            size_t k;
            while ((k = nextChunk.fetch_add(1, std::memory_order_relaxed)) < nChunks)
            {
                long long simultaneous = chunkSimult[k];
                for (size_t p = chunkBase[k]; p < chunkBase[k + 1]; ++p)
                {
                    const MIDIChannelEvent row = vChannelEvents[outputBase + p];
                    SetEventSimult(row, (unsigned)(std::max)(0LL, simultaneous));
                    if (EventHasSister(row))
                    {
                        if (GetEventChannelEventType(row) == NoteOn && GetEventParam2(row) > 0)
                            ++simultaneous;
                        else
                            --simultaneous;
                    }
                }
            }
            setProgress(worker, 5000);
        });
    }

    passMark("pass4 simult");
    const size_t newChannelCount = channelRows;
    if (newChannelCount > 1)
    {
        std::vector<size_t> boundaries(workers + 1, 0);
        boundaries[0] = 0;
        boundaries[workers] = newChannelCount;
        for (unsigned worker = 1; worker < workers; ++worker)
        {
            size_t p = newChannelCount * (size_t)worker / workers;
            while (p < newChannelCount && p > 0 &&
                   GetEventTime(vChannelEvents[outputBase + p]) == GetEventTime(vChannelEvents[outputBase + p - 1]))
                ++p;
            boundaries[worker] = p;
        }
        for (unsigned worker = 1; worker < workers; ++worker)
            boundaries[worker] = (std::max)(boundaries[worker], boundaries[worker - 1]);

        // Reorder each equal-time group in place (through a small per-worker buffer).
        RunWorkers(workers, [&](unsigned worker) {
            std::vector<MIDIChannelEvent> groupBuf;
            const size_t begin = boundaries[worker], end = boundaries[worker + 1];
            const size_t span = (std::max)((size_t)1, end - begin);
            size_t group = begin;
            while (group < end)
            {
                const long long groupTime = GetEventTime(vChannelEvents[outputBase + group]);
                size_t groupEnd = group + 1;
                while (groupEnd < end && GetEventTime(vChannelEvents[outputBase + groupEnd]) == groupTime)
                    ++groupEnd;

                // Only note-offs that end an EARLIER note go ahead of the note-ons (file order
                // for repeated notes). A zero-length note's off shares its note-on's tick and
                // must stay after it; hoisting it put the off before the on, so the player
                // pushed the key down after it had already "released" and it stuck forever.
                auto hoistedOff = [&](MIDIChannelEvent row) {
                    return IsThinRow(row) && GetThinTick((size_t)row - m_iFullRows) != m_vTicks[GetThinOwner(row)];
                };
                size_t hoisted = 0;
                if (groupEnd - group > 1)
                    for (size_t p = group; p < groupEnd; ++p)
                        hoisted += hoistedOff(vChannelEvents[outputBase + p]) ? 1u : 0u;
                if (hoisted != 0 && hoisted != groupEnd - group)
                {
                    groupBuf.resize(groupEnd - group);
                    size_t thinOut = 0;
                    size_t poolOut = hoisted;
                    for (size_t p = group; p < groupEnd; ++p)
                    {
                        const MIDIChannelEvent row = vChannelEvents[outputBase + p];
                        groupBuf[hoistedOff(row) ? thinOut++ : poolOut++] = row;
                    }
                    std::copy(groupBuf.begin(), groupBuf.end(), vChannelEvents.begin() + outputBase + group);
                }
                group = groupEnd;
                setProgress(worker, 5000 + (uint64_t)((group - begin) * 1000 / span));
            }
            setProgress(worker, 6000);
        });
    }
    else
    {
        for (unsigned worker = 0; worker < workers; ++worker)
            setProgress(worker, 6000);
    }

    passMark("equal-time reorder");
    std::vector<std::vector<pair<long long, int>>> programLocal(workers);
    RunWorkers(workers, [&](unsigned worker) {
        const size_t begin = newChannelCount * (size_t)worker / workers;
        const size_t end = newChannelCount * (size_t)(worker + 1) / workers;
        auto& programs = programLocal[worker];
        programs.reserve((end - begin) / 128 + 8);
        const size_t span = (std::max)((size_t)1, end - begin);
        for (size_t p = begin; p < end; ++p)
        {
            const MIDIChannelEvent row = vChannelEvents[outputBase + p];
            if (!IsThinRow(row))
            {
                if (EventHasSister(row))
                    SetEventSisterIdx(row, (unsigned)(outputBase + p));
                const ChannelEventType type = GetEventChannelEventType(row);
                if (vProgramChanges && (type == ProgramChange || type == Controller || type == PitchBend))
                    programs.push_back(pair<long long, int>(GetEventTime(row), (int)(outputBase + p)));
            }
            if (((p - begin) & 0xFFFFu) == 0)
                setProgress(worker, 6000 + (uint64_t)((p - begin) * 500 / span));
        }
        setProgress(worker, 6500);
    });

    passMark("sister pass A + program list");
    // Sister-index pass B: each folded note-off owns one unique note-on, so all
    // writes are disjoint and safe to complete in parallel.
    RunWorkers(workers, [&](unsigned worker) {
        const size_t begin = newChannelCount * (size_t)worker / workers;
        const size_t end = newChannelCount * (size_t)(worker + 1) / workers;
        const size_t span = (std::max)((size_t)1, end - begin);
        for (size_t p = begin; p < end; ++p)
        {
            const MIDIChannelEvent row = vChannelEvents[outputBase + p];
            if (IsThinRow(row))
            {
                const MIDIChannelEvent owner = GetThinOwner(row);
                const unsigned ownerPos = GetEventSisterIdx(owner);
                SetEventSisterIdx(row, ownerPos);
                SetEventSisterIdx(owner, (unsigned)(outputBase + p));
                SetEventPassDone(row, true);
                SetEventPassDone(owner, true);
            }
            if (((p - begin) & 0xFFFFu) == 0)
                setProgress(worker, 6500 + (uint64_t)((p - begin) * 500 / span));
        }
        setProgress(worker, 7000);
    });

    passMark("sister pass B");
    if (vProgramChanges)
    {
        size_t extra = 0;
        for (unsigned worker = 0; worker < workers; ++worker)
            extra += programLocal[worker].size();
        vProgramChanges->reserve(vProgramChanges->size() + extra);
        for (unsigned worker = 0; worker < workers; ++worker)
        {
            vProgramChanges->insert(vProgramChanges->end(), programLocal[worker].begin(), programLocal[worker].end());
            std::vector<pair<long long, int>>().swap(programLocal[worker]);
        }
    }

    // Meta maps are tiny compared with channel data. Sort just the meta indices
    // in exact global order and append them after all parallel timestamp work.
    if (vMetaEvents)
    {
        vMetaEvents->reserve(vMetaEvents->size() + metaCount);
        if (vTempo) vTempo->reserve(vTempo->size() + tempoMeta.size());
        for (const MetaKey& key : metaOrder)
        {
            MIDIEvent* event = metaRefs[key.idx];
            if (!event || event->GetEventType() != MIDIEvent::MetaEvent)
                continue;
            MIDIMetaEvent* metaEvent = reinterpret_cast<MIDIMetaEvent*>(event);
            vMetaEvents->push_back(metaEvent);
            const int outIndex = (int)vMetaEvents->size() - 1;
            switch (metaEvent->GetMetaEventType())
            {
            case MIDIMetaEvent::SetTempo:
                if (vTempo) vTempo->push_back(pair<long long, int>(metaEvent->GetAbsMicroSec(), outIndex));
                break;
            case MIDIMetaEvent::TimeSignature:
                if (vSignature) vSignature->push_back(pair<long long, int>(metaEvent->GetAbsMicroSec(), outIndex));
                break;
            case MIDIMetaEvent::Marker:
                if (vMarkers) vMarkers->push_back(pair<long long, int>(metaEvent->GetAbsMicroSec(), outIndex));
                break;
            default:
                break;
            }
        }
    }

    long long llFirst = (std::numeric_limits<long long>::max)();
    for (unsigned worker = 0; worker < workers; ++worker)
        llFirst = (std::min)(llFirst, firstNote[worker]);
    m_Info.llFirstNote = llFirst == (std::numeric_limits<long long>::max)() ? 0 : (std::max)(0LL, llFirst);

    const int lastTick = m_Info.iTotalTicks;
    const size_t lastAnchor = anchorForTick(lastTick);
    m_Info.llTotalMicroSecs = timeFromAnchor(lastTick, anchors[lastAnchor]);

    g_LoadingProgress.progress.store(g_LoadingProgress.max, std::memory_order_release);
    g_LoadingProgress.sortWorkerCount.store(0, std::memory_order_release);
}
