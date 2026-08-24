#pragma once

// Keep the original renderer declaration intact, but add a private implementation
// name for ImageBufferRenderChunk so RendererBase.cpp can wrap only that method
// without duplicating the large shared renderer source file.
#define ImageBufferRenderChunk(...) ImageBufferRenderChunkLegacy(__VA_ARGS__); bool ImageBufferRenderChunk(__VA_ARGS__)

// The legacy header keeps PushNoteData inline. Split that one declaration into
// a filtered public entry point plus the untouched legacy body so the shared
// renderer can cheaply discard fully occluded notes only on absurdly dense
// direct-render frames without duplicating RendererLegacy.h.
#define PushNoteData(...) PushNoteData(__VA_ARGS__) { \
    if (PushNoteDataFiltered(data)) return; \
    m_vNotesIntermediate.push_back(data); \
} \
bool PushNoteDataFiltered(NoteData data); \
void PushNoteDataLegacy(__VA_ARGS__)

#include "RendererLegacy.h"

#undef PushNoteData
#undef ImageBufferRenderChunk
