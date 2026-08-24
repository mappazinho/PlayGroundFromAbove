#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

// Multicore implementation of the expensive part of MIDI::PostProcess.
// The previous ParallelMIDIPos accelerated only the initial lane sorts. Its
// N-way heap merge and the timestamp/output/sister walk still consumed every
// event on one core while the UI continued to say "Sorting events". This path
// keeps the whole heavy stage on the physical-core worker pool:
//   lane sort -> parallel merge path -> timestamps -> ordered channel output
//   -> equal-time note-off ordering -> sister/program-index finalization.
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
        std::vector<std::thread> threads;
        threads.reserve(workers);
        for (unsigned worker = 0; worker < workers; ++worker)
            threads.emplace_back([&, worker]() { fn(worker); });
        for (auto& thread : threads)
            thread.join();
    }

    template <typename Fn>
    static inline void RunTaskPool(unsigned workers, size_t taskCount, Fn&& fn)
    {
        if (taskCount == 0)
            return;
        std::atomic<size_t> next{ 0 };
        RunWorkers(workers, [&](unsigned worker) {
            for (;;)
            {
                const size_t task = next.fetch_add(1, std::memory_order_relaxed);
                if (task >= taskCount)
                    break;
                fn(worker, task);
            }
        });
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
    using Ref = uint32_t;
    static constexpr Ref MetaFlag = 0x80000000u;
    static constexpr size_t SortBlock = 1u << 18;
    static constexpr uint64_t ProgressUnits = 7000;

    size_t metaCount = 0;
    for (MIDITrack* trackPtr : m_vTracks)
        metaCount += trackPtr->m_vMetas.size();

    const size_t channelRows = (size_t)m_iFullRows + m_vThinTicks.size();
    const size_t totalEvents = channelRows + metaCount;
    if (channelRows >= (size_t)MetaFlag || metaCount >= (size_t)MetaFlag)
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

    // Keep meta/sysex pointers in one compact table. Their index doubles as the
    // stable parse-order ordinal for same-tick/same-track ordering.
    std::vector<MIDIEvent*> metaRefs(metaCount);
    {
        size_t out = 0;
        for (MIDITrack* trackPtr : m_vTracks)
            for (MIDIEvent* event : trackPtr->m_vMetas)
                metaRefs[out++] = event;
    }

    std::vector<Ref> refs(totalEvents);
    RunWorkers(workers, [&](unsigned worker) {
        const size_t begin = totalEvents * (size_t)worker / workers;
        const size_t end = totalEvents * (size_t)(worker + 1) / workers;
        for (size_t i = begin; i < end; ++i)
        {
            if (i < channelRows)
                refs[i] = (Ref)i;
            else
                refs[i] = MetaFlag | (Ref)(i - channelRows);
        }
    });

    auto isMeta = [&](Ref ref) { return (ref & MetaFlag) != 0; };
    auto metaIndex = [&](Ref ref) { return (size_t)(ref & ~MetaFlag); };
    auto meta = [&](Ref ref) -> MIDIEvent* { return metaRefs[metaIndex(ref)]; };
    auto eventTickOf = [&](Ref ref) -> int {
        return isMeta(ref) ? meta(ref)->GetAbsT() : (int)GetEventTicks((MIDIChannelEvent)ref);
    };
    auto eventTrackOf = [&](Ref ref) -> int {
        return isMeta(ref) ? meta(ref)->GetTrack() : (int)GetEventTrack((MIDIChannelEvent)ref);
    };
    auto eventKind = [&](Ref ref) -> int {
        if (isMeta(ref)) return 0;
        return IsThinRow((MIDIChannelEvent)ref) ? 2 : 1;
    };
    auto eventOrdinal = [&](Ref ref) -> uint32_t {
        return isMeta(ref) ? (uint32_t)metaIndex(ref) : ref;
    };
    auto less = [&](Ref a, Ref b) {
        const int at = eventTickOf(a), bt = eventTickOf(b);
        if (at != bt) return at < bt;
        const int atr = eventTrackOf(a), btr = eventTrackOf(b);
        if (atr != btr) return atr < btr;
        const int ak = eventKind(a), bk = eventKind(b);
        if (ak != bk) return ak < bk; // meta, pool, thin: legacy merge order
        return eventOrdinal(a) < eventOrdinal(b);
    };

    struct Range { size_t begin, end; };
    std::vector<Range> ranges(workers);

    // Per-core lane sort. Block sorting keeps every progress bar visibly moving
    // instead of sitting at zero for one giant std::sort call.
    RunWorkers(workers, [&](unsigned worker) {
        const size_t begin = totalEvents * (size_t)worker / workers;
        const size_t end = totalEvents * (size_t)(worker + 1) / workers;
        ranges[worker] = { begin, end };
        const size_t count = end - begin;
        const size_t blocks = count ? (count + SortBlock - 1) / SortBlock : 0;
        size_t mergeTasks = 0;
        for (size_t width = SortBlock; width < count; )
        {
            mergeTasks += (count + width * 2 - 1) / (width * 2);
            if (width > count / 2)
                break;
            width *= 2;
        }
        const size_t taskMax = (std::max)((size_t)1, blocks + mergeTasks);
        size_t done = 0;

        for (size_t left = begin; left < end; left += SortBlock)
        {
            const size_t right = (std::min)(end, left + SortBlock);
            std::sort(refs.begin() + left, refs.begin() + right, less);
            ++done;
            setProgress(worker, (uint64_t)(done * 1000 / taskMax));
        }
        for (size_t width = SortBlock; width < count; )
        {
            const size_t pairWidth = width * 2;
            for (size_t rel = 0; rel < count; rel += pairWidth)
            {
                const size_t left = begin + rel;
                const size_t mid = (std::min)(end, left + width);
                const size_t right = (std::min)(end, left + pairWidth);
                if (mid < right)
                    std::inplace_merge(refs.begin() + left, refs.begin() + mid, refs.begin() + right, less);
                ++done;
                setProgress(worker, (uint64_t)(done * 1000 / taskMax));
            }
            if (width > count / 2)
                break;
            width *= 2;
        }
        setProgress(worker, 1000);
    });

    // Merge-path co-ranking lets even the final two huge ranges be merged by
    // every physical core instead of collapsing back to one thread.
    auto coRank = [&](const std::vector<Ref>& src, size_t a0, size_t aLen,
                      size_t b0, size_t bLen, size_t diagonal) -> size_t {
        size_t low = diagonal > bLen ? diagonal - bLen : 0;
        size_t high = (std::min)(diagonal, aLen);
        while (low <= high)
        {
            const size_t i = low + (high - low) / 2;
            const size_t j = diagonal - i;
            if (i > 0 && j < bLen && less(src[b0 + j], src[a0 + i - 1]))
            {
                high = i - 1;
            }
            else if (j > 0 && i < aLen && less(src[a0 + i], src[b0 + j - 1]))
            {
                low = i + 1;
            }
            else
                return i;
        }
        return low;
    };

    struct MergeTask
    {
        size_t a0, a1, b0, b1, out;
    };
    std::vector<Ref> scratch(totalEvents);
    bool sourceIsRefs = true;
    while (ranges.size() > 1)
    {
        const std::vector<Ref>& src = sourceIsRefs ? refs : scratch;
        std::vector<Ref>& dst = sourceIsRefs ? scratch : refs;
        std::vector<Range> nextRanges;
        std::vector<MergeTask> tasks;
        nextRanges.reserve((ranges.size() + 1) / 2);

        for (size_t r = 0; r < ranges.size(); r += 2)
        {
            const Range a = ranges[r];
            if (r + 1 >= ranges.size())
            {
                const size_t len = a.end - a.begin;
                const size_t block = (std::max)((size_t)1, (len + workers * 4 - 1) / (workers * 4));
                for (size_t off = 0; off < len; off += block)
                {
                    const size_t n = (std::min)(block, len - off);
                    tasks.push_back({ a.begin + off, a.begin + off + n, 0, 0, a.begin + off });
                }
                nextRanges.push_back(a);
                continue;
            }

            const Range b = ranges[r + 1];
            const size_t aLen = a.end - a.begin;
            const size_t bLen = b.end - b.begin;
            const size_t total = aLen + bLen;
            const size_t block = (std::max)((size_t)1, (total + workers * 4 - 1) / (workers * 4));
            for (size_t d0 = 0; d0 < total; d0 += block)
            {
                const size_t d1 = (std::min)(total, d0 + block);
                const size_t i0 = coRank(src, a.begin, aLen, b.begin, bLen, d0);
                const size_t i1 = coRank(src, a.begin, aLen, b.begin, bLen, d1);
                const size_t j0 = d0 - i0;
                const size_t j1 = d1 - i1;
                tasks.push_back({ a.begin + i0, a.begin + i1,
                                  b.begin + j0, b.begin + j1,
                                  a.begin + d0 });
            }
            nextRanges.push_back({ a.begin, b.end });
        }

        std::atomic<size_t> mergeDone{ 0 };
        RunTaskPool(workers, tasks.size(), [&](unsigned worker, size_t taskIndex) {
            const MergeTask& task = tasks[taskIndex];
            if (task.b0 == task.b1)
                std::copy(src.begin() + task.a0, src.begin() + task.a1, dst.begin() + task.out);
            else
                std::merge(src.begin() + task.a0, src.begin() + task.a1,
                           src.begin() + task.b0, src.begin() + task.b1,
                           dst.begin() + task.out, less);
            const size_t done = mergeDone.fetch_add(1, std::memory_order_relaxed) + 1;
            setProgress(worker, 1000 + (uint64_t)(done * 1000 / (std::max)((size_t)1, tasks.size())));
        });
        for (unsigned worker = 0; worker < workers; ++worker)
            setProgress(worker, 2000);

        ranges.swap(nextRanges);
        sourceIsRefs = !sourceIsRefs;
    }
    if (!sourceIsRefs)
        refs.swap(scratch);
    std::vector<Ref>().swap(scratch);

    // Build tempo anchors once. Event timestamps can then be calculated
    // independently in each sorted slice with a local forward-only anchor index.
    std::vector<uint32_t> tempoMeta;
    tempoMeta.reserve(metaCount / 32 + 8);
    for (uint32_t i = 0; i < (uint32_t)metaRefs.size(); ++i)
    {
        MIDIEvent* event = metaRefs[i];
        if (event && event->GetEventType() == MIDIEvent::MetaEvent &&
            reinterpret_cast<MIDIMetaEvent*>(event)->GetMetaEventType() == MIDIMetaEvent::SetTempo)
            tempoMeta.push_back(i);
    }
    std::sort(tempoMeta.begin(), tempoMeta.end(), [&](uint32_t a, uint32_t b) {
        MIDIEvent* ea = metaRefs[a];
        MIDIEvent* eb = metaRefs[b];
        if (ea->GetAbsT() != eb->GetAbsT()) return ea->GetAbsT() < eb->GetAbsT();
        if (ea->GetTrack() != eb->GetTrack()) return ea->GetTrack() < eb->GetTrack();
        return a < b;
    });

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

    std::vector<size_t> sliceBegin(workers), sliceEnd(workers);
    std::vector<size_t> channelCount(workers, 0);
    std::vector<long long> simultDelta(workers, 0);
    std::vector<long long> firstNote(workers, (std::numeric_limits<long long>::max)());

    // Pass 1: full-row/meta timestamps plus per-slice counts/prefix deltas.
    RunWorkers(workers, [&](unsigned worker) {
        const size_t begin = totalEvents * (size_t)worker / workers;
        const size_t end = totalEvents * (size_t)(worker + 1) / workers;
        sliceBegin[worker] = begin;
        sliceEnd[worker] = end;
        size_t ai = begin < end ? anchorForTick(eventTickOf(refs[begin])) : 0;
        size_t localChannels = 0;
        long long delta = 0;
        long long localFirst = (std::numeric_limits<long long>::max)();
        const size_t span = (std::max)((size_t)1, end - begin);
        for (size_t pos = begin; pos < end; ++pos)
        {
            const Ref ref = refs[pos];
            const int eventTick = eventTickOf(ref);
            while (ai + 1 < anchors.size() && anchors[ai + 1].tick <= eventTick)
                ++ai;
            const long long eventTime = timeFromAnchor(eventTick, anchors[ai]);
            if (isMeta(ref))
            {
                meta(ref)->SetAbsMicroSec(eventTime);
            }
            else
            {
                const MIDIChannelEvent row = (MIDIChannelEvent)ref;
                ++localChannels;
                if (!IsThinRow(row))
                    SetEventTime(row, eventTime);
                if (EventHasSister(row))
                {
                    if (GetEventChannelEventType(row) == NoteOn && GetEventParam2(row) > 0)
                    {
                        ++delta;
                        localFirst = (std::min)(localFirst, eventTime);
                    }
                    else
                        --delta;
                }
            }
            if (((pos - begin) & 0xFFFFu) == 0)
                setProgress(worker, 2000 + (uint64_t)((pos - begin) * 1000 / span));
        }
        channelCount[worker] = localChannels;
        simultDelta[worker] = delta;
        firstNote[worker] = localFirst;
        setProgress(worker, 3000);
    });

    // Pass 2: folded note-offs store length rather than an absolute timestamp.
    // Full owner timestamps are complete now, so this pass is fully independent.
    RunWorkers(workers, [&](unsigned worker) {
        const size_t begin = sliceBegin[worker], end = sliceEnd[worker];
        size_t ai = begin < end ? anchorForTick(eventTickOf(refs[begin])) : 0;
        const size_t span = (std::max)((size_t)1, end - begin);
        for (size_t pos = begin; pos < end; ++pos)
        {
            const Ref ref = refs[pos];
            if (!isMeta(ref) && IsThinRow((MIDIChannelEvent)ref))
            {
                const MIDIChannelEvent row = (MIDIChannelEvent)ref;
                const int eventTick = (int)GetEventTicks(row);
                while (ai + 1 < anchors.size() && anchors[ai + 1].tick <= eventTick)
                    ++ai;
                const long long eventTime = timeFromAnchor(eventTick, anchors[ai]);
                const MIDIChannelEvent owner = GetThinOwner(row);
                if (GetEventTicks(row) == GetEventTicks(owner))
                    SetEventLength(row, 0);
                else
                    SetEventLength(row, (uint32_t)(std::max)(0LL, eventTime - GetEventTime(owner)));
            }
            if (((pos - begin) & 0xFFFFu) == 0)
                setProgress(worker, 3000 + (uint64_t)((pos - begin) * 1000 / span));
        }
        setProgress(worker, 4000);
    });

    std::vector<size_t> channelBase(workers, 0);
    std::vector<long long> simultBase(workers, 0);
    size_t newChannelCount = 0;
    long long runningSimult = 0;
    for (unsigned worker = 0; worker < workers; ++worker)
    {
        channelBase[worker] = newChannelCount;
        simultBase[worker] = runningSimult;
        newChannelCount += channelCount[worker];
        runningSimult += simultDelta[worker];
    }

    const size_t outputBase = vChannelEvents.size();
    vChannelEvents.resize(outputBase + newChannelCount);

    // Pass 3: filter the globally ordered refs into disjoint output slices and
    // compute the legacy pre-partition simultaneous-note counter with prefixes.
    RunWorkers(workers, [&](unsigned worker) {
        const size_t begin = sliceBegin[worker], end = sliceEnd[worker];
        size_t out = outputBase + channelBase[worker];
        long long simultaneous = simultBase[worker];
        const size_t span = (std::max)((size_t)1, end - begin);
        for (size_t pos = begin; pos < end; ++pos)
        {
            const Ref ref = refs[pos];
            if (!isMeta(ref))
            {
                const MIDIChannelEvent row = (MIDIChannelEvent)ref;
                SetEventSimult(row, (unsigned)(std::max)(0LL, simultaneous));
                vChannelEvents[out++] = row;
                if (EventHasSister(row))
                {
                    if (GetEventChannelEventType(row) == NoteOn && GetEventParam2(row) > 0)
                        ++simultaneous;
                    else
                        --simultaneous;
                }
            }
            if (((pos - begin) & 0xFFFFu) == 0)
                setProgress(worker, 4000 + (uint64_t)((pos - begin) * 1000 / span));
        }
        setProgress(worker, 5000);
    });

    std::vector<Ref>().swap(refs);

    // Legacy behavior restores repeated-note ordering after time conversion:
    // within every equal-microsecond channel group, folded note-offs (thin rows)
    // precede pool rows. Partition groups in independent physical-core slices.
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

        std::vector<MIDIChannelEvent> reordered(newChannelCount);
        RunWorkers(workers, [&](unsigned worker) {
            const size_t begin = boundaries[worker], end = boundaries[worker + 1];
            const size_t span = (std::max)((size_t)1, end - begin);
            size_t group = begin;
            while (group < end)
            {
                const long long groupTime = GetEventTime(vChannelEvents[outputBase + group]);
                size_t groupEnd = group + 1;
                while (groupEnd < end && GetEventTime(vChannelEvents[outputBase + groupEnd]) == groupTime)
                    ++groupEnd;

                size_t thinCount = 0;
                for (size_t p = group; p < groupEnd; ++p)
                    thinCount += IsThinRow(vChannelEvents[outputBase + p]) ? 1u : 0u;
                size_t thinOut = group;
                size_t poolOut = group + thinCount;
                for (size_t p = group; p < groupEnd; ++p)
                {
                    const MIDIChannelEvent row = vChannelEvents[outputBase + p];
                    reordered[IsThinRow(row) ? thinOut++ : poolOut++] = row;
                }
                group = groupEnd;
                setProgress(worker, 5000 + (uint64_t)((group - begin) * 1000 / span));
            }
            setProgress(worker, 6000);
        });

        RunWorkers(workers, [&](unsigned worker) {
            const size_t begin = newChannelCount * (size_t)worker / workers;
            const size_t end = newChannelCount * (size_t)(worker + 1) / workers;
            std::copy(reordered.begin() + begin, reordered.begin() + end,
                      vChannelEvents.begin() + outputBase + begin);
        });
        std::vector<MIDIChannelEvent>().swap(reordered);
    }
    else
    {
        for (unsigned worker = 0; worker < workers; ++worker)
            setProgress(worker, 6000);
    }

    // Sister-index pass A: paired pool rows temporarily store their own final
    // list position. Thin rows can then resolve the final owner position without
    // a separate row->position array (which would cost hundreds of MB here).
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
                SetEventLength(owner, GetEventLength(row));
                SetEventPassDone(row, true);
                SetEventPassDone(owner, true);
            }
            if (((p - begin) & 0xFFFFu) == 0)
                setProgress(worker, 6500 + (uint64_t)((p - begin) * 500 / span));
        }
        setProgress(worker, 7000);
    });

    if (vProgramChanges)
        for (unsigned worker = 0; worker < workers; ++worker)
            vProgramChanges->insert(vProgramChanges->end(), programLocal[worker].begin(), programLocal[worker].end());

    // Meta maps are tiny compared with channel data. Sort just the meta indices
    // in exact global order and append them after all parallel timestamp work.
    if (vMetaEvents)
    {
        std::vector<uint32_t> orderedMeta(metaRefs.size());
        for (uint32_t i = 0; i < (uint32_t)orderedMeta.size(); ++i)
            orderedMeta[i] = i;
        std::sort(orderedMeta.begin(), orderedMeta.end(), [&](uint32_t a, uint32_t b) {
            MIDIEvent* ea = metaRefs[a];
            MIDIEvent* eb = metaRefs[b];
            if (ea->GetAbsT() != eb->GetAbsT()) return ea->GetAbsT() < eb->GetAbsT();
            if (ea->GetTrack() != eb->GetTrack()) return ea->GetTrack() < eb->GetTrack();
            return a < b;
        });
        for (uint32_t index : orderedMeta)
        {
            MIDIEvent* event = metaRefs[index];
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
