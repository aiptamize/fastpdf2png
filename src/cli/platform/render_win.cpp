// fastpdf2png — Windows multi-process rendering (CreateProcess-based)
// SPDX-License-Identifier: MIT

#ifdef _WIN32

#include "cli/platform/render_platform.h"
#include "cli/shared_cmd.h"
#include "internal/pdfium_render.h"

#include <cstdio>
#include <vector>

#define NOMINMAX
#include <windows.h>

#include "fpdfview.h"

namespace fpdf2png::cli {

// ---------------------------------------------------------------------------
// Shared state — cache-line padded to prevent false sharing
// ---------------------------------------------------------------------------

struct SharedState {
    volatile LONG next_page;
    char pad1[60];
    volatile LONG completed_pages;
    char pad2[60];
    int total_pages;
};

static int ClaimNextPage(SharedState* s)  { return InterlockedExchangeAdd(&s->next_page, 1); }
static void MarkCompleted(SharedState* s) { InterlockedExchangeAdd(&s->completed_pages, 1); }
static int GetCompleted(SharedState* s)   { return InterlockedCompareExchange(&s->completed_pages, 0, 0); }

// ---------------------------------------------------------------------------
// Single-process rendering
// ---------------------------------------------------------------------------

int RenderSingle(const char* pdf_path, float dpi, const char* pattern,
                 int pages, int compression, bool no_aa) {
    auto* doc = FPDF_LoadDocument(pdf_path, nullptr);
    if (!doc) {
        std::fprintf(stderr, "Failed to open: %s\n", pdf_path);
        return 1;
    }

    int rendered = 0;
    for (int i = 0; i < pages; ++i) {
        if (internal::RenderPageToFile(doc, i, dpi, pattern, compression, no_aa))
            ++rendered;
    }

    FPDF_CloseDocument(doc);
    return (rendered == pages) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Multi-process rendering (Windows)
// ---------------------------------------------------------------------------

int RenderMulti(const char* pdf_path, float dpi, const char* pattern,
                int pages, int workers, int compression, bool no_aa) {
    char shm_name[64];
    std::snprintf(shm_name, sizeof(shm_name), "fastpdf2png_%lu",
                  static_cast<unsigned long>(GetCurrentProcessId()));

    auto hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                   0, sizeof(SharedState), shm_name);
    if (!hMap) {
        std::fprintf(stderr, "CreateFileMapping failed (%lu)\n", GetLastError());
        return 1;
    }

    auto* shared = static_cast<SharedState*>(
        MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!shared) { CloseHandle(hMap); return 1; }

    shared->next_page = 0;
    shared->completed_pages = 0;
    shared->total_pages = pages;

    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);

    const auto dpi_x10 = static_cast<int>(dpi * 10 + 0.5f);

    std::vector<HANDLE> children;
    children.reserve(workers);

    for (int i = 0; i < workers; ++i) {
        char cmdline[8192];
        std::snprintf(cmdline, sizeof(cmdline),
                      "\"%s\" --worker \"%s\" \"%s\" %d %d \"%s\" %d",
                      exe_path, pdf_path, pattern, dpi_x10, compression, shm_name,
                      no_aa ? 1 : 0);

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        if (CreateProcessA(nullptr, cmdline, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            children.push_back(pi.hProcess);
        } else {
            std::fprintf(stderr, "CreateProcess failed for worker %d (%lu)\n", i, GetLastError());
        }
    }

    if (!children.empty()) {
        auto wait = WaitForMultipleObjects(
            static_cast<DWORD>(children.size()), children.data(), TRUE, INFINITE);
        if (wait == WAIT_FAILED)
            std::fprintf(stderr, "WaitForMultipleObjects failed (%lu)\n", GetLastError());
    }
    for (auto h : children) CloseHandle(h);

    const auto completed = GetCompleted(shared);
    UnmapViewOfFile(shared);
    CloseHandle(hMap);
    return (completed == pages) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Windows shared-memory worker entry point (invoked via --worker)
// ---------------------------------------------------------------------------

int RunWindowsWorker(const char* pdf_path, float dpi, const char* pattern,
                     int compression, const char* shm_name, bool no_aa) {
    InitPdfium();

    auto hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, shm_name);
    if (!hMap) {
        std::fprintf(stderr, "Worker: OpenFileMapping failed (%lu)\n", GetLastError());
        FPDF_DestroyLibrary();
        return 1;
    }

    auto* shared = static_cast<SharedState*>(
        MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!shared) { CloseHandle(hMap); FPDF_DestroyLibrary(); return 1; }

    auto* doc = FPDF_LoadDocument(pdf_path, nullptr);
    if (!doc) {
        UnmapViewOfFile(shared);
        CloseHandle(hMap);
        FPDF_DestroyLibrary();
        return 1;
    }

    while (true) {
        const auto page = ClaimNextPage(shared);
        if (page >= shared->total_pages) break;
        if (internal::RenderPageToFile(doc, page, dpi, pattern, compression, no_aa))
            MarkCompleted(shared);
    }

    FPDF_CloseDocument(doc);
    UnmapViewOfFile(shared);
    CloseHandle(hMap);
    FPDF_DestroyLibrary();
    return 0;
}

} // namespace fpdf2png::cli

#endif // _WIN32
