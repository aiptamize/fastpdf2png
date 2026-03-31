// fastpdf2png — Single-file render command
// SPDX-License-Identifier: MIT

#pragma once

#include "cli/args.h"

namespace fpdf2png::cli {

/// Run single-file rendering (single-process or multi-process via fork/CreateProcess).
int RunRender(const ParsedArgs& args);

} // namespace fpdf2png::cli
