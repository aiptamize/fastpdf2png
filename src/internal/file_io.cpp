// fastpdf2png — File I/O utilities
// SPDX-License-Identifier: MIT

#include "internal/file_io.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace fpdf2png::internal {

// Read entire file into a malloc'd buffer using fstat (not ftell) for
// correct >2 GB support on all platforms.
std::pair<uint8_t*, size_t> ReadFile(std::string_view path) {
    std::string p(path);
    auto* f = std::fopen(p.c_str(), "rb");
    if (!f) return {nullptr, 0};

#ifndef _WIN32
    // Use fstat + fileno — correct for files > 2 GB (ftell returns long,
    // which is 32-bit on some platforms).
    struct stat st{};
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0) {
        std::fclose(f);
        return {nullptr, 0};
    }
    const auto size = static_cast<size_t>(st.st_size);
#else
    // On Windows, _fseeki64/_ftelli64 handle large files, but fstat
    // via _fileno works equally well and avoids the seek dance.
    struct _stat64 st{};
    if (_fstat64(_fileno(f), &st) != 0 || st.st_size <= 0) {
        std::fclose(f);
        return {nullptr, 0};
    }
    const auto size = static_cast<size_t>(st.st_size);
#endif

    auto* buf = static_cast<uint8_t*>(std::malloc(size));
    if (!buf) { std::fclose(f); return {nullptr, 0}; }

    const size_t read = std::fread(buf, 1, size, f);
    std::fclose(f);
    if (read != size) { std::free(buf); return {nullptr, 0}; }
    return {buf, size};
}

#ifndef _WIN32

uint8_t* ReadFileToMemory(const char* path, size_t& out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return nullptr;
#ifdef __linux__
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return nullptr; }
    auto* buf = static_cast<uint8_t*>(std::malloc(st.st_size));
    if (!buf) { close(fd); return nullptr; }
    size_t total = 0;
    while (total < static_cast<size_t>(st.st_size)) {
        auto n = read(fd, buf + total, st.st_size - total);
        if (n <= 0) break;
        total += n;
    }
    close(fd);
    out_size = total;
    return buf;
}

bool ReadFull(int fd, void* buf, size_t count) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t remaining = count;
    while (remaining > 0) {
        auto n = read(fd, p, remaining);
        if (n <= 0) return false;
        p += n;
        remaining -= n;
    }
    return true;
}

bool WriteFull(int fd, const void* buf, size_t count) {
    auto* p = static_cast<const uint8_t*>(buf);
    size_t remaining = count;
    while (remaining > 0) {
        auto n = write(fd, p, remaining);
        if (n <= 0) return false;
        p += n;
        remaining -= n;
    }
    return true;
}

#endif // !_WIN32

#ifdef _WIN32

uint8_t* ReadFileToMemoryWin(const char* path, size_t& out_size) {
    HANDLE fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(fh, &li) || li.QuadPart <= 0) { CloseHandle(fh); return nullptr; }
    auto* buf = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(li.QuadPart)));
    if (!buf) { CloseHandle(fh); return nullptr; }
    DWORD total = 0;
    while (total < static_cast<DWORD>(li.QuadPart)) {
        DWORD n = 0;
        if (!::ReadFile(fh, buf + total, static_cast<DWORD>(li.QuadPart) - total, &n, nullptr) || n == 0) break;
        total += n;
    }
    CloseHandle(fh);
    out_size = total;
    return buf;
}

bool ReadFullWin(void* h, void* buf, unsigned long count) {
    auto* p = static_cast<uint8_t*>(buf);
    DWORD remaining = count;
    while (remaining > 0) {
        DWORD n = 0;
        if (!::ReadFile(static_cast<HANDLE>(h), p, remaining, &n, nullptr) || n == 0) return false;
        p += n;
        remaining -= n;
    }
    return true;
}

bool WriteFullWin(void* h, const void* buf, unsigned long count) {
    auto* p = static_cast<const uint8_t*>(buf);
    DWORD remaining = count;
    while (remaining > 0) {
        DWORD n = 0;
        if (!::WriteFile(static_cast<HANDLE>(h), p, remaining, &n, nullptr) || n == 0) return false;
        p += n;
        remaining -= n;
    }
    return true;
}

#endif // _WIN32

} // namespace fpdf2png::internal
