#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

static constexpr size_t ImageBufferOverlapBlockEvents = 4096;
static constexpr size_t ImageBufferPreparedRawBlockNotes = 2048;

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
    bool timeOverflow = false;
    bool tickOverflow = false;
};

struct ImageBufferOverlapIndexState {
    const void* owner = nullptr;
    const MIDIChannelEvent* eventData = nullptr;
    size_t eventCount = 0;
    std::vector<long long> maxEndTime;
    std::vector<long long> maxEndTick;
    std::shared_ptr<const ImageBufferPreparedSource> preparedSource;
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

    if (state.owner == owner && state.eventData == data &&
        state.eventCount == events.size() && state.maxEndTime.size() == blockCount &&
        state.preparedSource)
        return state;

    state.owner = owner;
    state.eventData = data;
    state.eventCount = events.size();
    state.maxEndTime.assign(blockCount, (std::numeric_limits<long long>::min)());
    state.maxEndTick.assign(blockCount, (std::numeric_limits<long long>::min)());

    auto source = std::make_shared<ImageBufferPreparedSource>();
    // NoteOn/NoteOff streams are commonly close to 50/50. Reserve without
    // constructing so huge MIDIs avoid repeated vector reallocations.
    source->notes.reserve(events.size() / 2);

    for (size_t i = 0; i < events.size(); ++i) {
        const MIDIChannelEvent event = events[i];
        if (midi.GetEventChannelEventType(event) != MIDI::NoteOn ||
            midi.GetEventParam2(event) <= 0 || !midi.EventHasSister(event))
            continue;

        const size_t sisterIndex = (size_t)midi.GetEventSisterIdx(event);
        if (sisterIndex >= events.size())
            continue;
        const MIDIChannelEvent sister = events[sisterIndex];

        const long long startTime = midi.GetEventTime(event);
        const long long lengthTime = (std::max)(0LL, midi.GetEventLength(event));
        const long long startTickLL = midi.GetEventAbsT(event);
        const long long endTickLL = midi.GetEventAbsT(sister);
        const long long lengthTickLL = (std::max)(0LL, endTickLL - startTickLL);

        const long long worstTime = ImageBufferOverlapWorstEnd(startTime, lengthTime);
        const long long worstTick = ImageBufferOverlapWorstEnd(startTickLL, lengthTickLL);
        const size_t eventBlock = i / ImageBufferOverlapBlockEvents;
        if (worstTime > state.maxEndTime[eventBlock])
            state.maxEndTime[eventBlock] = worstTime;
        if (worstTick > state.maxEndTick[eventBlock])
            state.maxEndTick[eventBlock] = worstTick;

        ImageBufferPreparedRawNote raw;
        raw.seed = ImageBufferOverlapSeed(event, sister);
        raw.track = (uint16_t)midi.GetEventTrack(event);
        raw.key = (uint8_t)midi.GetEventParam1(event);
        raw.channel = (uint8_t)midi.GetEventChannel(event);

        if (startTime < 0 || lengthTime < 0) {
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

        if (startTickLL < 0 || lengthTickLL < 0 ||
            (uint64_t)startTickLL > (std::numeric_limits<uint32_t>::max)() ||
            (uint64_t)lengthTickLL > (std::numeric_limits<uint32_t>::max)()) {
            source->tickOverflow = true;
        } else {
            raw.startTick = (uint32_t)startTickLL;
            raw.lengthTick = (uint32_t)lengthTickLL;
        }

        const size_t rawIndex = source->notes.size();
        const size_t rawBlock = rawIndex / ImageBufferPreparedRawBlockNotes;
        if (rawBlock >= source->maxEndTime150_100us.size()) {
            source->maxEndTime150_100us.push_back(0);
            source->maxEndTick150.push_back(0);
        }

        const uint64_t worst100 = (uint64_t)raw.start100us +
            (uint64_t)raw.length100us + ((uint64_t)raw.length100us + 1ULL) / 2ULL;
        const uint64_t worstRawTick = (uint64_t)raw.startTick +
            (uint64_t)raw.lengthTick + ((uint64_t)raw.lengthTick + 1ULL) / 2ULL;
        if (worst100 > source->maxEndTime150_100us[rawBlock])
            source->maxEndTime150_100us[rawBlock] = worst100;
        if (worstRawTick > source->maxEndTick150[rawBlock])
            source->maxEndTick150[rawBlock] = worstRawTick;

        source->notes.push_back(raw);
    }

    state.preparedSource = std::move(source);
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
        const long long blockMaxEnd = tickMode ? state.maxEndTick[block] : state.maxEndTime[block];
        if (blockMaxEnd >= oldestUsefulEnd) {
            const size_t begin = block * ImageBufferOverlapBlockEvents;
            const size_t end = (std::min)(hi, begin + ImageBufferOverlapBlockEvents);
            for (size_t i = end; i != begin; ) {
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
