// Unit tests for fast_png::WriteRgbaToMemory.
// These tests use synthetic pixel buffers — no PDFium or real PDFs needed.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "png/png_writer.h"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

// stb_image for PNG decode validation (optional but present in third_party)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// RAII wrapper for malloc'd PNG output from WriteRgbaToMemory
struct PngOutput {
    uint8_t* data = nullptr;
    size_t size = 0;

    ~PngOutput() { std::free(data); }
    PngOutput() = default;
    PngOutput(const PngOutput&) = delete;
    PngOutput& operator=(const PngOutput&) = delete;
};

// Create a solid-color RGBA buffer (stride = width * 4)
static std::vector<uint8_t> MakeSolidRgba(int width, int height,
                                            uint8_t r, uint8_t g,
                                            uint8_t b, uint8_t a = 255) {
    const int stride = width * 4;
    std::vector<uint8_t> buf(static_cast<size_t>(stride) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto* p = buf.data() + y * stride + x * 4;
            p[0] = r; p[1] = g; p[2] = b; p[3] = a;
        }
    }
    return buf;
}

// Create a rainbow gradient RGBA buffer (each row is a different hue)
static std::vector<uint8_t> MakeRainbowRgba(int width, int height) {
    const int stride = width * 4;
    std::vector<uint8_t> buf(static_cast<size_t>(stride) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto* p = buf.data() + y * stride + x * 4;
            p[0] = static_cast<uint8_t>((x * 255) / std::max(width - 1, 1));
            p[1] = static_cast<uint8_t>((y * 255) / std::max(height - 1, 1));
            p[2] = static_cast<uint8_t>(128);
            p[3] = 255;
        }
    }
    return buf;
}

// Parse IHDR from a PNG buffer: extract width, height, bit_depth, color_type
static bool ParseIhdr(const uint8_t* data, size_t size,
                       uint32_t& w, uint32_t& h,
                       uint8_t& bit_depth, uint8_t& color_type) {
    if (size < 33) return false;
    // PNG signature (8) + IHDR length (4) + "IHDR" (4) + data (13)
    auto read_u32 = [](const uint8_t* p) -> uint32_t {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    };
    // Verify chunk type is IHDR
    if (std::memcmp(data + 12, "IHDR", 4) != 0) return false;
    w = read_u32(data + 16);
    h = read_u32(data + 20);
    bit_depth = data[24];
    color_type = data[25];
    return true;
}

