// fastpdf2png — PDFium rendering internals
// SPDX-License-Identifier: MIT

#include "internal/pdfium_render.h"
#include "png/png_writer.h"
#include "png/memory_pool.h"

#include <cstdio>
#include <cstdlib>

#include "fpdfview.h"
#include "fpdf_edit.h"

namespace fpdf2png::internal {

std::vector<Page> RenderDoc(FPDF_DOCUMENT doc, int start, int end,
                            float dpi, bool no_aa) {
    std::vector<Page> pages;
    pages.reserve(end - start);

    for (int i = start; i < end; ++i) {
        auto* page = FPDF_LoadPage(doc, i);
        if (!page) continue;

        const auto scale = dpi / kPointsPerInch;
        const auto w = static_cast<int>(FPDF_GetPageWidth(page) * scale + 0.5f);
        const auto h = static_cast<int>(FPDF_GetPageHeight(page) * scale + 0.5f);
        if (w <= 0 || h <= 0) { FPDF_ClosePage(page); continue; }

        const auto stride = (w * 4 + 63) & ~63;
        const auto buf_size = static_cast<size_t>(stride) * h;
        auto* buffer = static_cast<uint8_t*>(std::malloc(buf_size));
        if (!buffer) { FPDF_ClosePage(page); continue; }

        auto* bitmap = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRx, buffer, stride);
        if (!bitmap) { std::free(buffer); FPDF_ClosePage(page); continue; }

        int flags = FPDF_PRINTING | FPDF_REVERSE_BYTE_ORDER;
        if (no_aa) flags |= kNoAA;
        FPDFBitmap_FillRect(bitmap, 0, 0, w, h, 0xFFFFFFFF);
        FPDF_RenderPageBitmap(bitmap, page, 0, 0, w, h, 0, flags);
        FPDFBitmap_Destroy(bitmap);
        FPDF_ClosePage(page);

        pages.emplace_back(buffer, w, h, stride);
    }
    return pages;
}

bool RenderPageToFile(FPDF_DOCUMENT doc, int page_idx, float dpi,
                      const char* pattern, int compression, bool no_aa) {
    auto* page = FPDF_LoadPage(doc, page_idx);
    if (!page) return false;

    const auto scale = dpi / kPointsPerInch;
    const auto width  = static_cast<int>(FPDF_GetPageWidth(page) * scale + 0.5f);
    const auto height = static_cast<int>(FPDF_GetPageHeight(page) * scale + 0.5f);

    if (width <= 0 || height <= 0) {
        FPDF_ClosePage(page);
        return false;
    }

    const auto stride = (width * 4 + 63) & ~63;
    const auto buf_size = static_cast<size_t>(stride) * height;

    auto& pool = fast_png::GetProcessLocalPool();
    auto* buffer = pool.Acquire(buf_size);
    if (!buffer) { FPDF_ClosePage(page); return false; }

    auto* bitmap = FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRx, buffer, stride);
    if (!bitmap) { FPDF_ClosePage(page); return false; }

    int flags = FPDF_PRINTING | FPDF_REVERSE_BYTE_ORDER;
    if (no_aa) flags |= kNoAA;
    FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0, flags);

    char path[4096];
    std::snprintf(path, sizeof(path), pattern, page_idx + 1);

    const auto result = fast_png::WriteRgba(path, buffer, width, height, stride, compression);
    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);

    return result == fast_png::kSuccess;
}

} // namespace fpdf2png::internal
