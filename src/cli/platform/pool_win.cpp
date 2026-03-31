// fastpdf2png — Windows pool mode (CreateProcess workers with pipe IPC)
// SPDX-License-Identifier: MIT

#ifdef _WIN32

#include "cli/pool_cmd.h"
#include "cli/ipc.h"
#include "cli/shared_cmd.h"
#include "internal/pdfium_render.h"
#include "internal/file_io.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#define NOMINMAX
#include <windows.h>

#include "fpdfview.h"
#include "fpdf_edit.h"

namespace fpdf2png::cli {

namespace {

constexpr DWORD kWinLargeFileBytes = 500 * 1024;

} // anonymous namespace

int RunPoolWorkerWin(HANDLE cmd_h, HANDLE result_h) {
    InitPdfium();
    PipeJob job;
    while (true) {
        if (!internal::ReadFullWin(cmd_h, &job, sizeof(job))) break;

        size_t file_size = 0;
        auto* file_data = internal::ReadFileToMemoryWin(job.pdf_path, file_size);
        FPDF_DOCUMENT doc = nullptr;
        if (file_data)
            doc = FPDF_LoadMemDocument64(file_data, file_size, nullptr);

        PipeResult result{0};
        if (doc) {
            const int p_start = (job.page_start >= 0) ? job.page_start : 0;
            const int p_end = (job.page_start >= 0)
                ? job.page_end : FPDF_GetPageCount(doc);
            for (int p = p_start; p < p_end; ++p) {
                if (internal::RenderPageToFile(doc, p, job.dpi,
                        job.output_pattern, job.compression, job.no_aa))
                    ++result.pages_rendered;
            }
            FPDF_CloseDocument(doc);
        }
        std::free(file_data);
        internal::WriteFullWin(result_h, &result, sizeof(result));
    }
    FPDF_DestroyLibrary();
    return 0;
}

int RunPoolWin(int num_workers, float default_dpi, int default_compression, bool no_aa) {
    InitPdfium();

    char exe_path[MAX_PATH];
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);

    struct WinWorker {
        HANDLE process;
        HANDLE cmd_write;    // parent writes jobs here
        HANDLE result_read;  // parent reads results here
        int in_flight;
    };
    std::vector<WinWorker> workers(num_workers);

    for (int i = 0; i < num_workers; ++i) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE cmd_read, cmd_write, result_read, result_write;
        if (!CreatePipe(&cmd_read, &cmd_write, &sa, 0) ||
            !CreatePipe(&result_read, &result_write, &sa, 0)) {
            std::fprintf(stderr, "CreatePipe failed (%lu)\n", GetLastError());
            return 1;
        }
        SetHandleInformation(cmd_write, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(result_read, HANDLE_FLAG_INHERIT, 0);

        char cmdline[8192];
        std::snprintf(cmdline, sizeof(cmdline),
                      "\"%s\" --pool-worker %llu %llu",
                      exe_path,
                      static_cast<unsigned long long>(
                          reinterpret_cast<uintptr_t>(cmd_read)),
                      static_cast<unsigned long long>(
                          reinterpret_cast<uintptr_t>(result_write)));

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        if (!CreateProcessA(nullptr, cmdline, nullptr, nullptr, TRUE,
                            0, nullptr, nullptr, &si, &pi)) {
            std::fprintf(stderr, "CreateProcess failed for worker %d (%lu)\n",
                         i, GetLastError());
            CloseHandle(cmd_read); CloseHandle(cmd_write);
            CloseHandle(result_read); CloseHandle(result_write);
            return 1;
        }

        CloseHandle(pi.hThread);
        CloseHandle(cmd_read);
        CloseHandle(result_write);

        workers[i] = {pi.hProcess, cmd_write, result_read, 0};
    }

    int total_pages = 0;
    int total_jobs = 0;

    auto send_job = [&](int w, const char* pdf, const char* pat,
                         float dpi, int comp, int p_start, int p_end) {
        PipeJob job{};
        std::strncpy(job.pdf_path, pdf, sizeof(job.pdf_path) - 1);
        std::strncpy(job.output_pattern, pat, sizeof(job.output_pattern) - 1);
        job.dpi = dpi;
        job.compression = comp;
        job.page_start = p_start;
        job.page_end = p_end;
        job.no_aa = no_aa ? 1 : 0;
        internal::WriteFullWin(workers[w].cmd_write, &job, sizeof(job));
        workers[w].in_flight++;
        total_jobs++;
    };

    const auto t0 = std::chrono::high_resolution_clock::now();

    char line[8192];
    while (std::fgets(line, sizeof(line), stdin)) {
        auto len = std::strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (std::strncmp(line, "QUIT", 4) == 0) break;
        if (len == 0 || (len == 1 && line[0] == '\n')) continue;

        char* tokens[8];
        const auto ntok = SplitTabs(line, tokens, 8);

        const char* pdf_path;
        const char* out_pattern;
        float dpi = default_dpi;
        int comp = default_compression;

        if (ntok >= 3 && std::string_view{tokens[0]} == "RENDER") {
            pdf_path = tokens[1];
            out_pattern = tokens[2];
            if (ntok >= 4) dpi = static_cast<float>(std::atof(tokens[3]));
            if (ntok >= 6) comp = std::clamp(std::atoi(tokens[5]), -1, 2);
        } else if (ntok >= 2) {
            pdf_path = tokens[0];
            out_pattern = tokens[1];
        } else {
            continue;
        }

        auto pick_w = [&]() -> int {
            int best = 0;
            for (int i = 1; i < num_workers; ++i)
                if (workers[i].in_flight < workers[best].in_flight) best = i;
            return best;
        };

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        GetFileAttributesExA(pdf_path, GetFileExInfoStandard, &fad);
        DWORD fsize = fad.nFileSizeLow;

        if (fsize > kWinLargeFileBytes) {
            auto* doc = FPDF_LoadDocument(pdf_path, nullptr);
            if (doc) {
                const int pages = FPDF_GetPageCount(doc);
                FPDF_CloseDocument(doc);
                const int ranges = std::min(pages, num_workers);
                const int per_range = (pages + ranges - 1) / ranges;
                for (int p = 0; p < pages; p += per_range) {
                    send_job(pick_w(), pdf_path, out_pattern,
                             dpi, comp, p, std::min(p + per_range, pages));
                }
            } else {
                send_job(pick_w(), pdf_path, out_pattern, dpi, comp, -1, 0);
            }
        } else {
            send_job(pick_w(), pdf_path, out_pattern, dpi, comp, -1, 0);
        }
    }

    // Close cmd pipes (signals shutdown), wait for results
    for (int i = 0; i < num_workers; ++i)
        CloseHandle(workers[i].cmd_write);

    for (int i = 0; i < num_workers; ++i) {
        PipeResult result;
        while (workers[i].in_flight > 0) {
            if (internal::ReadFullWin(workers[i].result_read, &result, sizeof(result))) {
                total_pages += result.pages_rendered;
                workers[i].in_flight--;
            } else {
                break;
            }
        }
        CloseHandle(workers[i].result_read);
        WaitForSingleObject(workers[i].process, INFINITE);
        CloseHandle(workers[i].process);
    }

    const auto t1 = std::chrono::high_resolution_clock::now();
    const auto elapsed = std::chrono::duration<double>(t1 - t0).count();

    if (elapsed > 0)
        std::printf("Pool: %d jobs, %d pages in %.3f seconds (%.1f pages/sec)\n",
                    total_jobs, total_pages, elapsed, total_pages / elapsed);

    FPDF_DestroyLibrary();
    return 0;
}

} // namespace fpdf2png::cli

#endif // _WIN32
