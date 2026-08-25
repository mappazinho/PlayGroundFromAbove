#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

static constexpr size_t ImageBufferOverlapBlockEvents = 4096;
static constexpr size_t ImageBufferOverlapSubBlockEvents = 64;
static constexpr size_t ImageBufferPreparedRawBlockNotes = 2048;
static constexpr size_t ImageBufferPreparedRawSubBlockNotes = 64;

struct ImageBufferPreparedRawNote {
    uint64_t seed = 0;
    uint32_t start100us = 0;
    uint32_t length100us = 0;
    uint32_t startTick = 0;
    uint32_t lengthTick = 0;
    uint16_t track = 0;
    uint8_t key = 0;
    uint8_t channel = 0;
};

struct ImageBufferPreparedSource {
    std::vector<ImageBufferPreparedRawNote> notes;
    // Conservative block maxima assuming corruption can lengthen a note by up
    // to 50%. The caller separately subtracts the start-shift margin.
    std::vector<uint64_t> maxEndTime150_100us;
    std::vector<uint64_t> maxEndTick150;
    std::vector<uint64_t> subMaxEndTime150_100us;
    std::vector<uint64_t> subMaxEndTick150;
    std::vector<uint64_t> prefixMaxEndTime150_100us;
    std::vector<uint64_t> prefixMaxEndTick150;
    bool timeOverflow = false;
    bool tickOverflow = false;
};

struct ImageBufferOverlapIndexState {
    const void* owner = nullptr;
    const MIDIChannelEvent* eventData = nullptr;
    size_t eventCount = 0;
    uint64_t firstEvent = 0;
    uint64_t lastEvent = 0;
    long long firstTime = 0;
    long long lastTime = 0;
    std::vector<long long> maxEndTime;
    std::vector<long long> maxEndTick;
    std::vector<long long> subMaxEndTime;
    std::vector<long long> subMaxEndTick;
    std::vector<long long> prefixMaxEndTime;
    std::vector<long long> prefixMaxEndTick;
    std::shared_ptr<const ImageBufferPreparedSource> preparedSource;
    bool preparedAttempted = false;
};

inline ImageBufferOverlapIndexState& ImageBufferOverlapIndexGet()
{
    static ImageBufferOverlapIndexState state;
    return state;
}

inline long long ImageBufferOverlapSaturatingAdd(long long a, long long b)
{
    if (b > 0 && a > (std::numeric_limits<long long>::max)() - b)
        return (std::numeric_limits<long long>::max)();
    if (b < 0 && a < (std::numeric_limits<long long>::min)() - b)
        return (std::numeric_limits<long long>::min)();
    return a + b;
}

inline uint64_t ImageBufferOverlapSeed(MIDIChannelEvent note, MIDIChannelEvent sister)
{
    uint64_t h = (std::min)((uint64_t)note, (uint64_t)sister);
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h;
}

inline long long ImageBufferOverlapWorstEnd(long long start, long long length)
{
    length = (std::max)(0LL, length);
    const long long extra = length / 2 + (length & 1);
    return ImageBufferOverlapSaturatingAdd(start,
        ImageBufferOverlapSaturatingAdd(length, extra));
}

