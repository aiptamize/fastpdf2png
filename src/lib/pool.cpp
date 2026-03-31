// fastpdf2png — Pool implementation (Unix fork-based parallelism)
// SPDX-License-Identifier: MIT
//
// Fixes applied:
//  1. Slot free: write stack entry BEFORE incrementing top (no stale read)
//  2. Job claim: CAS loop on job_head (no permanent over-advance on miss)
//  3. next() re-checks submit_count after poll timeout (no premature nullopt)
//  4. Slot cleanup on OOM/error (no leaked slots)
//  5. FD leak: close previous workers' result_pipe write ends in child
//  6. POLLHUP/POLLERR handling (no infinite busy-loop on worker crash)
//  7. Job ring back-pressure (no overwrite while worker reads)

#ifndef _WIN32

#include "fastpdf2png/pool.h"
#include "fastpdf2png/types.h"
#include "internal/pdfium_render.h"
#include "internal/file_io.h"
#include "internal/shared_memory.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fpdfview.h"
#include "fpdf_edit.h"

namespace fpdf2png {

namespace {

constexpr int kMaxPoolJobs = 4096;
constexpr int kMaxPagesPerResult = 512;
constexpr size_t kPageSlotSize = 48ULL * 1024 * 1024;
constexpr int kNumPageSlots = 64;

struct alignas(64) PoolShared {
    std::atomic<int> job_tail{0};
    char pad1[60];
    std::atomic<int> job_head{0};
    char pad2[60];
    // Slot free-stack: slot_stack[0..slot_top-1] are available slot indices.
    // Alloc: CAS decrement slot_top, read slot_stack[new_top].
    // Free: write slot_stack[old_top], then CAS increment slot_top.
    // The write-before-increment ensures no stale read.
    std::atomic<int> slot_top{0};
    char pad3[60];
    std::atomic<int> slot_lock{0};  // spinlock protecting slot_stack + slot_top
    char pad4[60];
    int slot_stack[kNumPageSlots];  // guarded by slot_lock spinlock
};

struct PoolJobSlot {
    char pdf_path[512];
    float dpi;
    bool no_aa;
    std::atomic<bool> consumed{false};  // worker sets true after copying fields
};

struct PoolResultHdr {
    char pdf_path[512];
    int32_t num_pages;
    struct PageInfo {
        int32_t width, height, stride;
        int32_t slot_idx;  // >= 0: shared memory slot, -1: pixels follow in pipe
    } pages[kMaxPagesPerResult];
};

bool FullWrite(int fd, const void* buf, size_t count) {
    auto* p = static_cast<const uint8_t*>(buf);
    while (count > 0) {
        auto n = write(fd, p, count);
        if (n <= 0) return false;
        p += n; count -= n;
    }
    return true;
}

bool FullRead(int fd, void* buf, size_t count) {
    auto* p = static_cast<uint8_t*>(buf);
    while (count > 0) {
        auto n = read(fd, p, count);
        if (n <= 0) return false;
        p += n; count -= n;
    }
    return true;
}

} // namespace

// ── Pool::Impl ──────────────────────────────────────────────────────

struct Pool::Impl {
    PoolShared* shared = nullptr;
    PoolJobSlot* job_slots = nullptr;
    uint8_t* page_slots = nullptr;
    int wake_pipe[2] = {-1, -1};

