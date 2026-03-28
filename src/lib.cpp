// libfastpdf2png — Implementation
// SPDX-License-Identifier: MIT

#define FASTPDF2PNG_BUILD_DLL
#include "lib.h"
#include "png_writer.h"
#include "memory_pool.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "fpdfview.h"
#include "fpdf_edit.h"

namespace fpdf2png {

namespace {

constexpr float kPointsPerInch = 72.0f;
constexpr int kNoAA = FPDF_RENDER_NO_SMOOTHTEXT | FPDF_RENDER_NO_SMOOTHIMAGE |
                      FPDF_RENDER_NO_SMOOTHPATH;

// Read file into memory
std::pair<uint8_t*, size_t> ReadFile(std::string_view path) {
    std::string p(path);
    auto* f = std::fopen(p.c_str(), "rb");
    if (!f) return {nullptr, 0};
    std::fseek(f, 0, SEEK_END);
    auto size = static_cast<size_t>(std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    auto* buf = static_cast<uint8_t*>(std::malloc(size));
    if (!buf) { std::fclose(f); return {nullptr, 0}; }
    std::fread(buf, 1, size, f);
    std::fclose(f);
    return {buf, size};
}

// Render pages from an already-opened doc
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

#ifndef _WIN32
// Shared state for multi-process rendering to memory
struct alignas(64) SharedRenderState {
    std::atomic<int> next_page{0};
    int total_pages = 0;
};

// Worker renders pages into shared memory segments
struct SharedPageResult {
    int32_t width, height, stride;
    size_t data_offset;  // offset into shared data buffer
    int32_t valid;       // 1 if rendered successfully
};
#endif

} // namespace

// --- Engine implementation ---

struct Engine::Impl {
    bool initialized = false;
};

Engine::Engine() : impl_(std::make_unique<Impl>()) {
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);
    impl_->initialized = true;
}

Engine::~Engine() {
    if (impl_ && impl_->initialized)
        FPDF_DestroyLibrary();
}

int Engine::page_count(std::string_view pdf_path) const {
    auto [data, size] = ReadFile(pdf_path);
    if (!data) return -1;
    auto* doc = FPDF_LoadMemDocument64(data, size, nullptr);
    if (!doc) { std::free(data); return -1; }
    int n = FPDF_GetPageCount(doc);
    FPDF_CloseDocument(doc);
    std::free(data);
    return n;
}

int Engine::page_count(std::span<const uint8_t> pdf_data) const {
    auto* doc = FPDF_LoadMemDocument64(pdf_data.data(), pdf_data.size(), nullptr);
    if (!doc) return -1;
    int n = FPDF_GetPageCount(doc);
    FPDF_CloseDocument(doc);
    return n;
}

RenderResult Engine::render(std::string_view pdf_path, Options opts) const {
    auto [data, size] = ReadFile(pdf_path);
    if (!data) return std::unexpected(Error::FileNotFound);
    auto* doc = FPDF_LoadMemDocument64(data, size, nullptr);
    if (!doc) { std::free(data); return std::unexpected(Error::InvalidPdf); }

    int total = FPDF_GetPageCount(doc);

#ifndef _WIN32
    // Multi-process rendering: only when explicitly requested and not called from
    // a host process (Python/Node) where fork() is unsafe due to ObjC/threading.
    int workers = opts.workers;  // 0 = single-threaded (safe default for library use)

    if (workers > 1 && total > 1) {
        // Multi-process: fork workers, each renders a page range
        FPDF_CloseDocument(doc);

        // Allocate shared state
        auto* shared = static_cast<SharedRenderState*>(
            mmap(nullptr, sizeof(SharedRenderState), PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        shared->next_page.store(0);
        shared->total_pages = total;

        // Allocate shared result metadata
        auto meta_size = sizeof(SharedPageResult) * total;
        auto* meta = static_cast<SharedPageResult*>(
            mmap(nullptr, meta_size, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        std::memset(meta, 0, meta_size);

        // Estimate max buffer size per page
        // At 150 DPI, letter = ~1275x1650 = ~8.4MB per page
        const size_t max_page_size = static_cast<size_t>(
            (opts.dpi / 72.0f * 900) * (opts.dpi / 72.0f * 1200) * 4 + 65536);
        const size_t data_buf_size = max_page_size * total;

        auto* data_buf = static_cast<uint8_t*>(
            mmap(nullptr, data_buf_size, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        if (data_buf == MAP_FAILED) {
            munmap(shared, sizeof(SharedRenderState));
            munmap(meta, meta_size);
            std::free(data);
            return std::unexpected(Error::AllocFailed);
        }

        std::vector<pid_t> children;
        workers = std::min(workers, total);
        for (int i = 0; i < workers; ++i) {
            auto pid = fork();
            if (pid == 0) {
                // Child: re-open doc from memory (inherited via COW)
                auto* child_doc = FPDF_LoadMemDocument64(data, size, nullptr);
                if (!child_doc) std::exit(1);

                while (true) {
                    int pg = shared->next_page.fetch_add(1, std::memory_order_relaxed);
                    if (pg >= total) break;

                    auto* page = FPDF_LoadPage(child_doc, pg);
                    if (!page) continue;

                    const auto scale = opts.dpi / kPointsPerInch;
                    const auto w = static_cast<int>(FPDF_GetPageWidth(page) * scale + 0.5f);
                    const auto h = static_cast<int>(FPDF_GetPageHeight(page) * scale + 0.5f);
                    if (w <= 0 || h <= 0) { FPDF_ClosePage(page); continue; }

                    const auto stride = (w * 4 + 63) & ~63;
                    const auto buf_size_pg = static_cast<size_t>(stride) * h;
                    const auto offset = static_cast<size_t>(pg) * max_page_size;

                    if (offset + buf_size_pg > data_buf_size) {
                        FPDF_ClosePage(page);
                        continue;
                    }

                    auto* buffer = data_buf + offset;
                    auto* bitmap = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRx, buffer, stride);
                    if (!bitmap) { FPDF_ClosePage(page); continue; }

                    int flags = FPDF_PRINTING | FPDF_REVERSE_BYTE_ORDER;
                    if (opts.no_aa) flags |= kNoAA;
                    FPDFBitmap_FillRect(bitmap, 0, 0, w, h, 0xFFFFFFFF);
                    FPDF_RenderPageBitmap(bitmap, page, 0, 0, w, h, 0, flags);
                    FPDFBitmap_Destroy(bitmap);
                    FPDF_ClosePage(page);

                    meta[pg] = {w, h, stride, offset, 1};
                }

                FPDF_CloseDocument(child_doc);
                FPDF_DestroyLibrary();
                std::exit(0);
            } else if (pid > 0) {
                children.push_back(pid);
            }
        }

        for (auto pid : children)
            waitpid(pid, nullptr, 0);

        // Collect results in page order
        std::vector<Page> pages;
        pages.reserve(total);
        for (int i = 0; i < total; ++i) {
            if (meta[i].valid) {
                auto buf_size_pg = static_cast<size_t>(meta[i].stride) * meta[i].height;
                auto* copy = static_cast<uint8_t*>(std::malloc(buf_size_pg));
                std::memcpy(copy, data_buf + meta[i].data_offset, buf_size_pg);
                pages.emplace_back(copy, meta[i].width, meta[i].height, meta[i].stride);
            }
        }

        munmap(data_buf, data_buf_size);
        munmap(meta, meta_size);
        munmap(shared, sizeof(SharedRenderState));
        std::free(data);
        return pages;
    }
#endif

    // Single-process fallback
    auto pages = RenderDoc(doc, 0, total, opts.dpi, opts.no_aa);
    FPDF_CloseDocument(doc);
    std::free(data);
    return pages;
}

RenderResult Engine::render(std::span<const uint8_t> pdf_data, Options opts) const {
    auto* doc = FPDF_LoadMemDocument64(pdf_data.data(), pdf_data.size(), nullptr);
    if (!doc) return std::unexpected(Error::InvalidPdf);
    int total = FPDF_GetPageCount(doc);
    auto pages = RenderDoc(doc, 0, total, opts.dpi, opts.no_aa);
    FPDF_CloseDocument(doc);
    return pages;
}

RenderResult Engine::render_pages(std::string_view pdf_path,
                                   int start, int end, Options opts) const {
    auto [data, size] = ReadFile(pdf_path);
    if (!data) return std::unexpected(Error::FileNotFound);
    auto* doc = FPDF_LoadMemDocument64(data, size, nullptr);
    if (!doc) { std::free(data); return std::unexpected(Error::InvalidPdf); }
    auto pages = RenderDoc(doc, start, end, opts.dpi, opts.no_aa);
    FPDF_CloseDocument(doc);
    std::free(data);
    return pages;
}

// --- C API ---

static Engine* g_engine = nullptr;

extern "C" {

void fpdf2png_init() {
    if (!g_engine) g_engine = new Engine();
}

void fpdf2png_shutdown() {
    delete g_engine;
    g_engine = nullptr;
}

int fpdf2png_page_count(const char* path) {
    if (!g_engine) fpdf2png_init();
    return g_engine->page_count(path);
}

int fpdf2png_render(const char* path, float dpi, int no_aa,
                     fpdf2png_page_c** out, int* count) {
    if (!g_engine) fpdf2png_init();
    Options opts{dpi, no_aa != 0};
    auto result = g_engine->render(path, opts);
    if (!result) { *out = nullptr; *count = 0; return static_cast<int>(result.error()); }

    auto& pages = *result;
    *count = static_cast<int>(pages.size());
    *out = static_cast<fpdf2png_page_c*>(std::malloc(sizeof(fpdf2png_page_c) * pages.size()));
    for (size_t i = 0; i < pages.size(); ++i) {
        (*out)[i] = {pages[i].data.release(), pages[i].width, pages[i].height, pages[i].stride};
    }
    return 0;
}

int fpdf2png_render_mem(const uint8_t* data, size_t size, float dpi, int no_aa,
                         fpdf2png_page_c** out, int* count) {
    if (!g_engine) fpdf2png_init();
    Options opts{dpi, no_aa != 0};
    auto result = g_engine->render({data, size}, opts);
    if (!result) { *out = nullptr; *count = 0; return static_cast<int>(result.error()); }

    auto& pages = *result;
    *count = static_cast<int>(pages.size());
    *out = static_cast<fpdf2png_page_c*>(std::malloc(sizeof(fpdf2png_page_c) * pages.size()));
    for (size_t i = 0; i < pages.size(); ++i) {
        (*out)[i] = {pages[i].data.release(), pages[i].width, pages[i].height, pages[i].stride};
    }
    return 0;
}

void fpdf2png_free(fpdf2png_page_c* pages, int count) {
    if (!pages) return;
    for (int i = 0; i < count; ++i)
        std::free(pages[i].data);
    std::free(pages);
}

} // extern "C"

} // namespace fpdf2png
