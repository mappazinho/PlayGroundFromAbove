from pathlib import Path


def load(path):
    return Path(path).read_text(encoding="utf-8")


def save(path, text):
    Path(path).write_text(text, encoding="utf-8")


def replace_once(path, old, new, label):
    text = load(path)
    n = text.count(old)
    if n != 1:
        raise RuntimeError(f"{label}: expected 1 match, got {n}")
    save(path, text.replace(old, new, 1))


overlap = "PianoFromAbove/ImageBufferOverlapIndex.h"
replace_once(
    overlap,
    "static constexpr size_t ImageBufferOverlapBlockEvents = 4096;\nstatic constexpr size_t ImageBufferPreparedRawBlockNotes = 2048;",
    "static constexpr size_t ImageBufferOverlapBlockEvents = 4096;\nstatic constexpr size_t ImageBufferOverlapSubBlockEvents = 64;\nstatic constexpr size_t ImageBufferPreparedRawBlockNotes = 2048;\nstatic constexpr size_t ImageBufferPreparedRawSubBlockNotes = 64;",
    "sub-block constants",
)
replace_once(
    overlap,
    "    std::vector<uint64_t> maxEndTime150_100us;\n    std::vector<uint64_t> maxEndTick150;\n    std::vector<uint64_t> prefixMaxEndTime150_100us;",
    "    std::vector<uint64_t> maxEndTime150_100us;\n    std::vector<uint64_t> maxEndTick150;\n    std::vector<uint64_t> subMaxEndTime150_100us;\n    std::vector<uint64_t> subMaxEndTick150;\n    std::vector<uint64_t> prefixMaxEndTime150_100us;",
    "prepared sub-block fields",
)
replace_once(
    overlap,
    "    std::vector<long long> maxEndTime;\n    std::vector<long long> maxEndTick;\n    std::vector<long long> prefixMaxEndTime;",
    "    std::vector<long long> maxEndTime;\n    std::vector<long long> maxEndTick;\n    std::vector<long long> subMaxEndTime;\n    std::vector<long long> subMaxEndTick;\n    std::vector<long long> prefixMaxEndTime;",
    "exact sub-block fields",
)
replace_once(
    overlap,
    "    const size_t blockCount =\n        (events.size() + ImageBufferOverlapBlockEvents - 1) / ImageBufferOverlapBlockEvents;\n    const uint64_t firstEvent",
    "    const size_t blockCount =\n        (events.size() + ImageBufferOverlapBlockEvents - 1) / ImageBufferOverlapBlockEvents;\n    const size_t subBlockCount =\n        (events.size() + ImageBufferOverlapSubBlockEvents - 1) / ImageBufferOverlapSubBlockEvents;\n    const uint64_t firstEvent",
    "exact sub-block count",
)
replace_once(
    overlap,
    "        state.lastEvent == lastEvent && state.firstTime == firstTime &&\n        state.lastTime == lastTime && state.maxEndTime.size() == blockCount &&\n        state.preparedAttempted)",
    "        state.lastEvent == lastEvent && state.firstTime == firstTime &&\n        state.lastTime == lastTime && state.maxEndTime.size() == blockCount &&\n        state.subMaxEndTime.size() == subBlockCount && state.preparedAttempted)",
    "sub-block cache identity",
)
replace_once(
    overlap,
    "    state.maxEndTime.assign(blockCount, (std::numeric_limits<long long>::min)());\n    state.maxEndTick.assign(blockCount, (std::numeric_limits<long long>::min)());\n    state.prefixMaxEndTime.resize(blockCount);",
    "    state.maxEndTime.assign(blockCount, (std::numeric_limits<long long>::min)());\n    state.maxEndTick.assign(blockCount, (std::numeric_limits<long long>::min)());\n    state.subMaxEndTime.assign(subBlockCount, (std::numeric_limits<long long>::min)());\n    state.subMaxEndTick.assign(subBlockCount, (std::numeric_limits<long long>::min)());\n    state.prefixMaxEndTime.resize(blockCount);",
    "exact sub-block allocation",
)
replace_once(
    overlap,
    "        const size_t eventBlock = i / ImageBufferOverlapBlockEvents;\n        if (worstTime > state.maxEndTime[eventBlock])\n            state.maxEndTime[eventBlock] = worstTime;\n        if (worstTick > state.maxEndTick[eventBlock])\n            state.maxEndTick[eventBlock] = worstTick;",
    "        const size_t eventBlock = i / ImageBufferOverlapBlockEvents;\n        const size_t eventSubBlock = i / ImageBufferOverlapSubBlockEvents;\n        if (worstTime > state.maxEndTime[eventBlock])\n            state.maxEndTime[eventBlock] = worstTime;\n        if (worstTick > state.maxEndTick[eventBlock])\n            state.maxEndTick[eventBlock] = worstTick;\n        if (worstTime > state.subMaxEndTime[eventSubBlock])\n            state.subMaxEndTime[eventSubBlock] = worstTime;\n        if (worstTick > state.subMaxEndTick[eventSubBlock])\n            state.subMaxEndTick[eventSubBlock] = worstTick;",
    "exact sub-block maxima",
)
replace_once(
    overlap,
    "            const size_t rawBlock = rawIndex / ImageBufferPreparedRawBlockNotes;\n            if (rawBlock >= source->maxEndTime150_100us.size()) {\n                source->maxEndTime150_100us.push_back(0);\n                source->maxEndTick150.push_back(0);\n            }",
    "            const size_t rawBlock = rawIndex / ImageBufferPreparedRawBlockNotes;\n            const size_t rawSubBlock = rawIndex / ImageBufferPreparedRawSubBlockNotes;\n            if (rawBlock >= source->maxEndTime150_100us.size()) {\n                source->maxEndTime150_100us.push_back(0);\n                source->maxEndTick150.push_back(0);\n            }\n            if (rawSubBlock >= source->subMaxEndTime150_100us.size()) {\n                source->subMaxEndTime150_100us.push_back(0);\n                source->subMaxEndTick150.push_back(0);\n            }",
    "prepared sub-block allocation",
)
replace_once(
    overlap,
    "            if (worst100 > source->maxEndTime150_100us[rawBlock])\n                source->maxEndTime150_100us[rawBlock] = worst100;\n            if (worstRawTick > source->maxEndTick150[rawBlock])\n                source->maxEndTick150[rawBlock] = worstRawTick;",
    "            if (worst100 > source->maxEndTime150_100us[rawBlock])\n                source->maxEndTime150_100us[rawBlock] = worst100;\n            if (worstRawTick > source->maxEndTick150[rawBlock])\n                source->maxEndTick150[rawBlock] = worstRawTick;\n            if (worst100 > source->subMaxEndTime150_100us[rawSubBlock])\n                source->subMaxEndTime150_100us[rawSubBlock] = worst100;\n            if (worstRawTick > source->subMaxEndTick150[rawSubBlock])\n                source->subMaxEndTick150[rawSubBlock] = worstRawTick;",
    "prepared sub-block maxima",
)

