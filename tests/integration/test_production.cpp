// Production hardening tests for fastpdf2png CLI binary.
// Tests edge cases: corrupted PDFs, empty files, bad arguments, etc.
// Requires FASTPDF2PNG_BIN environment variable pointing to the CLI binary.
// Optionally requires TEST_PDF for tests that need a valid PDF.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int RunBinary(const std::string& binary,
                     const std::vector<std::string>& args,
                     std::string* out = nullptr) {
    std::string cmd = binary;
    for (const auto& a : args) cmd += " \"" + a + "\"";
    cmd += " 2>&1";
    auto* fp = popen(cmd.c_str(), "r");
    if (!fp) return -1;
    char buf[4096];
    std::string output;
    while (fgets(buf, sizeof(buf), fp)) output += buf;
    if (out) *out = output;
    return pclose(fp) >> 8;
}

static std::string WriteTempFile(const std::string& name,
                                  const void* data, size_t size) {
    auto path = fs::temp_directory_path() / name;
    std::ofstream f(path, std::ios::binary);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return path.string();
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class ProductionTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* bin_env = std::getenv("FASTPDF2PNG_BIN");
        if (!bin_env || bin_env[0] == '\0') {
            GTEST_SKIP() << "FASTPDF2PNG_BIN not set";
        }
        binary_ = bin_env;

        const char* pdf_env = std::getenv("TEST_PDF");
        if (pdf_env && pdf_env[0] != '\0') {
            pdf_path_ = pdf_env;
        }

        tmpdir_ = fs::temp_directory_path() / "fastpdf2png_gtest";
        fs::create_directories(tmpdir_);
        pattern_ = (tmpdir_ / "page_%03d.png").string();
    }

    void TearDown() override {
        if (!tmpdir_.empty() && fs::exists(tmpdir_)) {
            fs::remove_all(tmpdir_);
        }
    }

    void RequirePdf() {
        if (pdf_path_.empty()) {
            GTEST_SKIP() << "TEST_PDF not set, skipping test that needs a valid PDF";
        }
    }

    std::string binary_;
    std::string pdf_path_;
    fs::path tmpdir_;
    std::string pattern_;
};

// ---------------------------------------------------------------------------
// Corrupted / malformed PDF tests
// ---------------------------------------------------------------------------

TEST_F(ProductionTest, EmptyFileReturnsError) {
    auto path = WriteTempFile("empty.pdf", "", 0);
    auto rc = RunBinary(binary_, {path, pattern_, "150", "1"});
    EXPECT_NE(rc, 0);
    fs::remove(path);
}

TEST_F(ProductionTest, GarbageDataReturnsError) {
    std::vector<uint8_t> garbage(1024);
    for (auto& b : garbage) b = static_cast<uint8_t>(rand() % 256);
    auto path = WriteTempFile("garbage.pdf", garbage.data(), garbage.size());
    auto rc = RunBinary(binary_, {path, pattern_, "150", "1"});
    EXPECT_NE(rc, 0);
    fs::remove(path);
}

TEST_F(ProductionTest, TruncatedPdfReturnsError) {
    const char* header = "%PDF-1.4\n";
    auto path = WriteTempFile("truncated.pdf", header, strlen(header));
    auto rc = RunBinary(binary_, {path, pattern_, "150", "1"});
    EXPECT_NE(rc, 0);
    fs::remove(path);
}

TEST_F(ProductionTest, MissingFileReturnsError) {
    auto rc = RunBinary(binary_, {"/nonexistent/path/fake.pdf", pattern_, "150", "1"});
    EXPECT_NE(rc, 0);
}

// ---------------------------------------------------------------------------
// Bad argument tests
// ---------------------------------------------------------------------------

TEST_F(ProductionTest, NoArgumentsReturnsError) {
    auto rc = RunBinary(binary_, {});
    EXPECT_NE(rc, 0);
}

TEST_F(ProductionTest, MissingOutputPatternReturnsError) {
    auto rc = RunBinary(binary_, {"some.pdf"});
    EXPECT_NE(rc, 0);
}

TEST_F(ProductionTest, DpiZeroReturnsError) {
    auto path = WriteTempFile("dummy.pdf", "%PDF-1.4", 8);
    auto rc = RunBinary(binary_, {path, pattern_, "0", "1"});
    EXPECT_NE(rc, 0);
    fs::remove(path);
}

// ---------------------------------------------------------------------------
// Info mode tests (require valid PDF)
// ---------------------------------------------------------------------------