    struct WorkerInfo { pid_t pid; int result_rd; };
    std::vector<WorkerInfo> workers;
    Options opts;
    std::atomic<int> submit_count{0};
    std::atomic<int> complete_count{0};
    std::atomic<bool> finished{false};
    std::mutex submit_mtx;
    std::mutex next_mtx;
    int num_alive_workers = 0;
};

// ── Constructor ─────────────────────────────────────────────────────

Pool::Pool(int num_workers, Options opts)
    : impl_(std::make_unique<Impl>()) {
    impl_->opts = opts;

    impl_->shared = static_cast<PoolShared*>(internal::MmapShared(sizeof(PoolShared)));
    if (!impl_->shared)
        throw std::runtime_error("Pool: failed to allocate shared memory");
    new (&impl_->shared->job_tail) std::atomic<int>(0);
    new (&impl_->shared->job_head) std::atomic<int>(0);
    new (&impl_->shared->slot_top) std::atomic<int>(kNumPageSlots);
    new (&impl_->shared->slot_lock) std::atomic<int>(0);
    for (int i = 0; i < kNumPageSlots; ++i)
        impl_->shared->slot_stack[i] = i;

    impl_->job_slots = static_cast<PoolJobSlot*>(
        internal::MmapShared(sizeof(PoolJobSlot) * kMaxPoolJobs));
    if (!impl_->job_slots) {
        munmap(impl_->shared, sizeof(PoolShared));
        impl_->shared = nullptr;
        return;
    }

    auto page_buf_size = static_cast<size_t>(kNumPageSlots) * kPageSlotSize;
    impl_->page_slots = static_cast<uint8_t*>(internal::MmapShared(page_buf_size));
    if (!impl_->page_slots) {
        munmap(impl_->job_slots, sizeof(PoolJobSlot) * kMaxPoolJobs);
        munmap(impl_->shared, sizeof(PoolShared));
        impl_->shared = nullptr;
        return;
    }

    if (pipe(impl_->wake_pipe) != 0) {
        munmap(impl_->page_slots, page_buf_size);
        munmap(impl_->job_slots, sizeof(PoolJobSlot) * kMaxPoolJobs);
        munmap(impl_->shared, sizeof(PoolShared));
        impl_->shared = nullptr;
        return;
    }

    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);

    num_workers = std::max(1, num_workers);
    auto* shared = impl_->shared;
    auto* job_slots = impl_->job_slots;
    auto* page_slots = impl_->page_slots;
    int wake_rd = impl_->wake_pipe[0];

    // Track result_pipe write-end fds so children can close them (fix #5)
    std::vector<int> result_write_fds;

    for (int i = 0; i < num_workers; ++i) {
        int result_pipe[2];
        if (pipe(result_pipe) != 0) continue;

        auto pid = fork();
        if (pid == 0) {
            // Child: close parent-side fds
            close(impl_->wake_pipe[1]);
            close(result_pipe[0]);
            // Close ALL previous workers' result read fds
            for (auto& w : impl_->workers)
                close(w.result_rd);
            // Close ALL previous workers' result WRITE fds (fix #5: prevent FD leak)
            for (int wfd : result_write_fds)
                close(wfd);

            int rfd = result_pipe[1];

            // Spinlock helpers for slot stack (fixes data race on slot_stack[])
            auto lock_slots = [&]() {
                while (shared->slot_lock.exchange(1, std::memory_order_acquire) != 0)
                    ; // spin
            };
            auto unlock_slots = [&]() {
                shared->slot_lock.store(0, std::memory_order_release);
            };

            auto try_alloc_slot = [&]() -> int {
                lock_slots();
                int top = shared->slot_top.load(std::memory_order_relaxed);
                int result = -1;
                if (top > 0) {
                    result = shared->slot_stack[top - 1];
                    shared->slot_top.store(top - 1, std::memory_order_relaxed);
                }
                unlock_slots();
                return result;
            };

            // Note: slot freeing is done by the parent in next(), not by the worker.
            // Workers allocate slots, write page data, and the parent frees after reading.

            while (true) {
                char c;
                if (read(wake_rd, &c, 1) <= 0) break;

                // Fix #2: CAS loop on job_head — don't advance on miss
                int idx, tail;
                while (true) {
                    idx = shared->job_head.load(std::memory_order_acquire);
                    tail = shared->job_tail.load(std::memory_order_acquire);
                    if (idx >= tail) { idx = -1; break; }  // no job available
                    if (shared->job_head.compare_exchange_weak(
                            idx, idx + 1, std::memory_order_acq_rel))
                        break;  // claimed idx
                }
                if (idx < 0) continue;  // spurious wake, no job lost

                // Copy job fields immediately (fix #7: slot can be reused after consumed flag)
                auto& jslot = job_slots[idx % kMaxPoolJobs];
                char path[512];
                std::memcpy(path, jslot.pdf_path, sizeof(path));
                float dpi = jslot.dpi;
                bool no_aa = jslot.no_aa;
                jslot.consumed.store(true, std::memory_order_release);

                auto [fdata, fsize] = internal::ReadFile(path);
                std::vector<Page> pages;
                if (fdata) {
                    auto* doc = FPDF_LoadMemDocument64(fdata, fsize, nullptr);
                    if (doc) {
                        pages = internal::RenderDoc(doc, 0, FPDF_GetPageCount(doc), dpi, no_aa);
                        FPDF_CloseDocument(doc);
                    }
                    std::free(fdata);
                }

                PoolResultHdr hdr{};
                std::strncpy(hdr.pdf_path, path, sizeof(hdr.pdf_path) - 1);
                hdr.num_pages = static_cast<int32_t>(
                    std::min(pages.size(), static_cast<size_t>(kMaxPagesPerResult)));

                for (int p = 0; p < hdr.num_pages; ++p) {
                    auto& pg = pages[p];
                    auto pg_size = static_cast<size_t>(pg.stride) * pg.height;
                    int slot = (pg_size <= kPageSlotSize) ? try_alloc_slot() : -1;
                    if (slot >= 0) {
                        std::memcpy(
                            page_slots + static_cast<size_t>(slot) * kPageSlotSize,
                            pg.data.get(), pg_size);
                    }
                    hdr.pages[p] = {pg.width, pg.height, pg.stride, slot};
                }

                if (!FullWrite(rfd, &hdr, sizeof(hdr))) break;

                for (int p = 0; p < hdr.num_pages; ++p) {
                    if (hdr.pages[p].slot_idx < 0) {
                        auto& pg = pages[p];
                        auto pg_size = static_cast<size_t>(pg.stride) * pg.height;
                        if (!FullWrite(rfd, pg.data.get(), pg_size)) goto worker_exit;
                    }
                }
                continue;
            worker_exit:
                break;
            }

            close(wake_rd);
            close(rfd);
            _exit(0);
        }

        // Parent: track write-end FD for future children BEFORE closing
        result_write_fds.push_back(result_pipe[1]);
        close(result_pipe[1]);
        impl_->workers.push_back({pid, result_pipe[0]});
    }
    impl_->num_alive_workers = static_cast<int>(impl_->workers.size());
}

