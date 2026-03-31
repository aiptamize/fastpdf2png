// fastpdf2png — Shared memory for multi-process rendering
// SPDX-License-Identifier: MIT

#include "internal/shared_memory.h"

#ifndef _WIN32

#include <sys/mman.h>

namespace fpdf2png::internal {

void* MmapShared(size_t size) {
    auto* p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
}

} // namespace fpdf2png::internal

#endif // !_WIN32
