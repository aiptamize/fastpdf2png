// Roundtrip integration test: PDFium render -> PNG encode -> PNG decode -> verify.
// Requires TEST_PDF environment variable pointing to a valid PDF file.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "fpdfview.h"
#include "png/png_writer.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

// ---------------------------------------------------------------------------
// Test fixture: initializes PDFium once, loads PDF from TEST_PDF env var
// ---------------------------------------------------------------------------

class RoundtripTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        FPDF_InitLibrary();

        const char* env = std::getenv("TEST_PDF");
        if (!env || env[0] == '\0') {
            pdf_path_.clear();
            return;
        }
        pdf_path_ = env;

        doc_ = FPDF_LoadDocument(pdf_path_.c_str(), nullptr);
        if (doc_) {
            page_count_ = FPDF_GetPageCount(doc_);
        }
    }

    static void TearDownTestSuite() {
        if (doc_) FPDF_CloseDocument(doc_);
        FPDF_DestroyLibrary();
    }

    void SetUp() override {
        if (pdf_path_.empty()) GTEST_SKIP() << "TEST_PDF not set";
        if (!doc_) GTEST_SKIP() << "Failed to open PDF: " << pdf_path_;
    }

    static std::string pdf_path_;
    static FPDF_DOCUMENT doc_;
    static int page_count_;

    static constexpr float kDpi = 150.0f;
    static constexpr float kPointsPerInch = 72.0f;
};

std::string RoundtripTest::pdf_path_;
FPDF_DOCUMENT RoundtripTest::doc_ = nullptr;
int RoundtripTest::page_count_ = 0;

// ---------------------------------------------------------------------------
// Helper: render a page, encode to PNG, decode, compare
// ---------------------------------------------------------------------------

struct PageVerifyResult {
    bool success;
    int diff_pixels;
    int max_diff;
    int dec_width;
    int dec_height;
};

static PageVerifyResult VerifyPage(FPDF_DOCUMENT doc, int page_idx,
                                    float dpi, int compression) {
    auto* page = FPDF_LoadPage(doc, page_idx);
    if (!page) return {false, -1, -1, 0, 0};

    const auto scale = dpi / 72.0f;
    const auto width  = static_cast<int>(FPDF_GetPageWidth(page) * scale + 0.5f);
    const auto height = static_cast<int>(FPDF_GetPageHeight(page) * scale + 0.5f);
    const auto stride = (width * 4 + 63) & ~63;
    const auto buf_size = static_cast<size_t>(stride) * height;

    std::vector<uint8_t> buffer(buf_size);
    auto* bitmap = FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRx,
                                        buffer.data(), stride);
    if (!bitmap) { FPDF_ClosePage(page); return {false, -1, -1, 0, 0}; }

    FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, 0,
                          FPDF_ANNOT | FPDF_PRINTING | FPDF_NO_CATCH);

    // Encode to PNG in memory
    uint8_t* png_data = nullptr;
    size_t png_size = 0;
    auto rc = fast_png::WriteBgraToMemory(buffer.data(), width, height, stride,
                                           &png_data, &png_size, compression);
    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);

    if (rc != fast_png::kSuccess || !png_data) return {false, -1, -1, 0, 0};

    // Decode PNG back
    int dec_w, dec_h, dec_ch;
    auto* decoded = stbi_load_from_memory(png_data, static_cast<int>(png_size),
                                           &dec_w, &dec_h, &dec_ch, 3);
    std::free(png_data);

    if (!decoded) return {false, -1, -1, 0, 0};

    // Compare pixels: original BGRx -> RGB vs decoded RGB
    int diff_pixels = 0;
    int max_diff = 0;

    for (int y = 0; y < height; ++y) {
        const auto* src = buffer.data() + y * stride;
        const auto* dec = decoded + y * width * 3;

        for (int x = 0; x < width; ++x) {
            const int orig_r = src[x * 4 + 2];
            const int orig_g = src[x * 4 + 1];
            const int orig_b = src[x * 4 + 0];

            const int dec_r = dec[x * 3 + 0];
            const int dec_g = dec[x * 3 + 1];
            const int dec_b = dec[x * 3 + 2];

            const int d = std::max({std::abs(orig_r - dec_r),
                                    std::abs(orig_g - dec_g),
                                    std::abs(orig_b - dec_b)});
            if (d > 0) {
                ++diff_pixels;
                if (d > max_diff) max_diff = d;
            }
        }
    }

    stbi_image_free(decoded);
    return {true, diff_pixels, max_diff, dec_w, dec_h};
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(RoundtripTest, HasPages) {
    ASSERT_GT(page_count_, 0) << "PDF has no pages";
}

TEST_F(RoundtripTest, AllPagesPixelIdenticalBest) {
    int failures = 0;
    for (int i = 0; i < page_count_; ++i) {
        auto result = VerifyPage(doc_, i, kDpi, fast_png::kCompressBest);
        ASSERT_TRUE(result.success) << "Page " << (i + 1) << " encode/decode failed";
        if (result.diff_pixels > 0) {
            ADD_FAILURE() << "Page " << (i + 1) << ": "
                          << result.diff_pixels << " pixels differ, max_diff="
                          << result.max_diff;
            ++failures;
        }
    }
    EXPECT_EQ(failures, 0) << failures << " pages had pixel differences";
}

TEST_F(RoundtripTest, AllPagesPixelIdenticalFast) {
    // Test with kCompressFast (fpng path)
    for (int i = 0; i < std::min(page_count_, 5); ++i) {
        auto result = VerifyPage(doc_, i, kDpi, fast_png::kCompressFast);
        ASSERT_TRUE(result.success) << "Page " << (i + 1) << " encode/decode failed";
        // fpng RGBA path — roundtrip should still match
        EXPECT_EQ(result.diff_pixels, 0)
            << "Page " << (i + 1) << ": " << result.diff_pixels << " pixels differ";
    }
}

TEST_F(RoundtripTest, AllPagesPixelIdenticalMedium) {
    for (int i = 0; i < std::min(page_count_, 5); ++i) {
        auto result = VerifyPage(doc_, i, kDpi, fast_png::kCompressMedium);
        ASSERT_TRUE(result.success) << "Page " << (i + 1) << " encode/decode failed";
        EXPECT_EQ(result.diff_pixels, 0)
            << "Page " << (i + 1) << ": " << result.diff_pixels << " pixels differ";
    }
}

TEST_F(RoundtripTest, DecodedDimensionsMatch) {
    for (int i = 0; i < std::min(page_count_, 3); ++i) {
        auto* page = FPDF_LoadPage(doc_, i);
        ASSERT_NE(page, nullptr);

        const auto scale = kDpi / kPointsPerInch;
        const int expected_w = static_cast<int>(FPDF_GetPageWidth(page) * scale + 0.5f);
        const int expected_h = static_cast<int>(FPDF_GetPageHeight(page) * scale + 0.5f);
        FPDF_ClosePage(page);

        auto result = VerifyPage(doc_, i, kDpi, fast_png::kCompressBest);
        ASSERT_TRUE(result.success);
        EXPECT_EQ(result.dec_width, expected_w) << "Width mismatch on page " << (i + 1);
        EXPECT_EQ(result.dec_height, expected_h) << "Height mismatch on page " << (i + 1);
    }
}
