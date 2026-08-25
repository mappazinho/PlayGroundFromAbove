from pathlib import Path


def replace_once(path, old, new, label):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected 1 match, got {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# Huge-image-buffer polish -------------------------------------------------
replace_once(
    "PianoFromAbove/GameState.cpp",
    'sprintf_s(log, "imgprep:skip-full events=%zu notes=%zu fallback=local", events, notes);',
    'sprintf_s(log, "imghuge:enabled events=%zu notes=%zu", events, notes);',
    "huge mode log")

replace_once(
    "PianoFromAbove/GameState.cpp",
    '''static void ImageBufferCollectHugeIndexedCompact(\n    const std::vector<MIDIChannelEvent>& events,\n    MIDI& midi,\n    std::vector<NoteData>& out,\n    long long chunk,\n    long long timeSpan,\n    long long corruptionMargin,\n    bool tickMode,\n    int rows,\n    const std::vector<long long>& maxEndTime,''',
    '''static void ImageBufferCollectHugeIndexedCompact(\n    const std::vector<MIDIChannelEvent>& events,\n    MIDI& midi,\n    std::vector<NoteData>& out,\n    long long chunk,\n    long long timeSpan,\n    long long corruptionMargin,\n    bool tickMode,\n    int rows,\n    bool stablePitch,\n    const std::vector<long long>& maxEndTime,''',
    "compact stable pitch parameter")

replace_once(
    "PianoFromAbove/GameState.cpp",
    '''    size_t remaining = (size_t)128 * (size_t)rows;\n    size_t block = (std::min)((hi - 1) / ImageBufferHugeBlockEvents, blockMax.size() - 1);''',
    '''    int firstKey = 0;\n    int lastKey = 127;\n    if (stablePitch && midi.GetInfo().iMinNote >= 0 &&\n        midi.GetInfo().iMaxNote >= midi.GetInfo().iMinNote) {\n        firstKey = (std::min)((std::max)((int)midi.GetInfo().iMinNote, 0), 127);\n        lastKey = (std::min)((std::max)((int)midi.GetInfo().iMaxNote, firstKey), 127);\n    }\n    int remainingByKey[128] = {};\n    for (int key = firstKey; key <= lastKey; ++key)\n        remainingByKey[key] = rows;\n    size_t remaining = (size_t)(lastKey - firstKey + 1) * (size_t)rows;\n    size_t block = (std::min)((hi - 1) / ImageBufferHugeBlockEvents, blockMax.size() - 1);''',
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
    '''static void ImageBufferCollectHugeAdaptive(\n    const std::vector<MIDIChannelEvent>& events,\n    MIDI& midi,\n    std::vector<NoteData>& out,\n    long long chunk,\n    long long timeSpan,\n    long long corruptionMargin,\n    bool tickMode,\n    int rows,\n    const std::vector<long long>& maxEndTime,''',
    '''static void ImageBufferCollectHugeAdaptive(\n    const std::vector<MIDIChannelEvent>& events,\n    MIDI& midi,\n    std::vector<NoteData>& out,\n    long long chunk,\n    long long timeSpan,\n    long long corruptionMargin,\n    bool tickMode,\n    int rows,\n    bool stablePitch,\n    const std::vector<long long>& maxEndTime,''',
    "adaptive stable pitch parameter")

replace_once(
    "PianoFromAbove/GameState.cpp",
    '''    ImageBufferCollectHugeIndexedCompact(\n        events, midi, out, chunk, timeSpan, corruptionMargin, tickMode, rows,\n        maxEndTime, maxEndTick, prefixEndTime, prefixEndTick,\n        std::forward<BuildFn>(buildNote), std::forward<HiddenFn>(isHidden));\n}''',
    '''    const bool logDense = g_bLoggingEnabled.load(std::memory_order_relaxed);\n    const auto compactStart = logDense ? std::chrono::steady_clock::now()\n        : std::chrono::steady_clock::time_point{};\n    ImageBufferCollectHugeIndexedCompact(\n        events, midi, out, chunk, timeSpan, corruptionMargin, tickMode, rows, stablePitch,\n        maxEndTime, maxEndTick, prefixEndTime, prefixEndTick,\n        std::forward<BuildFn>(buildNote), std::forward<HiddenFn>(isHidden));\n    if (logDense) {\n        const double ms = std::chrono::duration<double, std::milli>(\n            std::chrono::steady_clock::now() - compactStart).count();\n        static std::atomic<unsigned long long> lastHugeCompactLog{ 0 };\n        const unsigned long long nowMs = GetTickCount64();\n        unsigned long long lastMs = lastHugeCompactLog.load(std::memory_order_relaxed);\n        if (nowMs - lastMs >= 1000 &&\n            lastHugeCompactLog.compare_exchange_strong(lastMs, nowMs,\n                std::memory_order_relaxed)) {\n            char log[192];\n            sprintf_s(log, "imghuge:compact chunk=%lld candidates=%zu compact=%zu ms=%.1f",\n                chunk, estimate, out.size(), ms);\n            HeartbeatLog(log);\n        }\n    }\n}''',
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


# Prerender SDL watchdog ----------------------------------------------------
# The callback timestamp used to start at zero. SDL is opened paused, then the
# game thread unpauses it and immediately polls PRE_AudioStalled(); with a process
# uptime already >2 seconds that treated "no first callback yet" as a dead device.
# SDL_CloseAudio/OpenAudio then ran synchronously every game frame (~90 ms here),
# pinning the UI at roughly 10 FPS and preventing the callback from ever starting.
replace_once(
    "PianoFromAbove/MIDIPreRenderPlayer.cpp",
    "static volatile long long s_llLastCallbackTick = 0;",
    "static std::atomic<long long> s_llLastCallbackTick{ 0 };",
    "atomic audio watchdog timestamp")

replace_once(
    "PianoFromAbove/MIDIPreRenderPlayer.cpp",
    "\ts_llLastCallbackTick = SDL_GetTicks64();",
    "\ts_llLastCallbackTick.store((long long)SDL_GetTicks64(), std::memory_order_relaxed);",
    "callback watchdog publish")

replace_once(
    "PianoFromAbove/MIDIPreRenderPlayer.cpp",
    '''bool PRE_AudioStalled()\n{\n\tlong long now = SDL_GetTicks64();\n\tlong long last = s_llLastCallbackTick;\n\treturn (now - last) > 2000;\n}\n\nvoid PRE_RestartAudio()\n{\n\tPRE_DbgLog("RestartAudio: err='%s' lastCB=%lldms", SDL_GetError() ? SDL_GetError() : "(null)", (long long)s_llLastCallbackTick);\n\tSDL_CloseAudio();\n\tPRE_OpenDevice();\n}''',
    '''void PRE_TouchAudio()\n{\n\ts_llLastCallbackTick.store((long long)SDL_GetTicks64(), std::memory_order_relaxed);\n}\n\nbool PRE_AudioStalled()\n{\n\t// A paused device is intentionally not producing callbacks. The game thread\n\t// touches the timestamp while paused, and PRE_OpenDevice() establishes a fresh\n\t// startup grace period before the first callback is expected.\n\tif (SDL_GetAudioStatus() != SDL_AUDIO_PLAYING)\n\t\treturn false;\n\tconst long long now = (long long)SDL_GetTicks64();\n\tconst long long last = s_llLastCallbackTick.load(std::memory_order_relaxed);\n\tif (last <= 0) {\n\t\tPRE_TouchAudio();\n\t\treturn false;\n\t}\n\treturn (now - last) > 2500;\n}\n\nvoid PRE_RestartAudio()\n{\n\tconst long long last = s_llLastCallbackTick.load(std::memory_order_relaxed);\n\tPRE_DbgLog("RestartAudio: err='%s' lastCB=%lldms",\n\t\tSDL_GetError() ? SDL_GetError() : "(null)", last);\n\tSDL_CloseAudio();\n\tPRE_OpenDevice();\n}''',
    "audio watchdog grace and atomic read")

replace_once(
    "PianoFromAbove/MIDIPreRenderPlayer.cpp",
    '''\tSDL_PauseAudio(1);\n\tPRE_DbgLog("PRE_InitAudio: SDL audio driver='%s'", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "(null)");''',
    '''\tSDL_PauseAudio(1);\n\t// Opening/reopening is not a stall. Give WASAPI time to dispatch its first\n\t// callback after the game thread resumes the device.\n\tPRE_TouchAudio();\n\tPRE_DbgLog("PRE_InitAudio: SDL audio driver='%s'", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "(null)");''',
    "audio open watchdog grace")

replace_once(
    "PianoFromAbove/GameStateLegacy.inc",
    '''    if (!cAudio.bPreRenderAudio)\n    {\n        if (PRE_MIDIAudio)\n            SDL_PauseAudio(1);\n        return;\n    }''',
    '''    if (!cAudio.bPreRenderAudio)\n    {\n        if (PRE_MIDIAudio)\n            SDL_PauseAudio(1);\n        PRE_TouchAudio();\n        return;\n    }''',
    "watchdog touch when prerender disabled")

replace_once(
    "PianoFromAbove/GameStateLegacy.inc",
    '''    if (s_bPreFailed)\n    {\n        if (PRE_MIDIAudio)\n            SDL_PauseAudio(1);\n        return;\n    }''',
    '''    if (s_bPreFailed)\n    {\n        if (PRE_MIDIAudio)\n            SDL_PauseAudio(1);\n        PRE_TouchAudio();\n        return;\n    }''',
    "watchdog touch after prerender fallback")

replace_once(
    "PianoFromAbove/GameStateLegacy.inc",
    '''    else\n    {\n        SDL_PauseAudio(1);\n    }\n}\n\nGameState::GameError SplashScreen::Logic()''',
    '''    else\n    {\n        SDL_PauseAudio(1);\n        // Pausing intentionally stops callbacks; keep the watchdog baseline fresh\n        // so resume cannot be mistaken for a multi-second device stall.\n        PRE_TouchAudio();\n    }\n}\n\nGameState::GameError SplashScreen::Logic()''',
    "watchdog touch while paused")
