#!/usr/bin/env bash
# Entropic installer — downloads and installs a prebuilt release from
# the GitHub Releases page.
#
# One-liner:
#   curl -fsSL https://raw.githubusercontent.com/tvanfossen/entropic/main/install.sh | bash
#
# Explicit version / backend / prefix:
#   curl -fsSL https://raw.githubusercontent.com/tvanfossen/entropic/main/install.sh \
#       | bash -s -- --version v2.0.5 --cuda --prefix /opt/entropic --yes
#
# Detects backend (CPU vs CUDA) by probing for `nvidia-smi`; override with
# --cpu / --cuda. Default prefix is /usr/local (requires sudo); use
# --prefix for a user-local install (no sudo needed).
#
# Installs with `tar --strip-components=1`, so the contents of the
# tarball's top-level entropic/ directory land directly in $PREFIX —
# conventional Unix layout:
#   $PREFIX/bin/entropic
#   $PREFIX/lib/librentropic.so.*
#   $PREFIX/lib/cmake/entropic/entropic-config.cmake
#   $PREFIX/share/entropic/...
#   $PREFIX/share/doc/entropic/{LICENSE,README.md}
#
# find_package(entropic 2.0 REQUIRED) in a downstream CMake project
# works against /usr/local and /opt/entropic out of the box.

set -euo pipefail

REPO="${ENTROPIC_REPO:-tvanfossen/entropic}"
VERSION=""
BACKEND=""
PREFIX="${PREFIX:-/usr/local}"
ASSUME_YES=""

usage() {
    cat <<EOF
Usage: install.sh [options]

  --version TAG    Release tag to install (e.g. v2.0.5). Default: latest stable.
  --cpu            Install the CPU tarball.
  --cuda           Install the CUDA tarball.
  --prefix DIR     Install prefix. Default: /usr/local (requires sudo).
                   Use ~/.local or /opt/entropic for a user-local install.
  --yes | -y       Skip the confirmation prompt.
  --help | -h      This help.

Env overrides:
  ENTROPIC_REPO    GitHub owner/name. Default: tvanfossen/entropic.
  PREFIX           Same as --prefix.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)     VERSION="$2"; shift 2 ;;
        --cpu)         BACKEND=cpu; shift ;;
        --cuda)        BACKEND=cuda; shift ;;
        --prefix)      PREFIX="$2"; shift 2 ;;
        --yes|-y)      ASSUME_YES=1; shift ;;
        --help|-h)     usage; exit 0 ;;
        *) echo "install.sh: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# ── Platform gate ──────────────────────────────────────────────────
OS=$(uname -s)
ARCH=$(uname -m)
if [[ "$OS" != "Linux" || "$ARCH" != "x86_64" ]]; then
    cat >&2 <<EOF
install.sh: only linux-x86_64 is supported in the v2.0.x series.
  detected: $OS / $ARCH
macOS (arm64), Windows, and Linux aarch64 are on the roadmap. For now,
build from source (see README in the repo).
EOF
    exit 1
fi

# ── Backend auto-detect ────────────────────────────────────────────
if [[ -z "$BACKEND" ]]; then
    if command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi -L >/dev/null 2>&1; then
        BACKEND=cuda
        echo "install.sh: NVIDIA GPU detected; selecting CUDA backend."
    else
        BACKEND=cpu
        echo "install.sh: no NVIDIA GPU detected; selecting CPU backend."
    fi
fi

# ── Resolve tag if not explicit ────────────────────────────────────
if [[ -z "$VERSION" ]]; then
    echo "install.sh: resolving latest release tag from github.com/$REPO..."
    VERSION=$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" \
              | grep -m1 '"tag_name":' \
              | sed -E 's/.*"tag_name": *"([^"]+)".*/\1/') || true
    if [[ -z "$VERSION" ]]; then
        echo "install.sh: failed to resolve latest tag. Pass --version explicitly." >&2
        exit 1
    fi
fi

BARE_VERSION="${VERSION#v}"
ASSET="entropic-${BARE_VERSION}-linux-x86_64-${BACKEND}.tar.gz"
URL="https://github.com/${REPO}/releases/download/${VERSION}/${ASSET}"
SHA_URL="${URL}.sha256"

# ── Summary + confirm ──────────────────────────────────────────────
cat <<EOF

Entropic installer
  repo:     $REPO
  tag:      $VERSION
  backend:  $BACKEND
  asset:    $ASSET
  url:      $URL
  prefix:   $PREFIX
EOF

if [[ -z "$ASSUME_YES" ]]; then
    read -rp "Proceed? [y/N] " ans
    if [[ ! "$ans" =~ ^[Yy]$ ]]; then
        echo "Aborted."
        exit 1
    fi
fi

# ── Download, verify, extract ──────────────────────────────────────
TMPDIR=$(mktemp -d -t entropic-install.XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT

echo "install.sh: downloading..."
curl -fL --progress-bar -o "$TMPDIR/$ASSET"        "$URL"
curl -fL --progress-bar -o "$TMPDIR/$ASSET.sha256" "$SHA_URL"

echo "install.sh: verifying sha256..."
(cd "$TMPDIR" && sha256sum --quiet --check "$ASSET.sha256")

SUDO=""
if [[ ! -w "$PREFIX" ]]; then
    SUDO=sudo
    echo "install.sh: $PREFIX is not writable; will use sudo."
fi
$SUDO mkdir -p "$PREFIX"

echo "install.sh: extracting to $PREFIX..."
# tar contents are entropic/{bin,lib,share,include}; --strip-components=1
# maps them to $PREFIX/{bin,lib,share,include}.
$SUDO tar -C "$PREFIX" --strip-components=1 -xzf "$TMPDIR/$ASSET"

# ── Post-install ────────────────────────────────────────────────────
BIN="$PREFIX/bin/entropic"
echo
echo "install.sh: installed $VERSION ($BACKEND) to $PREFIX."
echo
echo "Verify with:"
echo "  $BIN version"

# PATH hint for non-conventional prefixes.
case "$PREFIX" in
    /usr/local|/usr) ;;
    *)
        echo
        echo "Note: $PREFIX/bin may not be on your PATH. Add it:"
        echo "  export PATH=\"$PREFIX/bin:\$PATH\""
        ;;
esac

cat <<EOF

Next steps:
  1. Download a GGUF model and point the engine at it:
       mkdir -p ~/.entropic/models
       # download your .gguf into that dir, or:
       # export ENTROPIC_MODEL_DIR=/path/to/dir
  2. C++ consumer (find_package works out of the box for this prefix):
       find_package(entropic 2.0 REQUIRED)
       target_link_libraries(my-app PRIVATE entropic::entropic)
  3. Python consumer (once published):
       pip install entropic-engine==${BARE_VERSION}

See $PREFIX/share/doc/entropic/README.md for the full guide.
EOF
