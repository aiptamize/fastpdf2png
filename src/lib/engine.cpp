// fastpdf2png — Engine implementation
// SPDX-License-Identifier: MIT

#include "fastpdf2png/engine.h"
#include "fastpdf2png/types.h"
#include "internal/pdfium_render.h"
#include "internal/file_io.h"
#include "internal/shared_memory.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "fpdfview.h"
#include "fpdf_edit.h"

namespace fpdf2png {

// ── Engine::Impl ────────────────────────────────────────────────────

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

// ── page_count ──────────────────────────────────────────────────────

int Engine::page_count(std::string_view pdf_path) const {
    auto [data, size] = internal::ReadFile(pdf_path);
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

// ── render (file path) ──────────────────────────────────────────────

RenderResult Engine::render(std::string_view pdf_path, Options opts) const {
    auto [data, size] = internal::ReadFile(pdf_path);
    if (!data) return std::unexpected(Error::FileNotFound);
    auto* doc = FPDF_LoadMemDocument64(data, size, nullptr);
    if (!doc) { std::free(data); return std::unexpected(Error::InvalidPdf); }

    int total = FPDF_GetPageCount(doc);

#ifndef _WIN32
    int workers = opts.workers;
    if (workers <= 0) {
        workers = static_cast<int>(std::thread::hardware_concurrency());
        if (workers < 1) workers = 4;
    }

    if (workers > 1 && total > 1) {
        FPDF_CloseDocument(doc);

        auto* shared = static_cast<internal::SharedRenderState*>(
            internal::MmapShared(sizeof(internal::SharedRenderState)));
        if (!shared) { std::free(data); return std::unexpected(Error::AllocFailed); }
        new (&shared->next_page) std::atomic<int>(0);
        shared->total_pages = total;

        auto meta_size = sizeof(internal::SharedPageResult) * total;
        auto* meta = static_cast<internal::SharedPageResult*>(internal::MmapShared(meta_size));
        if (!meta) {
            munmap(shared, sizeof(internal::SharedRenderState));
            std::free(data);
            return std::unexpected(Error::AllocFailed);
        }
        std::memset(meta, 0, meta_size);

        // Compute max page buffer size with overflow protection
        const double pw = static_cast<double>(opts.dpi) / 72.0 * 900.0;
        const double ph = static_cast<double>(opts.dpi) / 72.0 * 1200.0;
        const double max_pg_d = pw * ph * 4.0 + 65536.0;
        if (max_pg_d > 1e12 || max_pg_d * total > 1e12) {
            munmap(meta, meta_size);
            munmap(shared, sizeof(internal::SharedRenderState));
            std::free(data);
            return std::unexpected(Error::AllocFailed);
        }
        const size_t max_page_size = static_cast<size_t>(max_pg_d);
        const size_t data_buf_size = max_page_size * total;

        auto* data_buf = static_cast<uint8_t*>(internal::MmapShared(data_buf_size));
        if (!data_buf) {
            munmap(meta, meta_size);
            munmap(shared, sizeof(internal::SharedRenderState));
            std::free(data);
            return std::unexpected(Error::AllocFailed);
        }

        std::vector<pid_t> children;
        workers = std::min(workers, total);
        for (int i = 0; i < workers; ++i) {
            auto pid = fork();
            if (pid == 0) {
                auto* child_doc = FPDF_LoadMemDocument64(data, size, nullptr);
                if (!child_doc) _exit(1);

                while (true) {
                    int pg = shared->next_page.fetch_add(1, std::memory_order_relaxed);
                    if (pg >= total) break;

                    auto* page = FPDF_LoadPage(child_doc, pg);
                    if (!page) continue;

                    const auto scale = opts.dpi / internal::kPointsPerInch;
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
                    if (opts.no_aa) flags |= internal::kNoAA;
                    FPDFBitmap_FillRect(bitmap, 0, 0, w, h, 0xFFFFFFFF);
                    FPDF_RenderPageBitmap(bitmap, page, 0, 0, w, h, 0, flags);
                    FPDFBitmap_Destroy(bitmap);
                    FPDF_ClosePage(page);

                    meta[pg] = {w, h, stride, offset, 1};
                }

                FPDF_CloseDocument(child_doc);
                _exit(0);
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
                if (!copy) continue;  // skip page on OOM
                std::memcpy(copy, data_buf + meta[i].data_offset, buf_size_pg);
                pages.emplace_back(copy, meta[i].width, meta[i].height, meta[i].stride);
            }
        }

        munmap(data_buf, data_buf_size);
        munmap(meta, meta_size);
        munmap(shared, sizeof(internal::SharedRenderState));
        std::free(data);
        return pages;
    }
#endif

    // Single-process fallback
    auto pages = internal::RenderDoc(doc, 0, total, opts.dpi, opts.no_aa);
    FPDF_CloseDocument(doc);
    std::free(data);
    return pages;
}

// ── render (in-memory) ──────────────────────────────────────────────

RenderResult Engine::render(std::span<const uint8_t> pdf_data, Options opts) const {
    auto* doc = FPDF_LoadMemDocument64(pdf_data.data(), pdf_data.size(), nullptr);
    if (!doc) return std::unexpected(Error::InvalidPdf);
    int total = FPDF_GetPageCount(doc);
    auto pages = internal::RenderDoc(doc, 0, total, opts.dpi, opts.no_aa);
    FPDF_CloseDocument(doc);
    return pages;
}

// ── render_pages ────────────────────────────────────────────────────

RenderResult Engine::render_pages(std::string_view pdf_path,
                                   int start, int end, Options opts) const {
    auto [data, size] = internal::ReadFile(pdf_path);
    if (!data) return std::unexpected(Error::FileNotFound);
    auto* doc = FPDF_LoadMemDocument64(data, size, nullptr);
    if (!doc) { std::free(data); return std::unexpected(Error::InvalidPdf); }
    auto pages = internal::RenderDoc(doc, start, end, opts.dpi, opts.no_aa);
    FPDF_CloseDocument(doc);
    std::free(data);
    return pages;
}

// ── process_many ────────────────────────────────────────────────────

#ifndef _WIN32
int Engine::process_many(const std::vector<std::string>& pdf_paths,
                          Options opts, int num_workers,
                          PageCallback callback) const {
    const int total = static_cast<int>(pdf_paths.size());
    if (total == 0) return 0;
    num_workers = std::clamp(num_workers, 1, total);

    if (num_workers <= 1) {
        int done = 0;
        for (auto& path : pdf_paths) {
            auto [data, size] = internal::ReadFile(path);
            if (!data) continue;
            auto* doc = FPDF_LoadMemDocument64(data, size, nullptr);
            if (!doc) { std::free(data); continue; }
            auto pages = internal::RenderDoc(doc, 0, FPDF_GetPageCount(doc), opts.dpi, opts.no_aa);
            FPDF_CloseDocument(doc);
            std::free(data);
            callback(path, pages);
            ++done;
        }
        return done;
    }

    // All counters in mmap shared memory — visible across fork
    struct alignas(64) SharedState {
        std::atomic<int> next{0};
        std::atomic<int> done_pdfs{0};
        std::atomic<int> done_pages{0};
    };

    auto* shared = static_cast<SharedState*>(internal::MmapShared(sizeof(SharedState)));
    if (!shared) return 0;
    new (&shared->next) std::atomic<int>(0);
    new (&shared->done_pdfs) std::atomic<int>(0);
    new (&shared->done_pages) std::atomic<int>(0);

    std::vector<pid_t> children;
    children.reserve(num_workers);

    for (int i = 0; i < num_workers; ++i) {
        auto pid = fork();
        if (pid == 0) {
            while (true) {
                int idx = shared->next.fetch_add(1, std::memory_order_relaxed);
                if (idx >= total) break;

                auto [fdata, fsize] = internal::ReadFile(pdf_paths[idx]);
                if (!fdata) continue;
                auto* doc = FPDF_LoadMemDocument64(fdata, fsize, nullptr);
                if (!doc) { std::free(fdata); continue; }

                auto pages = internal::RenderDoc(doc, 0, FPDF_GetPageCount(doc),
                                                  opts.dpi, opts.no_aa);
                FPDF_CloseDocument(doc);
                std::free(fdata);

                callback(pdf_paths[idx], pages);

                shared->done_pdfs.fetch_add(1, std::memory_order_relaxed);
                shared->done_pages.fetch_add(
                    static_cast<int>(pages.size()), std::memory_order_relaxed);
            }
            _exit(0);
        } else if (pid > 0) {
            children.push_back(pid);
        }
    }

    for (auto pid : children)
        waitpid(pid, nullptr, 0);

    int result = shared->done_pdfs.load(std::memory_order_acquire);
    munmap(shared, sizeof(SharedState));
    return result;
}
#else
int Engine::process_many(const std::vector<std::string>& pdf_paths,
                          Options opts, int num_workers,
                          PageCallback callback) const {
    int done = 0;
    for (auto& path : pdf_paths) {
        auto result = render(path, opts);
        if (result) {
            callback(path, *result);
            ++done;
        }
    }
    return done;
}
#endif

} // namespace fpdf2png
