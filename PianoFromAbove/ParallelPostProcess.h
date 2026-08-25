#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

// Multicore MIDI post-process. Sorting uses stable parallel radix passes.
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
    auto isThinRef = [&](Ref ref) { return !isMeta(ref) && ref >= m_iFullRows; };
    auto eventTickOf = [&](Ref ref) -> int {
        if (isMeta(ref)) return meta(ref)->GetAbsT();
        if (isThinRef(ref)) return (int)m_vThinTicks[(size_t)ref - m_iFullRows];
        return (int)m_vTicks[ref];
    };
    auto eventTrackOf = [&](Ref ref) -> uint32_t {
        if (isMeta(ref)) return (uint32_t)meta(ref)->GetTrack();
        if (isThinRef(ref)) {
            const size_t thin = (size_t)ref - m_iFullRows;
            const Ref owner = m_vThinOwners[thin] & ~THIN_OWNER_NOTEON_FLAG;
            return (uint32_t)m_vEventTrack[owner];
        }
        return (uint32_t)m_vEventTrack[ref];
    };
    auto eventKind = [&](Ref ref) -> uint32_t {
        if (isMeta(ref)) return 0u;
        return isThinRef(ref) ? 2u : 1u;
    };

    static constexpr unsigned RadixBits = 11;
    static constexpr size_t RadixBuckets = 1u << RadixBits;
    static constexpr uint32_t RadixMask = (uint32_t)RadixBuckets - 1u;
    static constexpr unsigned RadixPasses = 6;
    std::vector<Ref> scratch(totalEvents);
    std::vector<size_t> radixOffsets((size_t)workers * RadixBuckets);
    bool sourceIsRefs = true;
    unsigned radixDone = 0;

    auto radixPass = [&](auto&& keyOf, unsigned shift) {
        const std::vector<Ref>& src = sourceIsRefs ? refs : scratch;
        std::vector<Ref>& dst = sourceIsRefs ? scratch : refs;
        std::fill(radixOffsets.begin(), radixOffsets.end(), 0);

        RunWorkers(workers, [&](unsigned worker) {
            size_t* local = radixOffsets.data() + (size_t)worker * RadixBuckets;
            const size_t begin = totalEvents * (size_t)worker / workers;
            const size_t end = totalEvents * (size_t)(worker + 1) / workers;
            for (size_t i = begin; i < end; ++i)
                ++local[(keyOf(src[i]) >> shift) & RadixMask];
        });

        size_t prefix = 0;
        for (size_t bucket = 0; bucket < RadixBuckets; ++bucket) {
            size_t offset = prefix;
            for (unsigned worker = 0; worker < workers; ++worker) {
                const size_t index = (size_t)worker * RadixBuckets + bucket;
                const size_t count = radixOffsets[index];
                radixOffsets[index] = offset;
                offset += count;
            }
            prefix = offset;
        }

        const uint64_t progressTarget = (uint64_t)(radixDone + 1) * 2000 / RadixPasses;
        RunWorkers(workers, [&](unsigned worker) {
            size_t* write = radixOffsets.data() + (size_t)worker * RadixBuckets;
            const size_t begin = totalEvents * (size_t)worker / workers;
            const size_t end = totalEvents * (size_t)(worker + 1) / workers;
            for (size_t i = begin; i < end; ++i) {
                const Ref ref = src[i];
                const size_t bucket = (keyOf(ref) >> shift) & RadixMask;
                dst[write[bucket]++] = ref;
            }
            setProgress(worker, progressTarget);
        });
        ++radixDone;
        sourceIsRefs = !sourceIsRefs;
    };

    auto kindKey = [&](Ref ref) -> uint32_t { return eventKind(ref); };
    auto trackKey = [&](Ref ref) -> uint32_t { return eventTrackOf(ref); };
    auto tickKey = [&](Ref ref) -> uint32_t { return (uint32_t)eventTickOf(ref) ^ 0x80000000u; };

    radixPass(kindKey, 0);
    radixPass(trackKey, 0);
    radixPass(trackKey, 11);
    radixPass(tickKey, 0);
    radixPass(tickKey, 11);
    radixPass(tickKey, 22);

    if (!sourceIsRefs)
        refs.swap(scratch);
    std::vector<Ref>().swap(scratch);
    std::vector<size_t>().swap(radixOffsets);

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

    const size_t thinCount = m_vThinTicks.size();
    RunWorkers(workers, [&](unsigned worker) {
        const size_t begin = thinCount * (size_t)worker / workers;
        const size_t end = thinCount * (size_t)(worker + 1) / workers;
        const size_t span = (std::max)((size_t)1, end - begin);
        for (size_t thin = begin; thin < end; ++thin) {
            const int eventTick = (int)m_vThinTicks[thin];
            const size_t ai = anchorForTick(eventTick);
            const long long eventTime = timeFromAnchor(eventTick, anchors[ai]);
            const MIDIChannelEvent owner = (MIDIChannelEvent)(m_vThinOwners[thin] & ~THIN_OWNER_NOTEON_FLAG);
            if (m_vThinTicks[thin] == m_vTicks[owner])
                m_vThinLengths[thin] = 0;
            else
                m_vThinLengths[thin] = (uint32_t)(std::max)(0LL, eventTime - m_vTimes[owner]);
            if (((thin - begin) & 0xFFFFu) == 0)
                setProgress(worker, 3000 + (uint64_t)((thin - begin) * 1000 / span));
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
