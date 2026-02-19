"""Tests for GPU offload detection in llama_cpp backend."""

import logging
from unittest.mock import patch

import pytest
from entropi.inference import llama_cpp

LOGGER_NAME = "entropi.inference.llama_cpp"


class TestCheckGpuOffload:
    """Tests for _check_gpu_offload() warning logic."""

    def setup_method(self) -> None:
        """Reset the global check flag before each test."""
        llama_cpp._gpu_check_done = False

    def test_no_warning_when_gpu_supported(self, caplog: pytest.LogCaptureFixture) -> None:
        """No warning when llama_supports_gpu_offload returns True."""
        with (
            caplog.at_level(logging.WARNING, logger=LOGGER_NAME),
            patch("llama_cpp.llama_supports_gpu_offload", return_value=True),
        ):
            llama_cpp._check_gpu_offload()

        assert "WITHOUT GPU acceleration" not in caplog.text

    def test_warns_when_cpu_only(self, caplog: pytest.LogCaptureFixture) -> None:
        """Warning emitted when llama_supports_gpu_offload returns False."""
        with (
            caplog.at_level(logging.WARNING, logger=LOGGER_NAME),
            patch("llama_cpp.llama_supports_gpu_offload", return_value=False),
        ):
            llama_cpp._check_gpu_offload()

        assert "WITHOUT GPU acceleration" in caplog.text
        assert "llama-cpp-python" in caplog.text
        assert "abetlen/llama-cpp-python" in caplog.text

    def test_warns_when_function_missing(self, caplog: pytest.LogCaptureFixture) -> None:
        """Warning emitted when llama_supports_gpu_offload can't be imported."""
        with (
            caplog.at_level(logging.WARNING, logger=LOGGER_NAME),
            patch(
                "llama_cpp.llama_supports_gpu_offload",
                side_effect=ImportError("no such function"),
            ),
        ):
            llama_cpp._check_gpu_offload()

        assert "WITHOUT GPU acceleration" in caplog.text

    def test_runs_only_once(self, caplog: pytest.LogCaptureFixture) -> None:
        """Check runs only once regardless of how many times called."""
        with (
            caplog.at_level(logging.WARNING, logger=LOGGER_NAME),
            patch("llama_cpp.llama_supports_gpu_offload", return_value=False),
        ):
            llama_cpp._check_gpu_offload()
            llama_cpp._check_gpu_offload()
            llama_cpp._check_gpu_offload()

        assert caplog.text.count("WITHOUT GPU acceleration") == 1
