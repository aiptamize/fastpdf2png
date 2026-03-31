// fastpdf2png — Shared memory for multi-process rendering
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fpdf2png::internal {

#ifndef _WIN32
/// Allocate anonymous shared memory (MAP_SHARED | MAP_ANONYMOUS).
void* MmapShared(size_t size);
#endif

/// Cache-line aligned shared state for multi-process rendering.
struct alignas(64) SharedRenderState {
    std::atomic<int> next_page{0};
    int total_pages = 0;
};

/// Per-page result stored in shared memory.
struct SharedPageResult {
    int32_t width;
    int32_t height;
    int32_t stride;
    size_t  data_offset;
    int32_t valid;
};

} // namespace fpdf2png::internal
