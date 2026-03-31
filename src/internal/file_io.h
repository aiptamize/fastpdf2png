// fastpdf2png — File I/O utilities
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <cstddef>
#include <utility>
#include <string_view>

namespace fpdf2png::internal {

/// Read entire file into a malloc'd buffer. Caller must free().
/// Uses fstat (not ftell) for correct >2 GB support.
std::pair<uint8_t*, size_t> ReadFile(std::string_view path);

#ifndef _WIN32
/// Read file using POSIX APIs with sequential read-ahead advice.
uint8_t* ReadFileToMemory(const char* path, size_t& out_size);

bool ReadFull(int fd, void* buf, size_t count);
bool WriteFull(int fd, const void* buf, size_t count);
#endif

#ifdef _WIN32
uint8_t* ReadFileToMemoryWin(const char* path, size_t& out_size);

// HANDLE = void* on Windows; using void* here to avoid including windows.h.
bool ReadFullWin(void* h, void* buf, unsigned long count);
bool WriteFullWin(void* h, const void* buf, unsigned long count);
#endif

} // namespace fpdf2png::internal