// Validate PNG magic bytes
static bool HasPngMagic(const uint8_t* data, size_t size) {
    if (size < 8) return false;
    const uint8_t expected[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    return std::memcmp(data, expected, 8) == 0;
}

// Decode PNG via stb_image, return decoded pixels + dimensions
struct DecodedImage {
    uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int channels = 0;

    ~DecodedImage() { if (pixels) stbi_image_free(pixels); }
    explicit operator bool() const { return pixels != nullptr; }
};

static DecodedImage DecodePng(const uint8_t* data, size_t size) {
    DecodedImage img;
    img.pixels = stbi_load_from_memory(data, static_cast<int>(size),
                                        &img.width, &img.height, &img.channels, 0);
    return img;
}

// ---------------------------------------------------------------------------
// PNG magic & IHDR tests
// ---------------------------------------------------------------------------

TEST(PngWriter, SolidWhiteHasPngMagic) {
    auto pixels = MakeSolidRgba(100, 100, 255, 255, 255);
    PngOutput out;
    int rc = fast_png::WriteRgbaToMemory(pixels.data(), 100, 100, 100 * 4,
                                          &out.data, &out.size);
    ASSERT_EQ(rc, fast_png::kSuccess);
    ASSERT_NE(out.data, nullptr);
    EXPECT_TRUE(HasPngMagic(out.data, out.size));
}

TEST(PngWriter, IhdrHasCorrectDimensions) {
    constexpr int W = 320, H = 240;
    auto pixels = MakeSolidRgba(W, H, 128, 128, 128);
    PngOutput out;
    int rc = fast_png::WriteRgbaToMemory(pixels.data(), W, H, W * 4,
                                          &out.data, &out.size);
    ASSERT_EQ(rc, fast_png::kSuccess);

    uint32_t w, h;
    uint8_t bd, ct;
    ASSERT_TRUE(ParseIhdr(out.data, out.size, w, h, bd, ct));
    EXPECT_EQ(w, static_cast<uint32_t>(W));
    EXPECT_EQ(h, static_cast<uint32_t>(H));
    EXPECT_EQ(bd, 8);
}

// ---------------------------------------------------------------------------
// Grayscale detection: uniform gray -> color_type 0
// ---------------------------------------------------------------------------

TEST(PngWriter, GrayscaleDetectionBest) {
    // Solid gray (R=G=B) at compression kCompressBest should yield color_type=0
    auto pixels = MakeSolidRgba(64, 64, 180, 180, 180);
    PngOutput out;
    int rc = fast_png::WriteRgbaToMemory(pixels.data(), 64, 64, 64 * 4,
                                          &out.data, &out.size,
                                          fast_png::kCompressBest);
    ASSERT_EQ(rc, fast_png::kSuccess);

    uint32_t w, h;
    uint8_t bd, ct;
    ASSERT_TRUE(ParseIhdr(out.data, out.size, w, h, bd, ct));
    EXPECT_EQ(ct, 0) << "Grayscale page should have color_type=0 with kCompressBest";
}

TEST(PngWriter, ColorPageIsRgbBest) {
    // Rainbow gradient is definitely not grayscale
    auto pixels = MakeRainbowRgba(64, 64);
    PngOutput out;
    int rc = fast_png::WriteRgbaToMemory(pixels.data(), 64, 64, 64 * 4,
                                          &out.data, &out.size,
                                          fast_png::kCompressBest);
    ASSERT_EQ(rc, fast_png::kSuccess);

    uint32_t w, h;
    uint8_t bd, ct;
    ASSERT_TRUE(ParseIhdr(out.data, out.size, w, h, bd, ct));
    EXPECT_EQ(ct, 2) << "Color page should have color_type=2 with kCompressBest";
}

// ---------------------------------------------------------------------------
// Compression levels
// ---------------------------------------------------------------------------

class PngWriterCompression : public ::testing::TestWithParam<int> {};

TEST_P(PngWriterCompression, EncodesSuccessfully) {
    int level = GetParam();
    auto pixels = MakeRainbowRgba(200, 150);
    PngOutput out;
    int rc = fast_png::WriteRgbaToMemory(pixels.data(), 200, 150, 200 * 4,
                                          &out.data, &out.size, level);
    ASSERT_EQ(rc, fast_png::kSuccess);
    ASSERT_NE(out.data, nullptr);
    ASSERT_GT(out.size, 0u);
    EXPECT_TRUE(HasPngMagic(out.data, out.size));
}

TEST_P(PngWriterCompression, RoundtripsCorrectly) {
    int level = GetParam();
    constexpr int W = 80, H = 60;
    auto pixels = MakeRainbowRgba(W, H);
    PngOutput out;
    int rc = fast_png::WriteRgbaToMemory(pixels.data(), W, H, W * 4,
                                          &out.data, &out.size, level);
    ASSERT_EQ(rc, fast_png::kSuccess);

    auto decoded = DecodePng(out.data, out.size);
    ASSERT_TRUE(decoded) << "stb_image failed to decode PNG at compression level " << level;
    EXPECT_EQ(decoded.width, W);
    EXPECT_EQ(decoded.height, H);
}

INSTANTIATE_TEST_SUITE_P(
    AllCompressionLevels,
    PngWriterCompression,
    ::testing::Values(fast_png::kCompressFast,
                      fast_png::kCompressMedium,
                      fast_png::kCompressBest)
);

// ---------------------------------------------------------------------------
// Edge cases: tiny and extreme aspect ratios
// ---------------------------------------------------------------------------

TEST(PngWriter, OneByOnePixel) {
    auto pixels = MakeSolidRgba(1, 1, 42, 42, 42);
    PngOutput out;
    int rc = fast_png::WriteRgbaToMemory(pixels.data(), 1, 1, 4,
                                          &out.data, &out.size,
                                          fast_png::kCompressBest);
    ASSERT_EQ(rc, fast_png::kSuccess);
    EXPECT_TRUE(HasPngMagic(out.data, out.size));

    auto decoded = DecodePng(out.data, out.size);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.width, 1);
    EXPECT_EQ(decoded.height, 1);
}

TEST(PngWriter, WideImage) {
    constexpr int W = 4000, H = 10;
    auto pixels = MakeSolidRgba(W, H, 100, 200, 50);
    PngOutput out;
    int rc = fast_png::WriteRgbaToMemory(pixels.data(), W, H, W * 4,
                                          &out.data, &out.size,
                                          fast_png::kCompressBest);
    ASSERT_EQ(rc, fast_png::kSuccess);

    auto decoded = DecodePng(out.data, out.size);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.width, W);
    EXPECT_EQ(decoded.height, H);
}

TEST(PngWriter, TallImage) {
    constexpr int W = 10, H = 4000;
    auto pixels = MakeSolidRgba(W, H, 50, 100, 200);
    PngOutput out;
    int rc = fast_png::WriteRgbaToMemory(pixels.data(), W, H, W * 4,
                                          &out.data, &out.size,
                                          fast_png::kCompressBest);
    ASSERT_EQ(rc, fast_png::kSuccess);

    auto decoded = DecodePng(out.data, out.size);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.width, W);
    EXPECT_EQ(decoded.height, H);
}

// ---------------------------------------------------------------------------
// Invalid parameter handling
// ---------------------------------------------------------------------------

