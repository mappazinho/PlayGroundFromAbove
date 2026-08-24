#pragma once
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Renderer;

// Dense chunks explicitly warmed before playback are a residency contract, not
// ordinary speculative lookahead. Keep them out of the renderer's LRU victim
// set so sparse chunks encountered earlier in the song cannot evict the work we
// just waited for. The registry is renderer-local and is replaced atomically
// whenever a new prewarm set is published.
struct ImageBufferGpuPinRegistry {
    std::mutex mutex;
    std::unordered_map<Renderer*, std::unordered_set<long long>> chunks;
};

inline ImageBufferGpuPinRegistry& ImageBufferGpuPins()
{
    static ImageBufferGpuPinRegistry registry;
    return registry;
}

inline void ImageBufferSetPinnedChunks(Renderer* renderer, const std::vector<long long>& chunks)
{
    if (!renderer)
        return;
    auto& registry = ImageBufferGpuPins();
    std::lock_guard<std::mutex> lock(registry.mutex);
    if (chunks.empty()) {
        registry.chunks.erase(renderer);
        return;
    }
    auto& dst = registry.chunks[renderer];
    dst.clear();
    dst.reserve(chunks.size());
    dst.insert(chunks.begin(), chunks.end());
}

inline void ImageBufferClearPinnedChunks(Renderer* renderer)
{
    if (!renderer)
        return;
    auto& registry = ImageBufferGpuPins();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.chunks.erase(renderer);
}

inline bool ImageBufferChunkPinned(Renderer* renderer, long long chunk)
{
    auto& registry = ImageBufferGpuPins();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto found = registry.chunks.find(renderer);
    return found != registry.chunks.end() && found->second.find(chunk) != found->second.end();
}
