# Releasing entropic

This document describes the **release workflow**. The native CUDA + CPU
tarballs are hand-built and hand-published — the free GitHub-hosted
runners cannot fit a CUDA build and self-hosted runners are out of scope.
The maintainer builds each backend tarball locally and uploads them via
`gh release create`.

The pure-Python wrapper wheel (`entropic-engine` on PyPI) **is** automated:
the `release-pypi.yml` workflow fires on the GitHub Release `published`
event, builds the wheel + sdist on `ubuntu-24.04`, and publishes to PyPI
via the OIDC trusted-publisher relationship. The wrapper has no native
build dependencies, so the OOM concern that killed the old `release.yaml`
does not apply here.

CI (`.github/workflows/ci.yml`) covers per-PR validation only:
pre-commit + unit tests + per-library coverage + distribution smoke. CUDA
builds, model tests, and benchmarks are developer-run and never executed
on hosted runners.

---

## Pre-release checklist

Before tagging:

1. **All planned proposals merged to `develop`**, with the implementation
   logs in each proposal pointing at landed commits.
2. **Every feature branch for this version merged** to `develop`, with each
   issue's implementation-log comment pointing at the landed commits.
3. **`VERSION` bumped.** Since v2.1.2 (#4) that is the whole edit —
   `CMakeLists.txt:10` and `pyproject.toml:64` both read the repo-root
   `VERSION` file, so there is no second place to keep in sync.
4. **`RELEASE_NOTES.md` drafted** (see template below).
5. **CI green** on `develop` head — `gh run list --branch develop --limit 3`
   should show the latest workflow run as "completed / success".
6. **Re-run model tests AFTER the VERSION bump rebuild**, so the captured
   `model-results-vX.Y.Z.json` is stamped with the version it audits.
   The results JSON's `version` comes from `_get_version()` — the VERSION
   *file*, read when the run starts — so a gate collected before the bump
   records the old version and the release's audit record then disagrees
   with the release. That is honest reporting, not a bug, but it makes the
   artifact useless as evidence for the version it is attached to.

   Since v2.13.0 the JSON also carries `built_version`, read from the
   tested build's generated header, and `_run_model_tests` refuses to start
   when the two disagree. That check is what catches a build directory
   which did not pick the bump up; `version` alone never could, because
   both it and the filename come from the same file.
   The assertion this step has always cited is real and still present —
   `tests/unit/api/api_version_test.cpp`, "Library version matches the
   canonical VERSION file", which reads `VERSION` at test time and compares
   it to `entropic_version()`. It is a **unit** test, so it only fires on a
   run that includes the unit lane: `inv test --model --model-only` skips
   it, and that is the invocation used to collect the results JSON in long
   bounded windows. A stale-version artifact therefore passes 87/87 and the
   mislabelling is silent on that path.

   Check `"version"` in the artifact against `VERSION` before attaching it,
   or run one un-filtered `inv test --model` (no `--model-only`) so the
   assertion runs against the same build directory the model tests used.
7. **Verify the pip wrapper covers every ENTROPIC_EXPORT** added since
   the last minor. As of v2.2.1 this is mechanical: `inv gen-bindings
   --check` runs in pre-commit and fails loud on drift. The check is
   self-tested in `tests/unit/test_gen_bindings.py`. Keep this
   checklist item as belt-and-suspenders against the case where the
   pre-commit hook is bypassed or the generator itself bit-rots.
8. **`develop → main`, the tag, and `gh release create` belong to the
   maintainer.** Claude does not do these by default — not the merge to
   `main`, not the tag, not the publish.

   The maintainer may grant any of them **case by case**, and that grant
   covers only the action it was given for: it does not carry to the next
   step, the next release, or a retry after a failure. A vague "go ahead"
   is not a grant (`.claude/CLAUDE.md`, Git Branching).

   This is how v2.13.0 shipped — Claude prepared and staged everything,
   the maintainer approved the merge, the push and the publish explicitly,
   and Claude then ran them. Recorded because this step previously read
   "Claude does not push" flatly, which had stopped matching both the
   policy and the practice.

---

## Local build

`inv release-check` configures, builds, packages, and runs the
distribution smoke test for both backends. Output lands in `dist/`.

```bash
# Clean working tree expected.
inv release-check

ls dist/
# entropic-2.1.0-linux-x86_64-cpu.tar.gz
# entropic-2.1.0-linux-x86_64-cpu.tar.gz.sha256
# entropic-2.1.0-linux-x86_64-cuda.tar.gz
# entropic-2.1.0-linux-x86_64-cuda.tar.gz.sha256
```

The CUDA tarball requires a local CUDA toolkit. Hardware-targeting flags
(`CMAKE_CUDA_ARCHITECTURES`) come from the host's `nvidia-smi` autodetect
in `tasks.py`. If you build on a host with an arch narrower than the
target audience, override via `CMAKE_CUDA_ARCHITECTURES` env var before
running `inv release-check`.

`.sha256` companion files are produced for every tarball — consumers
verify before extracting.

---

## Tag and publish

The release commit is the head of `main` after the `develop → main` merge.

```bash
git checkout main
git pull origin main

# Sanity: tag matches CMakeLists.txt project VERSION.
TAG_VERSION="v2.1.0"
CMAKE_VERSION=$(grep -oE 'VERSION [0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt \
                  | head -1 | awk '{print $2}')
[[ "v${CMAKE_VERSION}" == "${TAG_VERSION}" ]] || { echo "version mismatch"; exit 1; }

# Tag locally; do NOT push the tag (CI release flow is intentionally absent).
git tag "${TAG_VERSION}" -m "entropic ${TAG_VERSION}"

# Publish the GitHub Release pointing at the tagged commit. The release
# is created on the remote without pushing the tag separately.
#
# At each x.y.0 minor bump, also attach the model-test snapshot
# JSON so consumers and reviewers can see the GPU-validated pass/fail
# matrix that gated the release. test-reports/ is gitignored — the
# release attachment IS the audit record.
gh release create "${TAG_VERSION}" \
    --target "$(git rev-parse HEAD)" \
    --title "entropic ${TAG_VERSION}" \
    --notes-file RELEASE_NOTES.md \
    dist/entropic-2.1.0-linux-x86_64-cpu.tar.gz \
    dist/entropic-2.1.0-linux-x86_64-cpu.tar.gz.sha256 \
    dist/entropic-2.1.0-linux-x86_64-cuda.tar.gz \
    dist/entropic-2.1.0-linux-x86_64-cuda.tar.gz.sha256 \
    build/test-reports/model/results.json#model-results-${TAG_VERSION}.json

# Add --prerelease for any -rc.N tag.
```

`gh release create` uploads the tarballs to the release page and creates
the tag on the remote. There is no separate `git push origin <tag>` step
unless you tagged earlier and now want to backfill.

The `release.published` event from this `gh release create` call also
fires `.github/workflows/release-pypi.yml`, which runs on a hosted
runner: it checks out the tag, asserts `pyproject.toml` version matches,
runs `python -m build` (sdist + wheel), and publishes to PyPI via the
trusted-publisher OIDC relationship. The wheel becomes installable as
`pip install entropic-engine==X.Y.Z` within a couple minutes of the
release going live.

This sequencing is important: the workflow assumes the tarballs are
already attached to the release, because `entropic install-engine`
fetches them from the same release page. Triggering on `release.published`
(rather than tag push) guarantees the assets exist before the wheel is
published.

---

## RELEASE_NOTES.md template

Sections:

```markdown
# entropic v<X.Y.Z>

## Highlights
- One-line per major user-facing change.

## Engine bug fixes
- Brief list (E1, E2, …) — link to commits or PRs.

## New features
- Brief list per proposal that landed.

## Breaking changes
- Deletions or signature changes that consumers will notice.

## Distribution
- CPU tarball: `entropic-X.Y.Z-linux-x86_64-cpu.tar.gz` (sha256 in companion file)
- CUDA tarball: `entropic-X.Y.Z-linux-x86_64-cuda.tar.gz` (sha256 in companion file)
- Python wrapper: `pip install entropic-engine==X.Y.Z` then `entropic install-engine`

## Known limitations
- Anything cut from scope or deferred to next minor.
```

Keep it short — full per-commit narrative belongs in `git log`.

---

## Post-release

1. **Verify** the release page lists all four artifacts and that
   `gh release download "${TAG_VERSION}"` recovers them by hash.
2. **Smoke the wrapper** (depends on `release-pypi.yml` having succeeded
   — check `gh run list --workflow release-pypi.yml --limit 1`):
   `pip install entropic-engine==X.Y.Z` in a fresh venv, run
   `entropic install-engine`, confirm `~/.entropic/` was populated
   and `entropic version` works.
3. **Close referenced issues AFTER the release ships, not before.**
   Surfaced in v2.2.0: closing the issues at merge time produced closure
   comments that named the release before it existed and could not link
   the live release URL. Close the gh issues only once the release page
   is live and the wrapper wheel is on PyPI, so the closure comment can
   reference `https://github.com/.../releases/tag/${TAG_VERSION}` and
   `pip install entropic-engine==X.Y.Z`.
4. **Bump `develop`** to the next development version (e.g., `2.1.1-dev0`)
   only if there's immediate work staged; otherwise leave at `X.Y.Z` until
   the next feature lands.
5. **Move proposals** absorbed into this release from `STAGED/` to
   `COMPLETE/` via `git mv` (per the proposal workflow in `~/.claude/CLAUDE.md`).
   Use `/review-staged` — never skip the review step.

---

## Why no tag-driven CI release for native tarballs

Earlier versions of this repo carried a `release.yaml` workflow that
attempted CUDA builds on hosted runners. The free 2-core GitHub runner
has 14 GB of disk and 16 GB of RAM — both routinely exhausted by the
CUDA toolkit + nvcc multi-arch kernel compilation. Trimming the arch
list to Volta+ helped but did not solve OOM consistently. The paid
8-core runner ($0.032/min) was viable but reintroduces a billing
relationship for releases the maintainer would otherwise produce free
of cost on local hardware.

That workflow was deleted in v2.1.0. Local-build + manual `gh release
create` is the documented path for the native tarballs going forward.
If hosted-runner CUDA quotas ever change materially, this decision can
be revisited.

This constraint applies **only to the native CUDA build**. The pure-Python
wrapper wheel has no native build dependency and publishes via
`release-pypi.yml` on every release without OOM concerns.