TEST(PngWriter, NullPixelsReturnsError) {
    PngOutput out;
    int rc = fast_png::WriteRgbaToMemory(nullptr, 100, 100, 400,
                                          &out.data, &out.size);
    EXPECT_EQ(rc, fast_png::kErrorInvalidParams);
}

TEST(PngWriter, ZeroWidthReturnsError) {
    auto pixels = MakeSolidRgba(1, 1, 0, 0, 0);
    PngOutput out;
    int rc = fast_png::WriteRgbaToMemory(pixels.data(), 0, 100, 0,
                                          &out.data, &out.size);
    EXPECT_EQ(rc, fast_png::kErrorInvalidParams);
}

TEST(PngWriter, ZeroHeightReturnsError) {
    auto pixels = MakeSolidRgba(1, 1, 0, 0, 0);
    PngOutput out;
    int rc = fast_png::WriteRgbaToMemory(pixels.data(), 100, 0, 400,
                                          &out.data, &out.size);
    EXPECT_EQ(rc, fast_png::kErrorInvalidParams);
}

TEST(PngWriter, NullOutDataReturnsError) {
    auto pixels = MakeSolidRgba(10, 10, 0, 0, 0);
    size_t sz;
    int rc = fast_png::WriteRgbaToMemory(pixels.data(), 10, 10, 40,
                                          nullptr, &sz);
    EXPECT_EQ(rc, fast_png::kErrorInvalidParams);
}

TEST(PngWriter, NullOutSizeReturnsError) {
    auto pixels = MakeSolidRgba(10, 10, 0, 0, 0);
    uint8_t* data;
    int rc = fast_png::WriteRgbaToMemory(pixels.data(), 10, 10, 40,
                                          &data, nullptr);
    EXPECT_EQ(rc, fast_png::kErrorInvalidParams);
}

// ---------------------------------------------------------------------------
// Pixel-level roundtrip verification
// ---------------------------------------------------------------------------

TEST(PngWriter, SolidColorRoundtripExact) {
    constexpr int W = 50, H = 50;
    constexpr uint8_t R = 200, G = 100, B = 50;
    auto pixels = MakeSolidRgba(W, H, R, G, B);
    PngOutput out;
    int rc = fast_png::WriteRgbaToMemory(pixels.data(), W, H, W * 4,
                                          &out.data, &out.size,
                                          fast_png::kCompressBest);
    ASSERT_EQ(rc, fast_png::kSuccess);

    auto decoded = DecodePng(out.data, out.size);
    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded.width, W);
    ASSERT_EQ(decoded.height, H);

    // Check that every decoded pixel matches the original RGB
    // (stb decodes grayscale as 1-ch, RGB as 3-ch)
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const auto* p = decoded.pixels + (y * W + x) * decoded.channels;
            if (decoded.channels == 3) {
                EXPECT_EQ(p[0], R) << "at (" << x << "," << y << ")";
                EXPECT_EQ(p[1], G) << "at (" << x << "," << y << ")";
                EXPECT_EQ(p[2], B) << "at (" << x << "," << y << ")";
            }
        }
    }
}

TEST(PngWriter, GrayscaleRoundtripExact) {
    constexpr int W = 50, H = 50;
    constexpr uint8_t V = 170;
    auto pixels = MakeSolidRgba(W, H, V, V, V);
    PngOutput out;
    int rc = fast_png::WriteRgbaToMemory(pixels.data(), W, H, W * 4,
                                          &out.data, &out.size,
                                          fast_png::kCompressBest);
    ASSERT_EQ(rc, fast_png::kSuccess);

    auto decoded = DecodePng(out.data, out.size);
    ASSERT_TRUE(decoded);

    // Grayscale PNG decoded by stb_image with channels=0 gives 1 channel
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const auto* p = decoded.pixels + (y * W + x) * decoded.channels;
            EXPECT_EQ(p[0], V) << "at (" << x << "," << y << ")";
        }
    }
}

// ---------------------------------------------------------------------------
// Stride handling: stride > width * 4 (padding bytes)
// ---------------------------------------------------------------------------

TEST(PngWriter, PaddedStride) {
    constexpr int W = 50, H = 50;
    // 64-byte aligned stride (like PDFium produces)
    constexpr int stride = (W * 4 + 63) & ~63;
    std::vector<uint8_t> buf(static_cast<size_t>(stride) * H, 0);

    // Fill with a known color
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            auto* p = buf.data() + y * stride + x * 4;
            p[0] = 99; p[1] = 88; p[2] = 77; p[3] = 255;
        }
    }

    PngOutput out;
    int rc = fast_png::WriteRgbaToMemory(buf.data(), W, H, stride,
                                          &out.data, &out.size,
                                          fast_png::kCompressBest);
    ASSERT_EQ(rc, fast_png::kSuccess);

    auto decoded = DecodePng(out.data, out.size);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.width, W);
    EXPECT_EQ(decoded.height, H);
}
