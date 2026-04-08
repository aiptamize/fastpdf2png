// fastpdf2png — Daemon mode (stdin/stdout command loop)
// SPDX-License-Identifier: MIT

#include "cli/daemon_cmd.h"
#include "cli/args.h"
#include "cli/shared_cmd.h"
#include "cli/platform/render_platform.h"
#include "internal/pdfium_render.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "fpdfview.h"

namespace fpdf2png::cli {

int RunDaemon() {
    InitPdfium();

    char line[8192];
    while (std::fgets(line, sizeof(line), stdin)) {
        auto len = std::strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (std::strncmp(line, "QUIT", 4) == 0) break;

        char* tokens[8];
        const auto ntok = SplitTabs(line, tokens, 8);

        if (ntok >= 2 && std::string_view{tokens[0]} == "INFO") {
            auto* doc = FPDF_LoadDocument(tokens[1], nullptr);
            if (!doc) {
                std::printf("ERROR cannot open\n");
            } else {
                std::printf("OK %d\n", FPDF_GetPageCount(doc));
                FPDF_CloseDocument(doc);
            }
            std::fflush(stdout);
            continue;
        }

        if (ntok >= 3 && std::string_view{tokens[0]} == "RENDER") {
            const auto* pdf = tokens[1];
            const auto* pat = tokens[2];
            auto dpi = (ntok >= 4)
                ? static_cast<float>(std::atof(tokens[3])) : 150.0f;
            if (dpi <= 0 || dpi > kMaxDpi) dpi = 150.0f;
            const auto workers = (ntok >= 5)
                ? std::clamp(std::atoi(tokens[4]), 1, kMaxWorkers) : 1;
            const auto comp = (ntok >= 6)
                ? std::clamp(std::atoi(tokens[5]), -1, 2) : -1;

            auto* doc = FPDF_LoadDocument(pdf, nullptr);
            if (!doc) {
                std::printf("ERROR cannot open %s\n", pdf);
                std::fflush(stdout);
                continue;
            }
            const auto pages = FPDF_GetPageCount(doc);
            FPDF_CloseDocument(doc);

            int rc;
            if (workers > 1 && pages > 1)
                rc = RenderMulti(pdf, dpi, pat, pages,
                                 std::min(workers, pages), comp, false);
            else
                rc = RenderSingle(pdf, dpi, pat, pages, comp, false);

            if (rc != 0)
                std::printf("ERROR render failed for %s\n", pdf);
            else
                std::printf("OK %d\n", pages);
            std::fflush(stdout);
            continue;
        }

        std::printf("ERROR unknown command\n");
        std::fflush(stdout);
    }

    FPDF_DestroyLibrary();
    return 0;
}

} // namespace fpdf2png::cli
