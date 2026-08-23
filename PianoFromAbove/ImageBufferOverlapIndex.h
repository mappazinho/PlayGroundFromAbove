#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

// The image-buffer collector used to search back by the single longest note in
// the whole MIDI. One long note therefore made every later chunk revisit the
// entire historical event prefix. For black MIDIs this becomes millions of
// event-type/sister checks per chunk, even when only a handful of notes are
// currently visible.
//
// Keep a tiny interval summary over the existing sorted event vector instead:
// each fixed-size block stores the latest raw note end in microseconds and
// ticks. A chunk query walks blocks newest -> oldest (preserving draw order) and
// skips a whole block when every drawable note in it ended before the chunk can
// begin, including the corruption margin. Only surviving blocks are inspected
// event-by-event.
static constexpr size_t ImageBufferOverlapBlockEvents = 4096;

struct ImageBufferOverlapIndexState {
    const void* owner = nullptr;
    const MIDIChannelEvent* eventData = nullptr;
    size_t eventCount = 0;
    std::vector<long long> maxEndTime;
    std::vector<long long> maxEndTick;
};

inline ImageBufferOverlapIndexState& ImageBufferOverlapIndexGet()
{
    // Only the active MainScreen renders. Keeping one index avoids retaining a
    // block table for every previously loaded MIDI while async loading may still
    // construct a replacement MainScreen in the background.
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
        state.eventCount == events.size() && state.maxEndTime.size() == blockCount)
        return state;

    state.owner = owner;
    state.eventData = data;
    state.eventCount = events.size();
    state.maxEndTime.assign(blockCount, (std::numeric_limits<long long>::min)());
    state.maxEndTick.assign(blockCount, (std::numeric_limits<long long>::min)());

    // This is intentionally built lazily on the first image-buffer render. The
    // player renders the newly loaded MainScreen while still paused, so the one
    // O(N) pass happens once at screen initialization instead of once per chunk.
    // It also avoids enlarging the already expensive MIDI PostProcess peak.
    for (size_t i = 0; i < events.size(); ++i) {
        const MIDIChannelEvent event = events[i];
        if (midi.GetEventChannelEventType(event) != MIDI::NoteOn ||
            midi.GetEventParam2(event) <= 0 || !midi.EventHasSister(event))
            continue;

        const size_t sister = (size_t)midi.GetEventSisterIdx(event);
        if (sister >= events.size())
            continue;

        const long long startTime = midi.GetEventTime(event);
        const long long lengthTime = midi.GetEventLength(event);
        const long long endTime = ImageBufferOverlapSaturatingAdd(startTime, std::max(0LL, lengthTime));
        const long long endTick = midi.GetEventAbsT(events[sister]);
        const size_t block = i / ImageBufferOverlapBlockEvents;

        if (endTime > state.maxEndTime[block])
            state.maxEndTime[block] = endTime;
        if (endTick > state.maxEndTick[block])
            state.maxEndTick[block] = endTick;
    }

    return state;
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
    if (events.empty() || timeSpan <= 0)
        return;

    auto& state = ImageBufferOverlapEnsureIndex(owner, events, midi);
    const long long chunkStart = chunk * timeSpan;
    const long long chunkEnd = ImageBufferOverlapSaturatingAdd(chunkStart, timeSpan);
    const long long hiTime = ImageBufferOverlapSaturatingAdd(chunkEnd, corruptionMargin);
    const long long oldestUsefulEnd = ImageBufferOverlapSaturatingAdd(chunkStart, -corruptionMargin);

    // Events are already sorted in both time domains. This upper boundary is
    // the same one the old collector used; notes starting later cannot corrupt
    // backwards far enough to enter this chunk.
    auto itHi = std::lower_bound(events.begin(), events.end(), hiTime,
        [&](MIDIChannelEvent lhs, long long rhs) {
            return (tickMode ? midi.GetEventAbsT(lhs) : midi.GetEventTime(lhs)) < rhs;
        });
    const size_t hi = (size_t)(itHi - events.begin());
    if (hi == 0)
        return;

    // Walk newest -> oldest exactly like normal rendering. A block whose latest
    // possible (raw + corruption margin) note end is still before chunkStart
    // cannot contribute anything and is skipped without touching its events.
    size_t block = (hi - 1) / ImageBufferOverlapBlockEvents;
    for (;;) {
        const long long blockMaxEnd = tickMode ? state.maxEndTick[block] : state.maxEndTime[block];
        if (blockMaxEnd >= oldestUsefulEnd) {
            const size_t begin = block * ImageBufferOverlapBlockEvents;
            const size_t end = std::min(hi, begin + ImageBufferOverlapBlockEvents);
            for (size_t i = end; i != begin; ) {
                --i;
                const MIDIChannelEvent event = events[i];
                if (midi.GetEventChannelEventType(event) != MIDI::NoteOn ||
                    midi.GetEventParam2(event) <= 0 || !midi.EventHasSister(event))
                    continue;

                NoteData data = buildNote(event, chunkStart);
                if (data.pos < (float)timeSpan &&
                    data.pos + std::max(data.length, 0.0f) >= 0.0f)
                    out.push_back(data);
            }
        }

        if (block == 0)
            break;
        --block;
    }
}