TEST_F(ProductionTest, InfoModeValidPdf) {
    RequirePdf();
    std::string out;
    auto rc = RunBinary(binary_, {"--info", pdf_path_}, &out);
    EXPECT_EQ(rc, 0);
    int pages = std::atoi(out.c_str());
    EXPECT_GT(pages, 0);
}

TEST_F(ProductionTest, InfoModeMissingFile) {
    auto rc = RunBinary(binary_, {"--info", "/nonexistent.pdf"});
    EXPECT_NE(rc, 0);
}

TEST_F(ProductionTest, InfoModeGarbage) {
    auto path = WriteTempFile("garbage2.pdf", "not a pdf", 9);
    auto rc = RunBinary(binary_, {"--info", path});
    EXPECT_NE(rc, 0);
    fs::remove(path);
}

// ---------------------------------------------------------------------------
// Output PNG validity (require valid PDF)
// ---------------------------------------------------------------------------

TEST_F(ProductionTest, OutputPngsAreValid) {
    RequirePdf();
    auto rc = RunBinary(binary_, {pdf_path_, pattern_, "150", "4"});
    ASSERT_EQ(rc, 0);

    int valid = 0, invalid = 0;
    for (const auto& entry : fs::directory_iterator(tmpdir_)) {
        if (entry.path().extension() != ".png") continue;
        int w, h, ch;
        auto* data = stbi_load(entry.path().c_str(), &w, &h, &ch, 3);
        if (data && w > 0 && h > 0) {
            ++valid;
            stbi_image_free(data);
        } else {
            ++invalid;
        }
    }
    EXPECT_GT(valid, 0) << "No valid PNGs produced";
    EXPECT_EQ(invalid, 0) << invalid << " invalid PNGs found";
}

// ---------------------------------------------------------------------------
// Compression level tests (require valid PDF)
// ---------------------------------------------------------------------------

TEST_F(ProductionTest, CompressionLevel0) {
    RequirePdf();
    auto rc = RunBinary(binary_, {pdf_path_, pattern_, "150", "1", "-c", "0"});
    EXPECT_EQ(rc, 0);

    auto first = tmpdir_ / "page_001.png";
    int w, h, ch;
    auto* data = stbi_load(first.c_str(), &w, &h, &ch, 3);
    EXPECT_NE(data, nullptr) << "Compression 0 output not decodable";
    if (data) stbi_image_free(data);
}

TEST_F(ProductionTest, CompressionLevel1) {
    RequirePdf();
    auto rc = RunBinary(binary_, {pdf_path_, pattern_, "150", "1", "-c", "1"});
    EXPECT_EQ(rc, 0);
}

TEST_F(ProductionTest, CompressionLevel2) {
    RequirePdf();
    auto rc = RunBinary(binary_, {pdf_path_, pattern_, "150", "1", "-c", "2"});
    EXPECT_EQ(rc, 0);
}

// ---------------------------------------------------------------------------
// Output path edge cases
// ---------------------------------------------------------------------------

TEST_F(ProductionTest, NonExistentOutputDirReturnsError) {
    RequirePdf();
    auto rc = RunBinary(binary_, {pdf_path_, "/nonexistent/dir/page_%03d.png", "150", "1"});
    EXPECT_NE(rc, 0);
}

TEST_F(ProductionTest, ReadOnlyOutputDirReturnsError) {
    RequirePdf();
    auto rodir = fs::temp_directory_path() / "fastpdf2png_readonly";
    fs::create_directories(rodir);
    fs::permissions(rodir, fs::perms::owner_read | fs::perms::owner_exec);

    auto pat = (rodir / "page_%03d.png").string();
    auto rc = RunBinary(binary_, {pdf_path_, pat, "150", "1"});
    EXPECT_NE(rc, 0);

    fs::permissions(rodir, fs::perms::all);
    fs::remove_all(rodir);
}

// ---------------------------------------------------------------------------
// Worker count variations (require valid PDF)
// ---------------------------------------------------------------------------

TEST_F(ProductionTest, SingleWorkerRender) {
    RequirePdf();
    auto rc = RunBinary(binary_, {pdf_path_, pattern_, "150", "1"});
    EXPECT_EQ(rc, 0);
}

TEST_F(ProductionTest, FourWorkerRender) {
    RequirePdf();
    auto rc = RunBinary(binary_, {pdf_path_, pattern_, "150", "4"});
    EXPECT_EQ(rc, 0);
}
