"""Tests for fastpdf2png Python bindings.

Uses pytest fixtures from conftest.py for test_pdf and binary discovery.
Tests that require a PDF are automatically skipped if no PDF is available.
"""

import struct
import tempfile
from pathlib import Path

import pytest


def test_import():
    """Verify all public API symbols are importable and callable."""
    import fastpdf2png

    assert callable(fastpdf2png.to_images)
    assert callable(fastpdf2png.to_files)
    assert callable(fastpdf2png.to_bytes)
    assert callable(fastpdf2png.page_count)
    assert callable(fastpdf2png.Engine)


def test_page_count(test_pdf):
    """Verify page_count returns a positive integer."""
    import fastpdf2png

    n = fastpdf2png.page_count(test_pdf)
    assert isinstance(n, int)
    assert n > 0


def test_to_images(test_pdf):
    """Verify to_images returns PIL Images with valid modes."""
    import fastpdf2png

    images = fastpdf2png.to_images(test_pdf, dpi=72)
    assert len(images) > 0
    for img in images:
        assert img.mode in ("L", "RGB", "RGBA")
        assert img.width > 0
        assert img.height > 0


def test_to_images_count_matches(test_pdf):
    """Verify to_images returns one image per page."""
    import fastpdf2png

    images = fastpdf2png.to_images(test_pdf, dpi=72)
    n = fastpdf2png.page_count(test_pdf)
    assert len(images) == n


def test_to_files(test_pdf):
    """Verify to_files writes PNG files to disk."""
    import fastpdf2png

    with tempfile.TemporaryDirectory() as d:
        files = fastpdf2png.to_files(test_pdf, d, dpi=72)
        assert len(files) > 0
        for f in files:
            p = Path(f)
            assert p.exists()
            assert p.stat().st_size > 0
            # Verify PNG magic bytes
            with open(p, "rb") as fh:
                magic = fh.read(4)
            assert magic == b"\x89PNG"


def test_to_bytes(test_pdf):
    """Verify to_bytes returns valid PNG byte strings."""
    import fastpdf2png

    data = fastpdf2png.to_bytes(test_pdf, dpi=72)
    assert len(data) > 0
    for b in data:
        assert isinstance(b, (bytes, bytearray))
        assert b[:4] == b"\x89PNG"
        assert len(b) > 8


def test_to_bytes_ihdr_dimensions(test_pdf):
    """Verify PNG IHDR chunk contains sensible dimensions."""
    import fastpdf2png

    data = fastpdf2png.to_bytes(test_pdf, dpi=72)
    assert len(data) > 0
    # Parse IHDR: offset 16 = width (4 bytes BE), offset 20 = height (4 bytes BE)
    first = data[0]
    assert len(first) >= 24
    width = struct.unpack(">I", first[16:20])[0]
    height = struct.unpack(">I", first[20:24])[0]
    assert width > 0
    assert height > 0


def test_engine(test_pdf):
    """Verify Engine context manager works correctly."""
    import fastpdf2png

    with fastpdf2png.Engine() as pdf:
        n = pdf.page_count(test_pdf)
        assert n > 0
        images = pdf.to_images(test_pdf, dpi=72)
        assert len(images) == n
        for img in images:
            assert img.width > 0
            assert img.height > 0


def test_engine_multiple_calls(test_pdf):
    """Verify Engine can be reused for multiple PDFs."""
    import fastpdf2png

    with fastpdf2png.Engine() as pdf:
        # Call twice on the same PDF
        images1 = pdf.to_images(test_pdf, dpi=72)
        images2 = pdf.to_images(test_pdf, dpi=72)
        assert len(images1) == len(images2)
