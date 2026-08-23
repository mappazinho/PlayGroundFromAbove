#include "Globals.h"
#include "Renderer.h"
#include "ImageBufferMultipass.h"

// Compile the existing shared renderer unchanged under a legacy name for the
// one method this file replaces.
#define ImageBufferRenderChunk ImageBufferRenderChunkLegacy
#include "RendererBaseLegacy.inc"
#undef ImageBufferRenderChunk

bool Renderer::ImageBufferRenderChunk(long long chunk, const NoteData* notes, unsigned noteCount)
{
    if (!notes)
        noteCount = 0;
    if (ImageBufferChunkCached(chunk))
        return true;

    // A pass this size is large enough to amortize state changes but bounded
    // enough that a multi-million-note chunk cannot monopolize one frame.
    static constexpr size_t kImageBufferNotesPerPass = 200000;
    static constexpr size_t kImageBufferNotesPerFrame = 200000;

    auto& state = ImageBufferMPGet(this);
    if (state.generation != m_ullImageBufferGeneration) {
        state = ImageBufferMultipassState{};
        state.generation = m_ullImageBufferGeneration;
        state.frame = m_uImageBufferFrame;
    }

    if (state.frame != m_uImageBufferFrame) {
        state.frame = m_uImageBufferFrame;
        state.frameNotes = 0;
        // Normally EndScene clears this after recording the previous pass. If
        // the backend aborted, retry the same cursor rather than skipping it.
        state.requestQueued = false;
        state.backendActive = false;
        state.requestSlot = -1;
        state.requestChunk = ImageBufferInvalidChunk;
        state.requestCount = 0;
        state.requestClear = false;
        state.requestFinalize = false;
    }

    // Only one GPU chunk pass is queued in a frame. This also prevents the
    // existing 8-chunk lookahead loop from turning into 8 dense GPU jobs.
    if (state.requestQueued)
        return false;

    // A newly-visible missing chunk takes priority over an unfinished future
    // chunk. Release the reserved slot; its next first pass will clear stale
    // color/depth before restarting from note zero.
    if (state.slot >= 0 && state.chunk != chunk) {
        if (state.slot < (int)ChunkPoolSize &&
            m_ChunkCache[state.slot].generation == m_ullImageBufferGeneration &&
            m_ChunkCache[state.slot].chunk == ImageBufferInvalidChunk - 1) {
            m_ChunkCache[state.slot].chunk = ImageBufferInvalidChunk;
            m_ChunkCache[state.slot].generation = 0;
        }
        state.chunk = ImageBufferInvalidChunk;
        state.slot = -1;
        state.cursor = 0;
        state.total = 0;
    }

    if (noteCount > 0 && state.frameNotes >= kImageBufferNotesPerFrame)
        return false;

    if (state.slot < 0) {
        // Reservation happens earlier than the legacy backend allocation, before
        // this frame's visible quad list has been populated. Use LRU here rather
        // than the legacy lowest-chunk eviction rule: screens visible last frame
        // have the freshest lastUsed value, while the in-progress sentinel is
        // never considered an eviction candidate.
        int slot = -1;
        for (unsigned i = 0; i < ChunkPoolSize; ++i) {
            if (m_ChunkCache[i].generation != m_ullImageBufferGeneration ||
                m_ChunkCache[i].chunk == ImageBufferInvalidChunk) {
                slot = (int)i;
                break;
            }
        }
        if (slot < 0) {
            unsigned oldest = UINT_MAX;
            for (unsigned i = 0; i < ChunkPoolSize; ++i) {
                if (m_ChunkCache[i].generation != m_ullImageBufferGeneration ||
                    m_ChunkCache[i].chunk == ImageBufferInvalidChunk ||
                    m_ChunkCache[i].chunk == ImageBufferInvalidChunk - 1)
                    continue;
                if (m_ChunkCache[i].lastUsed < oldest) {
                    oldest = m_ChunkCache[i].lastUsed;
                    slot = (int)i;
                }
            }
        }
        if (slot < 0)
            return false;

        m_ChunkCache[slot].chunk = ImageBufferInvalidChunk - 1;
        m_ChunkCache[slot].generation = m_ullImageBufferGeneration;
        state.slot = slot;
        state.chunk = chunk;
        state.cursor = 0;
        state.total = noteCount;
    } else if (state.total != noteCount) {
        // The same chunk should be stable within a cache generation. If its
        // source count changes anyway, rebuild it from scratch safely.
        state.cursor = 0;
        state.total = noteCount;
    }

    if (state.cursor > state.total)
        state.cursor = 0;

    const size_t remaining = state.total - state.cursor;
    const size_t frameRemaining = kImageBufferNotesPerFrame - state.frameNotes;
    const size_t passCount = remaining == 0
        ? 0
        : min(remaining, min(kImageBufferNotesPerPass, frameRemaining));

    if (remaining > 0 && passCount == 0)
        return false;

    const unsigned noteOffset = (unsigned)m_vChunkNotes.size();
    if (passCount > 0)
        m_vChunkNotes.insert(m_vChunkNotes.end(), notes + state.cursor, notes + state.cursor + passCount);

    m_vChunkBuilds.push_back({ chunk, noteOffset, (unsigned)passCount });
    state.requestQueued = true;
    state.requestSlot = state.slot;
    state.requestChunk = chunk;
    state.requestCount = (unsigned)passCount;
    state.requestClear = (state.cursor == 0);
    state.requestFinalize = (state.cursor + passCount >= state.total);
    state.frameNotes += passCount;

    // Partial textures deliberately remain absent from ImageBufferGetChunkSlot.
    // Visible callers therefore take the existing normal-note fallback until
    // the final pass is recorded and the backend marks this slot baked.
    return state.requestFinalize;
}
