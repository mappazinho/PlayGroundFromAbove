#pragma once
#include <unordered_map>
#include <cstddef>
#include <vector>

// Shared CPU-side state for incremental image-buffer texture construction.
// One chunk is allowed to be in progress per Renderer instance. The render
// thread is the only writer, so no locking is required.
struct ImageBufferMultipassState {
    unsigned long long generation = 0;
    unsigned frame = 0;
    size_t frameNotes = 0;

    long long chunk = Renderer::ImageBufferInvalidChunk;
    int slot = -1;
    size_t cursor = 0;       // notes successfully committed by prior GPU passes
    size_t total = 0;
    std::vector<NoteData> notes; // CPU staging copy; built once, reused by later passes

    bool requestQueued = false;
    bool backendActive = false;
    int requestSlot = -1;
    long long requestChunk = Renderer::ImageBufferInvalidChunk;
    unsigned requestCount = 0;
    bool requestClear = false;
    bool requestFinalize = false;

    long long collectChunk = Renderer::ImageBufferInvalidChunk;
    unsigned collectCalls = 0;
};

inline std::unordered_map<Renderer*, ImageBufferMultipassState>& ImageBufferMPMap()
{
    static std::unordered_map<Renderer*, ImageBufferMultipassState> states;
    return states;
}

inline ImageBufferMultipassState& ImageBufferMPGet(Renderer* renderer)
{
    return ImageBufferMPMap()[renderer];
}

// GameState normally rebuilds a complete NoteData vector before every call to
// ImageBufferRenderChunk. Once a multipass chunk has its CPU staging copy, the
// first collection on the next frame can be skipped entirely. A second call in
// the same frame means the chunk is visible and needs the normal-note fallback,
// so provide the cached full vector instead of rescanning/reconverting events.
template <typename Collector>
inline void ImageBufferMPCollectDispatch(Renderer* renderer, std::vector<NoteData>& out,
                                         Collector& collector, long long chunk)
{
    auto& s = ImageBufferMPGet(renderer);
    if (s.chunk == chunk && !s.notes.empty()) {
        if (s.collectChunk != chunk) {
            s.collectChunk = chunk;
            s.collectCalls = 0;
        }
        if (s.collectCalls == 0) {
            s.collectCalls = 1;
            out.clear();
            return;
        }
        ++s.collectCalls;
        out = s.notes;
        return;
    }

    s.collectChunk = chunk;
    s.collectCalls = 1;
    collector(chunk);
}

inline int ImageBufferMPBeginBackend(Renderer* renderer)
{
    auto& s = ImageBufferMPGet(renderer);
    s.backendActive = s.requestQueued;
    return s.backendActive ? s.requestSlot : -1;
}

inline bool ImageBufferMPBackendActive(Renderer* renderer)
{
    return ImageBufferMPGet(renderer).backendActive;
}

inline int ImageBufferMPRequestSlot(Renderer* renderer)
{
    return ImageBufferMPGet(renderer).requestSlot;
}

inline bool ImageBufferMPRequestClear(Renderer* renderer)
{
    const auto& s = ImageBufferMPGet(renderer);
    return s.backendActive && s.requestClear;
}

inline bool ImageBufferMPRequestFinalize(Renderer* renderer)
{
    const auto& s = ImageBufferMPGet(renderer);
    return s.backendActive && s.requestFinalize;
}

// Called only after the backend has actually recorded the pass. Cursor progress
// is committed here instead of when it is queued, so a failed/aborted backend
// does not silently skip a slice on the next frame.
inline void ImageBufferMPEndBackend(Renderer* renderer)
{
    auto& s = ImageBufferMPGet(renderer);
    if (!s.backendActive)
        return;

    if (s.requestQueued) {
        if (s.requestFinalize) {
            s.chunk = Renderer::ImageBufferInvalidChunk;
            s.slot = -1;
            s.cursor = 0;
            s.total = 0;
            s.notes.clear();
        } else {
            s.cursor += s.requestCount;
        }
    }

    s.requestQueued = false;
    s.backendActive = false;
    s.requestSlot = -1;
    s.requestChunk = Renderer::ImageBufferInvalidChunk;
    s.requestCount = 0;
    s.requestClear = false;
    s.requestFinalize = false;
    s.collectChunk = Renderer::ImageBufferInvalidChunk;
    s.collectCalls = 0;
}
