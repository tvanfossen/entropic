"""Unit tests for benchmark GPU utilities."""

from unittest.mock import MagicMock, patch

from entropic.benchmark.gpu import free_model, get_gpu_info, get_vram_mb


class TestGetVramMb:
    """Tests for get_vram_mb() — nvidia-smi subprocess query."""

    def test_returns_used_and_total(self) -> None:
        """Parses nvidia-smi output into (used, total) tuple."""
        mock_result = MagicMock()
        mock_result.returncode = 0
        mock_result.stdout = "4096, 24576\n"
        with patch("subprocess.run", return_value=mock_result):
            used, total = get_vram_mb()
        assert used == 4096
        assert total == 24576

    def test_returns_zeros_on_nonzero_returncode(self) -> None:
        """Returns (0, 0) when nvidia-smi is unavailable."""
        mock_result = MagicMock()
        mock_result.returncode = 1
        with patch("subprocess.run", return_value=mock_result):
            assert get_vram_mb() == (0, 0)

    def test_returns_zeros_on_malformed_output(self) -> None:
        """Returns (0, 0) when output cannot be parsed."""
        mock_result = MagicMock()
        mock_result.returncode = 0
        mock_result.stdout = "not,numbers,here\n"
        with patch("subprocess.run", return_value=mock_result):
            assert get_vram_mb() == (0, 0)

    def test_returns_zeros_on_incomplete_output(self) -> None:
        """Returns (0, 0) when fewer than 2 CSV fields returned."""
        mock_result = MagicMock()
        mock_result.returncode = 0
        mock_result.stdout = "4096\n"
        with patch("subprocess.run", return_value=mock_result):
            assert get_vram_mb() == (0, 0)


class TestGetGpuInfo:
    """Tests for get_gpu_info() — GPU metadata query."""

    def test_returns_gpu_info_dict(self) -> None:
        """Parses nvidia-smi into gpu_name, vram_total_mb, driver_version."""
        mock_result = MagicMock()
        mock_result.returncode = 0
        mock_result.stdout = "NVIDIA GeForce RTX 4090, 24564, 550.54\n"
        with patch("subprocess.run", return_value=mock_result):
            info = get_gpu_info()
        assert info["gpu_name"] == "NVIDIA GeForce RTX 4090"
        assert info["vram_total_mb"] == 24564
        assert info["driver_version"] == "550.54"

    def test_returns_empty_on_failure(self) -> None:
        """Returns {} when nvidia-smi fails."""
        mock_result = MagicMock()
        mock_result.returncode = 1
        with patch("subprocess.run", return_value=mock_result):
            assert get_gpu_info() == {}

    def test_returns_empty_on_incomplete_output(self) -> None:
        """Returns {} when fewer than 3 CSV fields returned."""
        mock_result = MagicMock()
        mock_result.returncode = 0
        mock_result.stdout = "GPU Name, 24576\n"
        with patch("subprocess.run", return_value=mock_result):
            assert get_gpu_info() == {}


class TestFreeModel:
    """Tests for free_model() — model cleanup with VRAM polling."""

    def test_calls_del_and_gc(self) -> None:
        """Deletes model and calls gc.collect()."""
        model = MagicMock()
        # Simulate VRAM below idle threshold immediately
        with (
            patch("gc.collect") as mock_gc,
            patch(
                "entropic.benchmark.gpu.get_vram_mb",
                return_value=(100, 24576),
            ),
        ):
            free_model(model)
        mock_gc.assert_called_once()

    def test_polls_until_vram_drops(self) -> None:
        """Polls VRAM until it drops below idle threshold."""
        model = MagicMock()
        # First 2 calls return high VRAM, third drops below threshold
        vram_sequence = [(5000, 24576), (5000, 24576), (500, 24576)]
        with (
            patch("gc.collect"),
            patch("time.sleep"),
            patch(
                "entropic.benchmark.gpu.get_vram_mb",
                side_effect=vram_sequence,
            ) as mock_vram,
        ):
            free_model(model)
        # First call is warmup, then polls until below threshold
        assert mock_vram.call_count == 3
