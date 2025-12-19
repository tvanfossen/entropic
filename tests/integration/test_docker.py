"""Integration tests for Docker deployment."""

import subprocess

import pytest


@pytest.mark.integration
class TestDocker:
    """Tests for Docker image."""

    def test_image_builds(self) -> None:
        """Test Docker image builds successfully."""
        result = subprocess.run(
            ["docker", "build", "-f", "docker/Dockerfile", "-t", "entropi:test", "."],
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0, f"Build failed: {result.stderr}"

    def test_help_command(self) -> None:
        """Test entropi --help works in container."""
        result = subprocess.run(
            ["docker", "run", "--rm", "entropi:test", "--help"],
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0
        assert "Entropi" in result.stdout or "entropi" in result.stdout.lower()

    def test_version_command(self) -> None:
        """Test entropi --version works in container."""
        result = subprocess.run(
            ["docker", "run", "--rm", "entropi:test", "--version"],
            capture_output=True,
            text=True,
        )
        assert result.returncode == 0
