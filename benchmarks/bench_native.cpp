// Pure C++ benchmark — renders all PDFs through the native library
// Tests single-threaded sequential AND multi-process parallel

#include "fastpdf2png/fastpdf2png.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    const char* dir = argc > 1 ? argv[1] : "../Documents";
    float dpi = argc > 2 ? std::atof(argv[2]) : 150.0f;
    int no_aa = argc > 3 ? std::atoi(argv[3]) : 0;
    int workers = argc > 4 ? std::atoi(argv[4]) : 8;

    std::vector<std::string> pdfs;
    for (auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".pdf")
            pdfs.push_back(e.path().string());
    std::sort(pdfs.begin(), pdfs.end());
    std::printf("PDFs: %zu, DPI: %.0f, AA: %s, workers: %d\n",
                pdfs.size(), dpi, no_aa ? "off" : "on", workers);

#ifndef _WIN32
    // Multi-process: fork workers, each grabs PDFs from shared atomic counter
    struct alignas(64) Shared {
        std::atomic<int> next{0};
        std::atomic<int> total_pages{0};
        int num_pdfs;
    };

    auto* shared = static_cast<Shared*>(
        mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    shared->next.store(0);
    shared->total_pages.store(0);
    shared->num_pdfs = static_cast<int>(pdfs.size());

    auto t0 = std::chrono::high_resolution_clock::now();

    std::vector<pid_t> children;
    for (int i = 0; i < workers; ++i) {
        auto pid = fork();
        if (pid == 0) {
            // Child: init PDFium, grab PDFs, render to memory
            fpdf2png::Engine engine;
            fpdf2png::Options opts{dpi, no_aa != 0, 0};

            while (true) {
                int idx = shared->next.fetch_add(1, std::memory_order_relaxed);
                if (idx >= shared->num_pdfs) break;

                auto result = engine.render(pdfs[idx], opts);
                if (result)
                    shared->total_pages.fetch_add(
                        static_cast<int>(result->size()), std::memory_order_relaxed);
            }
            std::exit(0);
        } else if (pid > 0) {
            children.push_back(pid);
        }
    }

    for (auto pid : children)
        waitpid(pid, nullptr, 0);

    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    int total = shared->total_pages.load();

    std::printf("Parallel: %d pages in %.3f seconds (%.1f pages/sec)\n",
                total, elapsed, total / elapsed);

    munmap(shared, sizeof(Shared));
#else
    // Single-threaded fallback on Windows
    fpdf2png::Engine engine;
    fpdf2png::Options opts{dpi, no_aa != 0, 0};

    auto t0 = std::chrono::high_resolution_clock::now();
    int total = 0;
    for (auto& pdf : pdfs) {
        auto result = engine.render(pdf, opts);
        if (result) total += static_cast<int>(result->size());
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    std::printf("Sequential: %d pages in %.3f seconds (%.1f pages/sec)\n",
                total, elapsed, total / elapsed);
#endif

    return 0;
}
