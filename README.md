<p align="center">
  <sub>Built for <a href="https://miruiq.com"><strong>Miruiq</strong></a> — AI-powered data extraction from PDFs and documents.</sub>
</p>

<p align="center">
  <a href="https://miruiq.com"><img src=".github/assets/miruiq_screenshot.png" alt="Miruiq" width="500"></a>
</p>

# fastpdf2png

Fast PDF to PNG converter. SIMD-optimized PNG encoding, automatic grayscale detection, multi-process scaling. MIT licensed.

[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux%20%7C%20Windows-lightgrey)]()

## Install

```bash
pip install fastpdf2png
```

Or build from source:
```bash
git clone https://github.com/nataell95/fastpdf2png.git && cd fastpdf2png
bash scripts/build.sh
```

## Usage

### CLI

```bash
./build/fastpdf2png input.pdf page_%03d.png 300 4 -c 2
```

### Python

```python
import fastpdf2png

images = fastpdf2png.to_images("doc.pdf")        # list of PIL images
fastpdf2png.to_files("doc.pdf", "output/")        # save PNGs to disk
data   = fastpdf2png.to_bytes("doc.pdf")          # raw PNG bytes
n      = fastpdf2png.page_count("doc.pdf")        # page count

# Batch processing — keep PDFium loaded between calls
with fastpdf2png.Engine() as pdf:
    for path in my_pdfs:
        images = pdf.to_images(path, dpi=150)
```

### Node.js

```js
const pdf = require("fastpdf2png");

pdf.toFiles("doc.pdf", "output/", { dpi: 150 });
const buffers = pdf.toBuffers("doc.pdf");
const count = pdf.pageCount("doc.pdf");

// Batch processing
const engine = new pdf.Engine();
await engine.toFiles("doc.pdf", "output/");
engine.close();
```

## Performance

<p align="center">
  <img src=".github/assets/scaling.svg" alt="Worker scaling" width="680">
</p>

<p align="center">
  <img src=".github/assets/benchmark.svg" alt="Benchmark" width="680">
</p>

## How it works

<p align="center">
  <img src=".github/assets/architecture.svg" alt="Architecture" width="800">
</p>

### Rendering

Google's [PDFium](https://pdfium.googlesource.com/pdfium/) (the engine inside Chromium) renders each page into a raw BGRA bitmap in memory. This gives us pixel-perfect output identical to what Chrome displays.

### Grayscale detection

Before encoding, a SIMD-accelerated pass scans every pixel to check if R == G == B. Most document pages (text, tables, charts) are grayscale — detecting this lets us encode them as 8-bit PNG instead of 24-bit RGB, cutting data size by 66% with zero quality loss. On ARM this uses NEON `vld4/vceq` intrinsics; on x86 it uses SSE/AVX2.

### PNG encoding

Instead of the standard zlib/libpng pipeline, we use a patched [libdeflate](https://github.com/ebiggers/libdeflate) with a modified hash-skip optimization that skips redundant hash table insertions for long matches (+45% throughput). The compressed data goes directly into a pre-allocated output buffer — the PNG header, IDAT chunk, and IEND trailer are assembled around it with zero intermediate copies. CRC32 checksums are computed using hardware-accelerated instructions (CRC32 on ARM, PCLMUL on x86).

### Parallelism

PDFium is not thread-safe, so we use `fork()` to create isolated worker processes. Each worker shares a single atomic page counter via `mmap`'d shared memory — workers grab the next unprocessed page with `fetch_add`, render it, and write the PNG to disk. Copy-on-write semantics mean the PDFium library and document data are shared across workers without duplicating memory.

### Thread-local pools

Each worker maintains thread-local memory pools for pixel buffers and compression scratch space. After the first page warms up the pools, subsequent pages require zero `malloc`/`free` calls in the hot path.

## CLI reference

```
fastpdf2png <input.pdf> <output_%03d.png> [dpi] [workers] [-c level]
fastpdf2png --info <input.pdf>
fastpdf2png --daemon
```

| Flag | Default | Description |
|------|---------|-------------|
| `dpi` | 150 | Output resolution |
| `workers` | 1 | Parallel processes |
| `-c 0/1/2` | 2 | Compression: fast / medium / best |
| `--info` | | Print page count |
| `--daemon` | | Persistent mode (stdin commands) |

## Platforms

| OS | Arch | SIMD |
|----|------|------|
| macOS | arm64 | NEON |
| macOS | x86_64 | AVX2, SSE4.1 |
| Linux | x86_64 | AVX2, SSE4.1 |
| Linux | arm64 | NEON |
| Windows | x86_64 | AVX2, SSE4.1 |

## License

MIT. See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