// ── Destructor ──────────────────────────────────────────────────────

Pool::~Pool() {
    if (!impl_->shared) return;
    // Ignore SIGPIPE so worker pipe writes fail cleanly
    signal(SIGPIPE, SIG_IGN);
    close(impl_->wake_pipe[1]);
    close(impl_->wake_pipe[0]);
    for (auto& w : impl_->workers)
        close(w.result_rd);
    for (auto& w : impl_->workers)
        waitpid(w.pid, nullptr, 0);
    auto page_buf_size = static_cast<size_t>(kNumPageSlots) * kPageSlotSize;
    munmap(impl_->page_slots, page_buf_size);
    munmap(impl_->job_slots, sizeof(PoolJobSlot) * kMaxPoolJobs);
    munmap(impl_->shared, sizeof(PoolShared));
    FPDF_DestroyLibrary();
}

// ── submit ──────────────────────────────────────────────────────────

void Pool::submit(std::string pdf_path) {
    std::lock_guard<std::mutex> lock(impl_->submit_mtx);
    if (!impl_->shared || impl_->finished.load(std::memory_order_acquire)) return;

    int idx = impl_->submit_count.load(std::memory_order_acquire);

    // Fix #7: back-pressure — wait for oldest job to be consumed before overwriting
    int oldest_in_ring = idx - kMaxPoolJobs;
    if (oldest_in_ring >= 0) {
        auto& old_slot = impl_->job_slots[oldest_in_ring % kMaxPoolJobs];
        int retries = 0;
        while (!old_slot.consumed.load(std::memory_order_acquire)) {
            usleep(100);
            if (++retries > 100000) // 10 seconds
                throw std::runtime_error("Pool::submit: worker not consuming jobs (possibly crashed)");
        }
    }

    auto& slot = impl_->job_slots[idx % kMaxPoolJobs];
    slot.consumed.store(false, std::memory_order_release);
    std::strncpy(slot.pdf_path, pdf_path.c_str(), sizeof(slot.pdf_path) - 1);
    slot.pdf_path[sizeof(slot.pdf_path) - 1] = '\0';
    slot.dpi = impl_->opts.dpi;
    slot.no_aa = impl_->opts.no_aa;

    impl_->shared->job_tail.store(idx + 1, std::memory_order_release);
    impl_->submit_count.fetch_add(1, std::memory_order_release);

    char c = 1;
    [[maybe_unused]] auto _ = write(impl_->wake_pipe[1], &c, 1);
}

