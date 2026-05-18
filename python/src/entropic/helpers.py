# SPDX-License-Identifier: Apache-2.0
"""Consumer-side helpers for the entropic Python wrapper.

These are not part of the C ABI — they are pure-Python conveniences for
consumers building on top of the engine. The primary entry point is
:func:`apply_patch`, which takes the patch text delivered by the
delegation-complete callback (gh#29, v2.1.5) and applies it to the
user's project directory via ``git apply``.

The engine NEVER applies patches itself; that is intentional (see
gh#29). Consumers are responsible for showing the diff to the user and
calling :func:`apply_patch` only after explicit user consent.

Example — wiring the gh#29 delegation-complete callback::

    import ctypes
    from entropic import (
        DELEGATION_COMPLETE_CB, EntDecision, EntDelegationResult,
        entropic_set_delegation_callbacks,
    )
    from entropic.helpers import apply_patch

    REPO = "/path/to/users/project"

    # ctypes callbacks must return plain ints (not IntEnum members).
    # ``int(EntDecision.ACCEPT)`` is the idiomatic form; bare integer
    # literals also work but lose the symbolic intent.
    def on_complete(res_ptr, _ud):
        res = res_ptr.contents
        patch = ctypes.string_at(res.patch, res.patch_len).decode("utf-8")
        if not ask_user_to_accept(patch):
            return int(EntDecision.REJECT)  # engine writes patch to pending/
        outcome = apply_patch(REPO, patch)
        return int(EntDecision.ACCEPT if outcome.applied
                   else EntDecision.REJECT)

    # Hold the CFUNCTYPE wrapper for the engine's lifetime — Python
    # would otherwise garbage-collect the trampoline.
    _CB = DELEGATION_COMPLETE_CB(on_complete)
    entropic_set_delegation_callbacks(handle, None, _CB, None)

Example — wiring the gh#30 attempt-boundary callback::

    from entropic import ATTEMPT_BOUNDARY_CB, entropic_set_attempt_boundary_cb

    def on_boundary(attempt_n, _ud):
        ui.split_section(f"--- attempt {attempt_n} ---")

    _BCB = ATTEMPT_BOUNDARY_CB(on_boundary)  # keep alive
    entropic_set_attempt_boundary_cb(handle, _BCB, None)
"""

from __future__ import annotations

import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class ApplyPatchResult:
    """Outcome of :func:`apply_patch`.

    :param applied: True if ``git apply`` exited 0.
    :param stderr: stderr captured from git (empty on success).
    :param files_touched: parsed file list from ``git apply --stat``.
    """

    applied: bool
    stderr: str
    files_touched: list[str]


## @brief Apply a unified-diff patch to a project directory via `git apply`.
## @utility
## @version 2.1.5
def apply_patch(
    repo_path: str | Path,
    patch: str | bytes,
    *,
    check_only: bool = False,
    three_way: bool = False,
) -> ApplyPatchResult:
    """Apply a unified-diff patch to a project directory.

    Wraps ``git apply`` and feeds the patch text on stdin. The function
    works on both git repos and plain directories (the engine snapshots
    via ``.gitignore`` when available but the patch is portable).

    :param repo_path: Project directory to apply the patch into.
    :param patch: Unified-diff text from
        ``ent_delegation_result_t.patch``. ``str`` is encoded as UTF-8.
    :param check_only: Pass ``--check`` — verify applicability without
        writing. Useful for previewing whether the patch will apply
        before showing the user a confirmation prompt.
    :param three_way: Pass ``--3way`` — attempt three-way merge on
        conflict. Requires the repo to be a git checkout.
    :returns: :class:`ApplyPatchResult` capturing exit status, stderr,
        and the file list (best-effort).
    :raises FileNotFoundError: If ``git`` is not on ``PATH`` or
        ``repo_path`` does not exist.
    """
    repo = Path(repo_path)
    if not repo.is_dir():
        raise FileNotFoundError(f"repo_path not a directory: {repo}")
    if shutil.which("git") is None:
        raise FileNotFoundError("git not found on PATH; required by apply_patch")

    payload = patch.encode("utf-8") if isinstance(patch, str) else patch
    if not payload:
        return ApplyPatchResult(applied=True, stderr="", files_touched=[])

    cmd = ["git", "apply"]
    if check_only:
        cmd.append("--check")
    if three_way:
        cmd.append("--3way")

    proc = subprocess.run(
        cmd,
        input=payload,
        cwd=str(repo),
        capture_output=True,
        check=False,
    )
    stderr = proc.stderr.decode("utf-8", errors="replace")

    stat = subprocess.run(
        ["git", "apply", "--stat"],
        input=payload,
        cwd=str(repo),
        capture_output=True,
        check=False,
    )
    files: list[str] = []
    for line in stat.stdout.decode("utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith(("---", "+++")):
            continue
        if "|" in stripped:
            files.append(stripped.split("|", 1)[0].strip())

    return ApplyPatchResult(
        applied=(proc.returncode == 0),
        stderr=stderr,
        files_touched=files,
    )
