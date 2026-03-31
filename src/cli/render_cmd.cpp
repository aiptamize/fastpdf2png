// fastpdf2png — Single-file render command (dispatcher)
// SPDX-License-Identifier: MIT

#include "cli/render_cmd.h"
#include "cli/shared_cmd.h"
#include "cli/platform/render_platform.h"

#include <cstdio>
#include <chrono>

#include "fpdfview.h"

namespace fpdf2png::cli {

int RunRender(const ParsedArgs& args) {
    InitPdfium();

    auto* doc = FPDF_LoadDocument(args.pdf_path, nullptr);
    if (!doc) {
        std::fprintf(stderr, "Failed to open PDF: %s (error %lu)\n",
                     args.pdf_path, FPDF_GetLastError());
        FPDF_DestroyLibrary();
        return 1;
    }

    const auto pages = FPDF_GetPageCount(doc);
    FPDF_CloseDocument(doc);

    if (pages <= 0) {
        std::fprintf(stderr, "PDF has no pages\n");
        FPDF_DestroyLibrary();
        return 1;
    }

    const auto t0 = std::chrono::high_resolution_clock::now();

    const auto result = (args.workers > 1)
        ? RenderMulti(args.pdf_path, args.dpi, args.pattern, pages,
                      args.workers, args.compression, args.no_aa)
        : RenderSingle(args.pdf_path, args.dpi, args.pattern, pages,
                       args.compression, args.no_aa);

    FPDF_DestroyLibrary();

    const auto t1 = std::chrono::high_resolution_clock::now();
    const auto elapsed = std::chrono::duration<double>(t1 - t0).count();

    if (result == 0)
        std::printf("Rendered %d pages in %.3f seconds (%.1f pages/sec)\n",
                    pages, elapsed, pages / elapsed);

    return result;
}

} // namespace fpdf2png::cli