template <typename MidiT>
inline ImageBufferOverlapIndexState& ImageBufferOverlapEnsureIndex(
    const void* owner,
    const std::vector<MIDIChannelEvent>& events,
    MidiT& midi)
{
    auto& state = ImageBufferOverlapIndexGet();
    const MIDIChannelEvent* const data = events.empty() ? nullptr : events.data();
    const size_t blockCount =
        (events.size() + ImageBufferOverlapBlockEvents - 1) / ImageBufferOverlapBlockEvents;
    const size_t subBlockCount =
        (events.size() + ImageBufferOverlapSubBlockEvents - 1) / ImageBufferOverlapSubBlockEvents;
    const uint64_t firstEvent = events.empty() ? 0 : (uint64_t)events.front();
    const uint64_t lastEvent = events.empty() ? 0 : (uint64_t)events.back();
    const long long firstTime = events.empty() ? 0 : midi.GetEventTime(events.front());
    const long long lastTime = events.empty() ? 0 : midi.GetEventTime(events.back());

    // Include cheap content probes as well as owner/storage identity. An old and
    // new MainScreen can otherwise reuse the same allocator addresses and event
    // count, which would make a stale prepared source look current.
    if (state.owner == owner && state.eventData == data &&
        state.eventCount == events.size() && state.firstEvent == firstEvent &&
        state.lastEvent == lastEvent && state.firstTime == firstTime &&
        state.lastTime == lastTime && state.maxEndTime.size() == blockCount &&
        state.subMaxEndTime.size() == subBlockCount && state.preparedAttempted)
        return state;

    state.owner = owner;
    state.eventData = data;
    state.eventCount = events.size();
    state.firstEvent = firstEvent;
    state.lastEvent = lastEvent;
    state.firstTime = firstTime;
    state.lastTime = lastTime;
    state.maxEndTime.assign(blockCount, (std::numeric_limits<long long>::min)());
    state.maxEndTick.assign(blockCount, (std::numeric_limits<long long>::min)());
    state.subMaxEndTime.assign(subBlockCount, (std::numeric_limits<long long>::min)());
    state.subMaxEndTick.assign(subBlockCount, (std::numeric_limits<long long>::min)());
    state.prefixMaxEndTime.resize(blockCount);
    state.prefixMaxEndTick.resize(blockCount);
    state.preparedSource.reset();
    state.preparedAttempted = false;

    std::shared_ptr<ImageBufferPreparedSource> source;
    try {
        source = std::make_shared<ImageBufferPreparedSource>();
        // NoteOn/NoteOff streams are commonly close to 50/50. Reserving once
        // avoids repeated 100+ MB reallocations on black MIDIs. If this reserve
        // cannot be satisfied, image buffering still has the exact overlap path.
        source->notes.reserve(events.size() / 2);
    } catch (const std::bad_alloc&) {
        source.reset();
    }

    for (size_t i = 0; i < events.size(); ++i) {
        const MIDIChannelEvent event = events[i];
        if (midi.GetEventChannelEventType(event) != MIDI::NoteOn ||
            midi.GetEventParam2(event) <= 0 || !midi.EventHasSister(event))
            continue;

        // GetEventSisterIdx returns a MIDI row id, not an index into the
        // time-sorted m_vEvents vector. Use that row directly for tick/end data.
        const MIDIChannelEvent sister = (MIDIChannelEvent)midi.GetEventSisterIdx(event);

        const long long startTime = midi.GetEventTime(event);
        const long long lengthTime = (std::max)(0LL, (long long)midi.GetEventLength(event));
        const long long startTickLL = midi.GetEventAbsT(event);
        const long long endTickLL = midi.GetEventAbsT(sister);
        const long long lengthTickLL = (std::max)(0LL, endTickLL - startTickLL);

        const long long worstTime = ImageBufferOverlapWorstEnd(startTime, lengthTime);
        const long long worstTick = ImageBufferOverlapWorstEnd(startTickLL, lengthTickLL);
        const size_t eventBlock = i / ImageBufferOverlapBlockEvents;
        const size_t eventSubBlock = i / ImageBufferOverlapSubBlockEvents;
        if (worstTime > state.maxEndTime[eventBlock])
            state.maxEndTime[eventBlock] = worstTime;
        if (worstTick > state.maxEndTick[eventBlock])
            state.maxEndTick[eventBlock] = worstTick;
        if (worstTime > state.subMaxEndTime[eventSubBlock])
            state.subMaxEndTime[eventSubBlock] = worstTime;
        if (worstTick > state.subMaxEndTick[eventSubBlock])
            state.subMaxEndTick[eventSubBlock] = worstTick;

        if (!source)
            continue;

        try {
            ImageBufferPreparedRawNote raw;
            raw.seed = ImageBufferOverlapSeed(event, sister);
            raw.track = (uint16_t)midi.GetEventTrack(event);
            raw.key = (uint8_t)midi.GetEventParam1(event);
            raw.channel = (uint8_t)midi.GetEventChannel(event);

            if (startTime < 0) {
                source->timeOverflow = true;
            } else {
                const uint64_t start100 = (uint64_t)startTime / 100ULL;
                const uint64_t length100 = ((uint64_t)lengthTime + 99ULL) / 100ULL;
                if (start100 > (std::numeric_limits<uint32_t>::max)() ||
                    length100 > (std::numeric_limits<uint32_t>::max)()) {
                    source->timeOverflow = true;
                } else {
                    raw.start100us = (uint32_t)start100;
                    raw.length100us = (uint32_t)length100;
                }
            }

            if (startTickLL < 0 ||
                (uint64_t)startTickLL > (std::numeric_limits<uint32_t>::max)() ||
                (uint64_t)lengthTickLL > (std::numeric_limits<uint32_t>::max)()) {
                source->tickOverflow = true;
            } else {
                raw.startTick = (uint32_t)startTickLL;
                raw.lengthTick = (uint32_t)lengthTickLL;
            }

            const size_t rawIndex = source->notes.size();
            source->notes.push_back(raw);
            const size_t rawBlock = rawIndex / ImageBufferPreparedRawBlockNotes;
            const size_t rawSubBlock = rawIndex / ImageBufferPreparedRawSubBlockNotes;
            if (rawBlock >= source->maxEndTime150_100us.size()) {
                source->maxEndTime150_100us.push_back(0);
                source->maxEndTick150.push_back(0);
            }
            if (rawSubBlock >= source->subMaxEndTime150_100us.size()) {
                source->subMaxEndTime150_100us.push_back(0);
                source->subMaxEndTick150.push_back(0);
            }

            const uint64_t worst100 = (uint64_t)raw.start100us +
                (uint64_t)raw.length100us + ((uint64_t)raw.length100us + 1ULL) / 2ULL;
            const uint64_t worstRawTick = (uint64_t)raw.startTick +
                (uint64_t)raw.lengthTick + ((uint64_t)raw.lengthTick + 1ULL) / 2ULL;
            if (worst100 > source->maxEndTime150_100us[rawBlock])
                source->maxEndTime150_100us[rawBlock] = worst100;
            if (worstRawTick > source->maxEndTick150[rawBlock])
                source->maxEndTick150[rawBlock] = worstRawTick;
            if (worst100 > source->subMaxEndTime150_100us[rawSubBlock])
                source->subMaxEndTime150_100us[rawSubBlock] = worst100;
            if (worstRawTick > source->subMaxEndTick150[rawSubBlock])
                source->subMaxEndTick150[rawSubBlock] = worstRawTick;
        } catch (const std::bad_alloc&) {
            // Preparation is an optimization. Keep the exact block index and
            // abandon the compact source instead of failing song playback.
            source.reset();
        }
    }

    long long prefixTime = (std::numeric_limits<long long>::min)();
    long long prefixTick = (std::numeric_limits<long long>::min)();
    for (size_t i = 0; i < blockCount; ++i) {
        prefixTime = (std::max)(prefixTime, state.maxEndTime[i]);
        prefixTick = (std::max)(prefixTick, state.maxEndTick[i]);
        state.prefixMaxEndTime[i] = prefixTime;
        state.prefixMaxEndTick[i] = prefixTick;
    }
    if (source) {
        const size_t rawBlocks = source->maxEndTime150_100us.size();
        source->prefixMaxEndTime150_100us.resize(rawBlocks);
        source->prefixMaxEndTick150.resize(rawBlocks);
        uint64_t prefixRawTime = 0;
        uint64_t prefixRawTick = 0;
        for (size_t i = 0; i < rawBlocks; ++i) {
            prefixRawTime = (std::max)(prefixRawTime, source->maxEndTime150_100us[i]);
            prefixRawTick = (std::max)(prefixRawTick, source->maxEndTick150[i]);
            source->prefixMaxEndTime150_100us[i] = prefixRawTime;
            source->prefixMaxEndTick150[i] = prefixRawTick;
        }
    }

    state.preparedSource = std::move(source);
    state.preparedAttempted = true;
    return state;
}

