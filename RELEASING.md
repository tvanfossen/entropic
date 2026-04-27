# Releasing entropic

This document describes the **manual release workflow**. Releases are not
tag-driven via CI — the free GitHub-hosted runners cannot fit a CUDA build,
and self-hosted runners are out of scope. Instead, the maintainer builds
each backend tarball locally and publishes them via `gh release create`.

CI (`.github/workflows/ci.yml`) covers per-PR validation only:
pre-commit + unit tests + per-library coverage + distribution smoke. CUDA
builds, model tests, and benchmarks are developer-run and never executed
on hosted runners.

---

## Pre-release checklist

Before tagging:

1. **All planned proposals merged to `develop`**, with the implementation
   logs in each proposal pointing at landed commits.
2. **`feature/v2.1.0-*` branches merged** to `develop`. Workstream A
   (engine bug bundle), B (release infrastructure), C (cleanup) all closed.
3. **Versions in sync**:
   - `CMakeLists.txt:project(entropic VERSION x.y.z)`
   - `pyproject.toml:version = "x.y.z"`
4. **`RELEASE_NOTES.md` drafted** (see template below).
5. **CI green** on `develop` head — `gh run list --branch develop --limit 3`
   should show the latest workflow run as "completed / success".
6. **`develop → main`** merged by the maintainer (Claude does not push).

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
gh release create "${TAG_VERSION}" \
    --target "$(git rev-parse HEAD)" \
    --title "entropic ${TAG_VERSION}" \
    --notes-file RELEASE_NOTES.md \
    dist/entropic-2.1.0-linux-x86_64-cpu.tar.gz \
    dist/entropic-2.1.0-linux-x86_64-cpu.tar.gz.sha256 \
    dist/entropic-2.1.0-linux-x86_64-cuda.tar.gz \
    dist/entropic-2.1.0-linux-x86_64-cuda.tar.gz.sha256

# Add --prerelease for any -rc.N tag.
```

`gh release create` uploads the tarballs to the release page and creates
the tag on the remote. There is no separate `git push origin <tag>` step
unless you tagged earlier and now want to backfill.

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
2. **Smoke the wrapper**: `pip install entropic-engine==X.Y.Z` in a
   fresh venv, run `entropic install-engine`, confirm `~/.entropic/lib/`
   was populated and `entropic version` works.
3. **Bump `develop`** to the next development version (e.g., `2.1.1-dev0`)
   only if there's immediate work staged; otherwise leave at `X.Y.Z` until
   the next feature lands.
4. **Move proposals** absorbed into this release from `STAGED/` to
   `COMPLETE/` via `git mv` (per the proposal workflow in `~/.claude/CLAUDE.md`).
   Use `/review-staged` — never skip the review step.

---

## Why no tag-driven CI release

Earlier versions of this repo carried a `release.yaml` workflow that
attempted CUDA builds on hosted runners. The free 2-core GitHub runner
has 14 GB of disk and 16 GB of RAM — both routinely exhausted by the
CUDA toolkit + nvcc multi-arch kernel compilation. Trimming the arch
list to Volta+ helped but did not solve OOM consistently. The paid
8-core runner ($0.032/min) was viable but reintroduces a billing
relationship for releases the maintainer would otherwise produce free
of cost on local hardware.

The release workflow was deleted in v2.1.0. Local-build + manual
publish is the documented path going forward. If hosted-runner CUDA
quotas ever change materially, this decision can be revisited.
