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

// Let the backend sample an in-progress slot after this frame's pass has been
// recorded. While requestQueued is true this helper deliberately returns -1 so
// the build loop does not mistake the partial slot for a completed cache entry.
inline int ImageBufferMPDrawableSlot(Renderer* renderer, int cachedSlot, long long chunk)
{
    if (cachedSlot >= 0)
        return cachedSlot;

    const auto& s = ImageBufferMPGet(renderer);
    if (!s.requestQueued && s.slot >= 0 && s.chunk == chunk && s.cursor > 0)
        return s.slot;
    return -1;
}

// A normal-render fallback above this size is more expensive than simply
// waiting for the image-buffer chunk to become drawable. Keeping this bounded
// prevents a dense visible chunk from rendering millions of notes normally
// while also spending a multipass GPU slice on the same frame.
static constexpr size_t ImageBufferMPMaxNormalFallbackNotes = 100000;

// GameState normally rebuilds a complete NoteData vector before every call to
// ImageBufferRenderChunk. Once a multipass chunk has its CPU staging copy, the
// first collection on later frames can be skipped entirely. Visible in-progress
// chunks are drawn from their partial texture, so they never need the full
// normal-note fallback. If another visible chunk could not start because this
// frame's one multipass request is already occupied, keep the ordinary fallback
// only when that chunk is small enough to be cheap.
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

        // First call this frame is BakeChunk's collection. The renderer already
        // owns the complete CPU staging vector, so avoid rebuilding it.
        if (s.collectCalls == 0) {
            s.collectCalls = 1;
            out.clear();
            return;
        }

        // A second call is GameState asking for visible fallback after the bake
        // returned incomplete. Do not copy/push millions of notes through the
        // regular renderer; the backend will draw the accumulated partial slot.
        ++s.collectCalls;
        out.clear();
        return;
    }

    // A second collection of a different chunk in the same frame means its
    // BakeChunk could not claim the one multipass request. Reuse the vector that
    // is already in `out`, but suppress it when it would be an expensive normal
    // fallback. It can begin its own multipass on a following frame.
    if (s.collectChunk == chunk && s.collectCalls > 0) {
        ++s.collectCalls;
        if (out.size() > ImageBufferMPMaxNormalFallbackNotes)
            out.clear();
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