template <typename MidiT, typename BuildFn, typename Visitor>
inline void ImageBufferOverlapVisit(
    const void* owner,
    const std::vector<MIDIChannelEvent>& events,
    MidiT& midi,
    long long chunk,
    long long timeSpan,
    long long corruptionMargin,
    bool tickMode,
    BuildFn&& buildNote,
    Visitor&& visitor)
{
    if (events.empty() || timeSpan <= 0)
        return;

    auto& state = ImageBufferOverlapEnsureIndex(owner, events, midi);
    const long long chunkStart = chunk * timeSpan;
    const long long chunkEnd = ImageBufferOverlapSaturatingAdd(chunkStart, timeSpan);
    const long long hiTime = ImageBufferOverlapSaturatingAdd(chunkEnd, corruptionMargin);
    const long long oldestUsefulEnd = ImageBufferOverlapSaturatingAdd(chunkStart, -corruptionMargin);

    auto itHi = std::lower_bound(events.begin(), events.end(), hiTime,
        [&](MIDIChannelEvent lhs, long long rhs) {
            return (tickMode ? midi.GetEventAbsT(lhs) : midi.GetEventTime(lhs)) < rhs;
        });
    const size_t hi = (size_t)(itHi - events.begin());
    if (hi == 0)
        return;

    size_t block = (hi - 1) / ImageBufferOverlapBlockEvents;
    for (;;) {
        const long long prefixMaxEnd = tickMode ? state.prefixMaxEndTick[block] : state.prefixMaxEndTime[block];
        if (prefixMaxEnd < oldestUsefulEnd)
            break;
        const long long blockMaxEnd = tickMode ? state.maxEndTick[block] : state.maxEndTime[block];
        if (blockMaxEnd >= oldestUsefulEnd) {
            const size_t begin = block * ImageBufferOverlapBlockEvents;
            const size_t end = (std::min)(hi, begin + ImageBufferOverlapBlockEvents);
            size_t subBlock = (end - 1) / ImageBufferOverlapSubBlockEvents;
            const size_t firstSubBlock = begin / ImageBufferOverlapSubBlockEvents;
            for (;;) {
                const long long subMaxEnd = tickMode
                    ? state.subMaxEndTick[subBlock]
                    : state.subMaxEndTime[subBlock];
                if (subMaxEnd >= oldestUsefulEnd) {
                    const size_t subBegin = (std::max)(begin,
                        subBlock * ImageBufferOverlapSubBlockEvents);
                    const size_t subEnd = (std::min)(end,
                        (subBlock + 1) * ImageBufferOverlapSubBlockEvents);
                    for (size_t i = subEnd; i != subBegin; ) {
                        --i;
                        const MIDIChannelEvent event = events[i];
                        if (midi.GetEventChannelEventType(event) != MIDI::NoteOn ||
                            midi.GetEventParam2(event) <= 0 || !midi.EventHasSister(event))
                            continue;

                        NoteData data = buildNote(event, chunkStart);
                        if (data.pos < (float)timeSpan &&
                            data.pos + (std::max)(data.length, 0.0f) >= 0.0f)
                            visitor(data);
                    }
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
}

template <typename MidiT, typename BuildFn>
inline void ImageBufferOverlapCollect(
    const void* owner,
    const std::vector<MIDIChannelEvent>& events,
    MidiT& midi,
    std::vector<NoteData>& out,
    long long chunk,
    long long timeSpan,
    long long corruptionMargin,
    bool tickMode,
    BuildFn&& buildNote)
{
    out.clear();
    ImageBufferOverlapVisit(owner, events, midi, chunk, timeSpan, corruptionMargin,
        tickMode, std::forward<BuildFn>(buildNote),
        [&](const NoteData& data) { out.push_back(data); });
}
