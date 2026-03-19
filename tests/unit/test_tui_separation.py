"""
Validation tests for v1.7.2 TUI separation.

Ensures the engine repo has no TUI dependencies and the API
boundary (Presenter, HeadlessPresenter, LibraryConfig) works
standalone.
"""

from pathlib import Path

import pytest
from entropic.config.loader import ConfigLoader
from entropic.config.schema import LibraryConfig
from entropic.core.headless_presenter import HeadlessPresenter
from entropic.core.presenter import Presenter, StatusInfo


class TestNoTUIImportsInEngine:
    """Scan all engine .py files for forbidden imports."""

    @staticmethod
    def _engine_py_files() -> list[Path]:
        """Collect all .py files under src/entropic/."""
        root = Path(__file__).resolve().parent.parent.parent / "src" / "entropic"
        return sorted(root.rglob("*.py"))

    def test_no_entropic_ui_imports(self) -> None:
        """No engine module imports from entropic.ui (package deleted)."""
        violations = []
        for py_file in self._engine_py_files():
            text = py_file.read_text()
            for i, line in enumerate(text.splitlines(), 1):
                if "from entropic.ui" in line or "import entropic.ui" in line:
                    violations.append(f"{py_file.name}:{i}: {line.strip()}")
        assert violations == [], "TUI imports found in engine:\n" + "\n".join(violations)

    def test_no_entropic_voice_imports(self) -> None:
        """No engine module imports from entropic.voice (package deleted)."""
        violations = []
        for py_file in self._engine_py_files():
            text = py_file.read_text()
            for i, line in enumerate(text.splitlines(), 1):
                if "from entropic.voice" in line or "import entropic.voice" in line:
                    violations.append(f"{py_file.name}:{i}: {line.strip()}")
        assert violations == [], "Voice imports found in engine:\n" + "\n".join(violations)

    def test_no_textual_imports(self) -> None:
        """No engine module imports textual directly."""
        violations = []
        for py_file in self._engine_py_files():
            text = py_file.read_text()
            for i, line in enumerate(text.splitlines(), 1):
                stripped = line.strip()
                if stripped.startswith("#"):
                    continue
                if "from textual" in stripped or "import textual" in stripped:
                    violations.append(f"{py_file.name}:{i}: {stripped}")
        assert violations == [], "Textual imports found in engine:\n" + "\n".join(violations)


class TestHeadlessPresenterRelocated:
    """Verify presenter classes are importable from their new locations."""

    def test_presenter_from_core(self) -> None:
        """Presenter ABC is importable from entropic.core.presenter."""
        assert Presenter is not None
        assert hasattr(Presenter, "run_async")
        assert hasattr(Presenter, "on_stream_chunk")

    def test_headless_presenter_from_core(self) -> None:
        """HeadlessPresenter is importable from entropic.core.headless_presenter."""
        hp = HeadlessPresenter()
        assert isinstance(hp, Presenter)

    def test_status_info_from_core(self) -> None:
        """StatusInfo is importable from entropic.core.presenter."""
        info = StatusInfo(
            model="test",
            vram_used=0,
            vram_total=0,
            tokens=0,
            context_used=0,
            context_max=16384,
        )
        assert info.model == "test"


class TestLibraryConfigStandalone:
    """LibraryConfig loads without TUI fields."""

    def test_library_config_default(self) -> None:
        """LibraryConfig() creates valid instance with defaults."""
        config = LibraryConfig()
        assert config.log_level == "INFO"
        assert config.compaction.enabled is True

    def test_library_config_has_no_tui_fields(self) -> None:
        """LibraryConfig does NOT have quality, ui, storage, voice fields."""
        config = LibraryConfig()
        assert not hasattr(config, "quality")
        assert not hasattr(config, "ui")
        assert not hasattr(config, "storage")
        assert not hasattr(config, "voice")
        assert not hasattr(config, "commands_dir")

    def test_library_config_has_lsp(self) -> None:
        """LSP config stays in LibraryConfig (used by diagnostics server)."""
        config = LibraryConfig()
        assert hasattr(config, "lsp")
        assert config.lsp.enabled is True


class TestApplicationRequiresPresenter:
    """Application raises if no presenter provided."""

    @pytest.mark.asyncio
    async def test_no_presenter_raises(self) -> None:
        """Application.run() raises RuntimeError without a presenter."""
        from entropic.app import Application

        app = Application(config=LibraryConfig())
        with pytest.raises(RuntimeError, match="No presenter provided"):
            await app.run()


class TestConfigLoaderLibraryConfig:
    """ConfigLoader returns LibraryConfig by default."""

    def test_default_config_class(self) -> None:
        """ConfigLoader defaults to LibraryConfig."""
        loader = ConfigLoader(global_config_dir=None)
        config = loader.load()
        assert isinstance(config, LibraryConfig)

    def test_no_entropy_config_in_schema(self) -> None:
        """EntropyConfig is not importable from entropic.config.schema."""
        from entropic.config import schema

        assert not hasattr(schema, "EntropyConfig")
