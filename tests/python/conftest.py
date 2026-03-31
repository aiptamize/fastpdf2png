"""Shared pytest fixtures for fastpdf2png tests."""

import os

import pytest
from pathlib import Path


def pytest_addoption(parser):
    parser.addoption("--pdf", action="store", default=None)


@pytest.fixture(scope="session")
def test_pdf(request):
    """Provide a test PDF path, or skip if unavailable."""
    path = request.config.getoption("--pdf") or os.environ.get("FASTPDF2PNG_TEST_PDF")
    if not path or not Path(path).exists():
        pytest.skip("No test PDF (set FASTPDF2PNG_TEST_PDF or --pdf)")
    return Path(path)


@pytest.fixture(scope="session")
def binary():
    """Provide the fastpdf2png binary path, or skip if unavailable."""
    path = os.environ.get("FASTPDF2PNG_BIN")
    if path and Path(path).exists():
        return Path(path)
    # Try common build locations
    for p in [
        "build/fastpdf2png",
        "build/Release/fastpdf2png",
        "build/Debug/fastpdf2png",
    ]:
        if Path(p).exists():
            return Path(p)
    pytest.skip("Binary not found (set FASTPDF2PNG_BIN)")
