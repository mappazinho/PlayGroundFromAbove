from pathlib import Path


def replace_once(path, old, new, label):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected 1 match, got {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "PianoFromAbove/GameState.cpp",
    'sprintf_s(log, "imgprep:skip-full events=%zu notes=%zu fallback=local", events, notes);',
    'sprintf_s(log, "imghuge:enabled events=%zu notes=%zu", events, notes);',
    "huge mode log")

replace_once(
    "PianoFromAbove/GameState.cpp",
    '''    bool tickMode,\n    int rows,\n    const std::vector<long long>& maxEndTime,''',
    '''    bool tickMode,\n    int rows,\n    bool stablePitch,\n    const std::vector<long long>& maxEndTime,''',
    "compact stable pitch parameter")

replace_once(
    "PianoFromAbove/GameState.cpp",
    '''    size_t remaining = (size_t)128 * (size_t)rows;\n    size_t block = (std::min)((hi - 1) / ImageBufferHugeBlockEvents, blockMax.size() - 1);''',
    '''    int firstKey = 0;\n    int lastKey = 127;\n    if (stablePitch && midi.GetInfo().iMinNote >= 0 &&\n        midi.GetInfo().iMaxNote >= midi.GetInfo().iMinNote) {\n        firstKey = (std::min)((std::max)(midi.GetInfo().iMinNote, 0), 127);\n        lastKey = (std::min)((std::max)(midi.GetInfo().iMaxNote, firstKey), 127);\n    }\n    int remainingByKey[128] = {};\n    for (int key = firstKey; key <= lastKey; ++key)\n        remainingByKey[key] = rows;\n    size_t remaining = (size_t)(lastKey - firstKey + 1) * (size_t)rows;\n    size_t block = (std::min)((hi - 1) / ImageBufferHugeBlockEvents, blockMax.size() - 1);''',
    "compact active key range")

replace_once(
    "PianoFromAbove/GameState.cpp",
    '''                if (midi.GetEventChannelEventType(event) != MIDI::NoteOn ||\n                    midi.GetEventParam2(event) <= 0 || !midi.EventHasSister(event))\n                    continue;\n\n                NoteData data = buildNote(event, chunkStart);\n                if (data.key >= 128 || isHidden(data.track, data.channel))\n                    continue;''',
    '''                if (midi.GetEventChannelEventType(event) != MIDI::NoteOn ||\n                    midi.GetEventParam2(event) <= 0 || !midi.EventHasSister(event))\n                    continue;\n                if (stablePitch) {\n                    const int sourceKey = midi.GetEventParam1(event);\n                    if (sourceKey < firstKey || sourceKey > lastKey ||\n                        remainingByKey[sourceKey] == 0)\n                        continue;\n                }\n\n                NoteData data = buildNote(event, chunkStart);\n                if (data.key >= 128 || data.key < firstKey || data.key > lastKey ||\n                    remainingByKey[data.key] == 0 || isHidden(data.track, data.channel))\n                    continue;''',
    "compact completed-key skip")

replace_once(
    "PianoFromAbove/GameState.cpp",
    '''                        next[base + current] = following;\n                        --remaining;\n                        runEnd = current + 1;''',
    '''                        next[base + current] = following;\n                        --remaining;\n                        --remainingByKey[data.key];\n                        runEnd = current + 1;''',
    "compact key remaining decrement")

replace_once(
    "PianoFromAbove/GameState.cpp",
    '''    bool tickMode,\n    int rows,\n    const std::vector<long long>& maxEndTime,\n    const std::vector<long long>& maxEndTick,\n    const std::vector<long long>& prefixEndTime,\n    const std::vector<long long>& prefixEndTick,\n    BuildFn&& buildNote,\n    HiddenFn&& isHidden)\n{\n    const size_t estimate = ImageBufferHugeEstimateCandidates(''',
    '''    bool tickMode,\n    int rows,\n    bool stablePitch,\n    const std::vector<long long>& maxEndTime,\n    const std::vector<long long>& maxEndTick,\n    const std::vector<long long>& prefixEndTime,\n    const std::vector<long long>& prefixEndTick,\n    BuildFn&& buildNote,\n    HiddenFn&& isHidden)\n{\n    const size_t estimate = ImageBufferHugeEstimateCandidates(''',
    "adaptive stable pitch parameter")