old_exact = """        if (blockMaxEnd >= oldestUsefulEnd) {
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
"""
new_exact = """        if (blockMaxEnd >= oldestUsefulEnd) {
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
"""
replace_once(overlap, old_exact, new_exact, "exact two-level scan")

prepared = "PianoFromAbove/ImageBufferPreparedChunks.h"
text = load(prepared)
old_head = """        if (blockMax >= oldest) {
            const size_t begin = block * ImageBufferPreparedRawBlockNotes;
            const size_t end = (std::min)(hi, begin + ImageBufferPreparedRawBlockNotes);
            for (size_t i = end; i != begin; ) {
                --i;
                const ImageBufferPreparedRawNote& raw = source.notes[i];
                ++result.rawVisited;
"""
new_head = """        if (blockMax >= oldest) {
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
"""
if text.count(old_head) != 1:
    raise RuntimeError(f"prepared two-level head: expected 1 match, got {text.count(old_head)}")
text = text.replace(old_head, new_head, 1)
old_tail = """                if (remainingCells == 0)
                    break;
            }
        }

        if (remainingCells == 0 || block == 0)
"""
new_tail = """                        if (remainingCells == 0)
                            break;
                    }
                }
                if (remainingCells == 0 || subBlock == firstSubBlock)
                    break;
                --subBlock;
            }
        }

        if (remainingCells == 0 || block == 0)
"""
if text.count(old_tail) != 1:
    raise RuntimeError(f"prepared two-level tail: expected 1 match, got {text.count(old_tail)}")
text = text.replace(old_tail, new_tail, 1)
save(prepared, text)

Path(".github/scripts/apply-image-buffer-subblocks.py").unlink()
Path(".github/workflows/final-image-buffer-subblocks.yml").unlink()
