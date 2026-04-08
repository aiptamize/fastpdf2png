// fastpdf2png — C API implementation
// SPDX-License-Identifier: MIT

#include "fastpdf2png/c_api.h"
#include "fastpdf2png/engine.h"
#include "fastpdf2png/types.h"

#include <cstdlib>
#include <mutex>

static fpdf2png::Engine* g_engine = nullptr;
static std::mutex g_engine_mtx;

extern "C" {

void fpdf2png_init(void) {
    std::lock_guard<std::mutex> lock(g_engine_mtx);
    if (!g_engine) g_engine = new fpdf2png::Engine();
}

void fpdf2png_shutdown(void) {
    std::lock_guard<std::mutex> lock(g_engine_mtx);
    delete g_engine;
    g_engine = nullptr;
}

int fpdf2png_page_count(const char* path) {
    if (!g_engine) fpdf2png_init();
    return g_engine->page_count(path);
}

int fpdf2png_render(const char* path, float dpi, int no_aa,
                     fpdf2png_page_c** out, int* out_count) {
    if (!out || !out_count) return 4; // AllocFailed
    *out = nullptr;
    *out_count = 0;

    if (!g_engine) fpdf2png_init();
    fpdf2png::Options opts{dpi, no_aa != 0};
    auto result = g_engine->render(path, opts);
    if (!result) return static_cast<int>(result.error());

    auto& pages = *result;
    const int n = static_cast<int>(pages.size());
    if (n == 0) return 0;

    *out = static_cast<fpdf2png_page_c*>(std::malloc(sizeof(fpdf2png_page_c) * n));
    if (!*out) return 4; // AllocFailed
    *out_count = n;

    for (int i = 0; i < n; ++i) {
        (*out)[i] = {pages[i].data.release(), pages[i].width, pages[i].height, pages[i].stride};
    }
    return 0;
}

int fpdf2png_render_mem(const uint8_t* data, size_t size, float dpi, int no_aa,
                         fpdf2png_page_c** out, int* out_count) {
    if (!out || !out_count) return 4;
    *out = nullptr;
    *out_count = 0;

    if (!g_engine) fpdf2png_init();
    fpdf2png::Options opts{dpi, no_aa != 0};
    auto result = g_engine->render({data, size}, opts);
    if (!result) return static_cast<int>(result.error());

    auto& pages = *result;
    const int n = static_cast<int>(pages.size());
    if (n == 0) return 0;

    *out = static_cast<fpdf2png_page_c*>(std::malloc(sizeof(fpdf2png_page_c) * n));
    if (!*out) return 4;
    *out_count = n;

    for (int i = 0; i < n; ++i) {
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
