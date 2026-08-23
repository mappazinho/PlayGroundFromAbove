#pragma once

// Keep the original renderer declaration intact, but add a private implementation
// name for ImageBufferRenderChunk so RendererBase.cpp can wrap only that method
// without duplicating the large shared renderer source file.
#define ImageBufferRenderChunk(...) ImageBufferRenderChunkLegacy(__VA_ARGS__); bool ImageBufferRenderChunk(__VA_ARGS__)
#include "RendererLegacy.h"
#undef ImageBufferRenderChunk
