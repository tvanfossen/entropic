# SPDX-License-Identifier: LGPL-3.0-or-later
"""Self-tests for scripts/gen_bindings.py.

@brief Validates the auto-generated bindings drift check actually fires
       when an ENTROPIC_EXPORT is added to the header without a matching
       wrapper update. This is the v2.2.1 (gh#55) defense-in-depth.
@version 2.2.1
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_GEN_PATH = _REPO_ROOT / "scripts" / "gen_bindings.py"


def _load_gen_module():
    """Import scripts/gen_bindings.py as a module without poking sys.path."""
    spec = importlib.util.spec_from_file_location("gen_bindings_under_test", _GEN_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def gen():
    """Loaded gen_bindings module (live header surface).

    @brief Module-scope fixture so all tests share the parsed surface.
    @version 2.2.1
    """
    return _load_gen_module()


class TestGeneratorBaseline:
    """Generator runs cleanly against the committed header.

    @brief Verifies the in-tree state is in equilibrium — the committed
           _bindings.py matches what the generator produces. The
           pre-commit hook depends on this being true; this test makes
           the same assertion in the unit-test environment so failures
           surface during ``inv test`` rather than only at commit time.
    @version 2.2.1
    """

    def test_check_passes_on_committed_files(self, gen) -> None:
        src = gen.generate()
        rc = gen.check_outputs(src)
        assert rc == 0, "gen_bindings --check reports drift against committed files"


class TestGeneratorDriftDetection:
    """The check fires when the wrapper drifts from the C header.

    @brief Synthesizes a header containing a fake ENTROPIC_EXPORT that
           the committed wrapper cannot possibly contain, runs the
           generator, and verifies the resulting bindings text differs
           from the committed one. If this test passes, the pre-commit
           hook will reject any commit that adds a header export
           without regenerating the wrapper — the exact failure mode
           gh#54 was the consequence of.
    @version 2.2.1
    """

    SYNTHETIC_EXPORT = "entropic_synthetic_gh55_selftest_marker"

    def test_synthetic_export_changes_generator_output(self, gen, tmp_path: Path) -> None:
        baseline = gen.generate().bindings_py
        assert (
            self.SYNTHETIC_EXPORT not in baseline
        ), "synthetic export name collides with a real export — pick a different sentinel"
        # Stage a copy of entropic.h with one extra declaration appended
        # just before the trailing #endif. We point the generator at the
        # staged file via monkey-patching ENTROPIC_H for the duration of
        # one generate() call, then restore.
        real_header = gen.ENTROPIC_H.read_text()
        injection = (
            "\n\nENTROPIC_EXPORT entropic_error_t "
            f"{self.SYNTHETIC_EXPORT}(entropic_handle_t handle);\n"
        )
        # Inject before the closing extern-C/include-guard fences.
        anchor = "#ifdef __cplusplus\n}"
        idx = real_header.rfind(anchor)
        assert idx != -1, "could not locate extern-C close to inject synthetic export"
        staged_text = real_header[:idx] + injection + real_header[idx:]
        staged = tmp_path / "entropic.h"
        staged.write_text(staged_text)
        original_path = gen.ENTROPIC_H
        gen.ENTROPIC_H = staged
        # ENUM_SOURCES + STRUCT_SOURCES reference ENTROPIC_H as the path
        # at import time; rebind those tuples to the staged file too so
        # the generator picks up ent_decision_t, the structs, etc.
        original_enums = gen.ENUM_SOURCES
        original_structs = gen.STRUCT_SOURCES
        gen.ENUM_SOURCES = [
            (staged if path == original_path else path, name) for path, name in original_enums
        ]
        gen.STRUCT_SOURCES = [
            (staged if path == original_path else path, c, py) for path, c, py in original_structs
        ]
        try:
            mutated = gen.generate().bindings_py
        finally:
            gen.ENTROPIC_H = original_path
            gen.ENUM_SOURCES = original_enums
            gen.STRUCT_SOURCES = original_structs
        assert self.SYNTHETIC_EXPORT in mutated, (
            "generator did not emit a binding for the synthetic export — "
            "drift detector cannot defend against missed wrapper updates"
        )
        assert mutated != baseline, "mutated bindings unexpectedly equal baseline"
