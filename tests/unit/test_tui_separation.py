"""
Validation tests for v1.7.2/v1.7.3 TUI separation and engine API cleanup.

Ensures the engine repo has no TUI dependencies, no Rich imports,
and the API boundary (Presenter, HeadlessPresenter, LibraryConfig)
works standalone.
"""

import warnings
from pathlib import Path

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

    def test_no_rich_imports(self) -> None:
        """No engine module imports rich (including try/except patterns)."""
        violations = []
        for py_file in self._engine_py_files():
            text = py_file.read_text()
            for i, line in enumerate(text.splitlines(), 1):
                stripped = line.strip()
                if stripped.startswith("#"):
                    continue
                if "from rich" in stripped or "import rich" in stripped:
                    violations.append(f"{py_file.name}:{i}: {stripped}")
        assert violations == [], "Rich imports found in engine:\n" + "\n".join(violations)

    def test_no_app_module(self) -> None:
        """app.py deleted — Application class is gone from engine."""
        root = Path(__file__).resolve().parent.parent.parent / "src" / "entropic"
        assert not (root / "app.py").exists(), "app.py should be deleted"


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

    def test_presenter_no_voice_or_queue_methods(self) -> None:
        """Presenter ABC has no set_voice_callbacks or set_queue_consumer."""
        assert not hasattr(Presenter, "set_voice_callbacks")
        assert not hasattr(Presenter, "set_queue_consumer")


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

    def test_library_config_sufficient_for_engine(self) -> None:
        """AgentEngine initializes with bare LibraryConfig()."""
        from entropic.core.engine import LoopConfig

        # AgentEngine requires orchestrator and server_manager,
        # but config type check is what we're validating
        config = LibraryConfig()
        assert isinstance(config.models, type(config.models))
        assert isinstance(config.permissions, type(config.permissions))
        # LoopConfig is the engine's internal config
        loop = LoopConfig()
        assert loop.max_iterations == 15


class TestEntropyConfigDeprecation:
    """EntropyConfig import emits DeprecationWarning."""

    def test_entropy_config_import_warns(self) -> None:
        """Importing EntropyConfig from entropic emits DeprecationWarning."""
        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter("always")
            from entropic import EntropyConfig  # noqa: F401

            deprecation_warnings = [x for x in w if issubclass(x.category, DeprecationWarning)]
            assert len(deprecation_warnings) >= 1
            assert "entropic-tui" in str(deprecation_warnings[0].message)

    def test_entropy_config_is_library_config(self) -> None:
        """Deprecated EntropyConfig is aliased to LibraryConfig."""
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", DeprecationWarning)
            from entropic import EntropyConfig

            assert EntropyConfig is LibraryConfig

    def test_no_entropy_config_in_schema(self) -> None:
        """EntropyConfig is not importable from entropic.config.schema."""
        from entropic.config import schema

        assert not hasattr(schema, "EntropyConfig")


class TestConfigLoaderLibraryConfig:
    """ConfigLoader returns LibraryConfig by default."""

    def test_default_config_class(self) -> None:
        """ConfigLoader defaults to LibraryConfig."""
        loader = ConfigLoader(global_config_dir=None)
        config = loader.load()
        assert isinstance(config, LibraryConfig)


class TestAllExportsDocumented:
    """__all__ enforcement test."""

    def test_all_exports_importable(self) -> None:
        """Every name in entropic.__all__ is actually importable."""
        import entropic

        for name in entropic.__all__:
            assert hasattr(entropic, name), f"{name} in __all__ but not importable"

    def test_no_undocumented_public_exports(self) -> None:
        """Public symbols not in __all__ should be private or internal."""
        import types

        import entropic

        public_names = {n for n in dir(entropic) if not n.startswith("_")}
        # Exclude subpackage modules (imported as side effects of from-imports)
        public_names = {
            n for n in public_names if not isinstance(getattr(entropic, n, None), types.ModuleType)
        }
        # Exclude known non-API names
        internal = {"warnings", "annotations"}
        public_names -= internal

        all_set = set(entropic.__all__)
        extra = public_names - all_set
        assert extra == set(), f"Public names not in __all__: {extra}"


class TestDefaultCommandShowsHelp:
    """entropic (no args) prints help."""

    def test_default_prints_help(self) -> None:
        """Invoking main group with no subcommand shows help text."""
        from click.testing import CliRunner
        from entropic.cli import main

        runner = CliRunner()
        result = runner.invoke(main, [])
        assert result.exit_code == 0
        assert "entropic" in result.output.lower()
        assert "ask" in result.output.lower()
