// fastpdf2png — PDFium rendering internals
// SPDX-License-Identifier: MIT

#pragma once

#include "fastpdf2png/types.h"
#include "fpdfview.h"

#include <vector>

namespace fpdf2png::internal {

constexpr float kPointsPerInch = 72.0f;
constexpr int kNoAA = FPDF_RENDER_NO_SMOOTHTEXT |
                      FPDF_RENDER_NO_SMOOTHIMAGE |
                      FPDF_RENDER_NO_SMOOTHPATH;

/// Render pages [start, end) from an opened document to raw RGBA buffers.
std::vector<Page> RenderDoc(FPDF_DOCUMENT doc, int start, int end,
                            float dpi, bool no_aa);

/// Render a single page to a PNG file on disk.
bool RenderPageToFile(FPDF_DOCUMENT doc, int page_idx, float dpi,
                      const char* pattern, int compression,
                      bool no_aa = false);

} // namespace fpdf2png::internal
