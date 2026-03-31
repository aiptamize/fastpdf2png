// fastpdf2png — Platform-specific render declarations
// SPDX-License-Identifier: MIT

#pragma once

namespace fpdf2png::cli {

/// Single-process rendering (all platforms).
int RenderSingle(const char* pdf_path, float dpi, const char* pattern,
                 int pages, int compression, bool no_aa);

/// Multi-process rendering (fork on Unix, CreateProcess on Windows).
int RenderMulti(const char* pdf_path, float dpi, const char* pattern,
                int pages, int workers, int compression, bool no_aa);

#ifdef _WIN32
/// Windows shared-memory worker entry point (invoked via --worker).
int RunWindowsWorker(const char* pdf_path, float dpi, const char* pattern,
                     int compression, const char* shm_name, bool no_aa);
#endif

} // namespace fpdf2png::cli