replace_once(
    "PianoFromAbove/GameState.cpp",
    '''    ImageBufferCollectHugeIndexedCompact(\n        events, midi, out, chunk, timeSpan, corruptionMargin, tickMode, rows,\n        maxEndTime, maxEndTick, prefixEndTime, prefixEndTick,\n        std::forward<BuildFn>(buildNote), std::forward<HiddenFn>(isHidden));\n}''',
    '''    const bool logDense = g_bLoggingEnabled.load(std::memory_order_relaxed);\n    const auto compactStart = logDense ? std::chrono::steady_clock::now()\n        : std::chrono::steady_clock::time_point{};\n    ImageBufferCollectHugeIndexedCompact(\n        events, midi, out, chunk, timeSpan, corruptionMargin, tickMode, rows, stablePitch,\n        maxEndTime, maxEndTick, prefixEndTime, prefixEndTick,\n        std::forward<BuildFn>(buildNote), std::forward<HiddenFn>(isHidden));\n    if (logDense) {\n        const double ms = std::chrono::duration<double, std::milli>(\n            std::chrono::steady_clock::now() - compactStart).count();\n        char log[192];\n        sprintf_s(log, "imghuge:compact chunk=%lld candidates=%zu compact=%zu ms=%.1f",\n            chunk, estimate, out.size(), ms);\n        HeartbeatLog(log);\n    }\n}''',
    "adaptive dense diagnostics")

replace_once(
    "PianoFromAbove/GameState.cpp",
    '''            events, midi, notes, chunk, timeSpan, corruptionMargin, tickMode, rows,\n            maxEndTime, maxEndTick, prefixEndTime, prefixEndTick,''',
    '''            events, midi, notes, chunk, timeSpan, corruptionMargin, tickMode, rows,\n            corruption <= 0.0f, maxEndTime, maxEndTick, prefixEndTime, prefixEndTick,''',
    "prewarm stable pitch call")

replace_once(
    "PianoFromAbove/GameState.cpp",
    '''    }\n\n    long long bakeChunk = Renderer::ImageBufferInvalidChunk;\n    {\n        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);''',
    '''    }\n\n    std::vector<long long> pinnedChunks;\n    {\n        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);\n        pinnedChunks = s_ImageBufferPrewarmGpu.chunks;\n    }\n    ImageBufferSetPinnedChunks(renderer, pinnedChunks);\n\n    long long bakeChunk = Renderer::ImageBufferInvalidChunk;\n    {\n        std::lock_guard<std::mutex> lock(s_ImageBufferPrewarmGpuMutex);''',
    "huge prewarm repin")

replace_once(
    "PianoFromAbove/GameState.cpp",
    '''            ImageBufferCollectHugeAdaptive(m_vEvents, m_MIDI, chunkNotes, imageBufferChunk, T, E, bTickMode, \\\n                imageBufferHugeRows, m_vImageBufferMaxEndTime, m_vImageBufferMaxEndTick, \\\n''',
    '''            ImageBufferCollectHugeAdaptive(m_vEvents, m_MIDI, chunkNotes, imageBufferChunk, T, E, bTickMode, \\\n                imageBufferHugeRows, fCorrupt <= 0.0f, m_vImageBufferMaxEndTime, m_vImageBufferMaxEndTick, \\\n''',
    "runtime stable pitch call")

replace_once(
    "PianoFromAbove/GameState.cpp",
    '''                if (imageBufferPrewarmAnyHidden) { \\\n                    ImageBufferPreparedMarkPrewarmUnavailable(imageBufferPrewarmSelf); \\\n                } else { \\\n''',
    '''                if (imageBufferPrewarmAnyHidden && ImageBufferFullPreparedSupported( \\\n                    imageBufferPrewarmSelf->m_vEvents, imageBufferPrewarmSelf->m_MIDI)) { \\\n                    ImageBufferPreparedMarkPrewarmUnavailable(imageBufferPrewarmSelf); \\\n                } else { \\\n''',
    "allow hidden huge prewarm")
