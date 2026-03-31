// fastpdf2png — Shared CLI utilities
// SPDX-License-Identifier: MIT

#pragma once

#include <cstring>

#include "fpdfview.h"

namespace fpdf2png::cli {

inline void InitPdfium() {
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);
}

inline int SplitTabs(char* line, char** tokens, int max_tokens) {
    int count = 0;
    auto* p = line;
    while (count < max_tokens) {
        tokens[count++] = p;
        auto* tab = std::strchr(p, '\t');
        if (!tab) break;
        *tab = '\0';
        p = tab + 1;
    }
    return count;
}

} // namespace fpdf2png::cli