// ── next ────────────────────────────────────────────────────────────

std::optional<PoolResult> Pool::next() {
    std::lock_guard<std::mutex> lock(impl_->next_mtx);
    if (!impl_->shared) return std::nullopt;

    auto nw = static_cast<int>(impl_->workers.size());
    std::vector<struct pollfd> pfds(nw);

    while (true) {
        // Fix #3: re-check counts each iteration (submit may have added more)
        int submitted = impl_->submit_count.load(std::memory_order_acquire);
        int completed = impl_->complete_count.load(std::memory_order_acquire);
        if (completed >= submitted) {
            if (impl_->finished.load(std::memory_order_acquire)) return std::nullopt;
            // Not finished — block until either a result arrives or finish is called
            usleep(1000);
            // Re-check — do NOT return nullopt unless finished
            continue;
        }

        for (int i = 0; i < nw; ++i)
            pfds[i] = {impl_->workers[i].result_rd, POLLIN, 0};

        int ready = poll(pfds.data(), static_cast<nfds_t>(nw), 500);
        if (ready <= 0) continue;

        for (int i = 0; i < nw; ++i) {
            // Fix #6: handle POLLHUP/POLLERR (worker crash)
            if (pfds[i].revents & (POLLHUP | POLLERR)) {
                // Worker died — mark it dead, skip
                impl_->workers[i].result_rd = -1;
                impl_->num_alive_workers--;
                if (impl_->num_alive_workers <= 0) return std::nullopt;
                continue;
            }
            if (!(pfds[i].revents & POLLIN)) continue;

            int fd = impl_->workers[i].result_rd;

            PoolResultHdr hdr{};
            if (!FullRead(fd, &hdr, sizeof(hdr))) return std::nullopt;

            PoolResult result;
            result.pdf_path = hdr.pdf_path;
            result.pages.reserve(hdr.num_pages);

            for (int j = 0; j < hdr.num_pages; ++j) {
                auto& pi = hdr.pages[j];
                auto pg_size = static_cast<size_t>(pi.stride) * pi.height;
                auto* copy = static_cast<uint8_t*>(std::malloc(pg_size));

                if (pi.slot_idx >= 0) {
                    if (copy) {
                        std::memcpy(copy,
                                    impl_->page_slots + static_cast<size_t>(pi.slot_idx) * kPageSlotSize,
                                    pg_size);
                        result.pages.emplace_back(copy, pi.width, pi.height, pi.stride);
                    }
                    // Fix #4: ALWAYS free slot, even on OOM (spinlock-protected)
                    auto* sh = impl_->shared;
                    while (sh->slot_lock.exchange(1, std::memory_order_acquire) != 0)
                        ; // spin
                    int top = sh->slot_top.load(std::memory_order_relaxed);
                    if (top < kNumPageSlots) {
                        sh->slot_stack[top] = pi.slot_idx;
                        sh->slot_top.store(top + 1, std::memory_order_relaxed);
                    }
                    sh->slot_lock.store(0, std::memory_order_release);
                } else {
                    // Pipe fallback — must read pixels regardless of OOM
                    if (copy) {
                        if (!FullRead(fd, copy, pg_size)) { std::free(copy); break; }
                        result.pages.emplace_back(copy, pi.width, pi.height, pi.stride);
                    } else {
                        // OOM but must drain pipe to keep protocol in sync
                        size_t skip = pg_size;
                        uint8_t drain[4096];
                        while (skip > 0) {
                            size_t chunk = std::min(skip, sizeof(drain));
                            if (!FullRead(fd, drain, chunk)) break;
                            skip -= chunk;
                        }
                    }
                }
            }

            impl_->complete_count.fetch_add(1, std::memory_order_release);
            return result;
        }
    }
}

// ── finish / submitted / completed ──────────────────────────────────

void Pool::finish() {
    impl_->finished.store(true, std::memory_order_release);
}

int Pool::submitted() const {
    return impl_->submit_count.load(std::memory_order_acquire);
}

int Pool::completed() const {
    return impl_->complete_count.load(std::memory_order_acquire);
}

} // namespace fpdf2png

#endif // !_WIN32
