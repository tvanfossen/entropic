#!/usr/bin/env bash
# docker_release_build.sh — in-container release build driver.
#
# Runs INSIDE an Ubuntu 22.04-based container (plain ubuntu:22.04 for
# cpu, nvidia/cuda:*-devel-ubuntu22.04 for cuda) so the produced
# librentropic.so links against glibc 2.35. A 2.35-linked binary runs
# on 22.04 AND every newer glibc (24.04, Debian 12, RHEL 9, ...) —
# the host build on 24.04 produces glibc-2.39 binaries that do NOT
# run on 22.04. Forward-compat only.
#
# Invoked by tasks.py:_build_release_docker via `docker run`. Not meant
# to be run directly on a host.
#
# Args: <backend> <version> <jobs> <host_uid> <host_gid>
#   backend     cpu | cuda
#   version     X.Y.Z (from repo VERSION)
#   jobs        parallel build jobs
#   host_uid    chown the output tarball back to the invoking user
#   host_gid    "
#
# Mounts (set by the caller):
#   /src   the repo, read-only (cmake -S /src; build dir is container-local)
#   /out   host dist/ dir, read-write (tarball + sha256 land here)
#
# @brief In-container Ubuntu-22.04 release build for glibc portability.
# @version 1

set -euo pipefail

BACKEND="${1:?backend (cpu|cuda) required}"
VERSION="${2:?version required}"
JOBS="${3:-4}"
HOST_UID="${4:-0}"
HOST_GID="${5:-0}"

export DEBIAN_FRONTEND=noninteractive

echo "══ docker build: backend=$BACKEND version=$VERSION jobs=$JOBS ══"
echo "   base: $(. /etc/os-release && echo "$PRETTY_NAME")  glibc: $(ldd --version | head -1 | grep -oE '[0-9]+\.[0-9]+$')"

# build-essential pulls gcc/g++/ld/objdump; cmake + git round it out.
# libsqlite3-dev is the one hard system dep (CMakeLists find_package
# SQLite3 >= 3.35 for the storage backend's FTS5; 22.04 ships 3.37).
# pkg-config helps find_package locate it. The nvidia/cuda devel base
# already ships nvcc + the CUDA toolkit.
apt-get update -qq
apt-get install -y -qq \
    cmake build-essential git file binutils \
    libsqlite3-dev pkg-config >/dev/null

# /src is a host-owned read-only mount; git refuses to read it as root
# ("detected dubious ownership") which blanks the embedded ggml/version
# commit. Trust it so version embedding works.
git config --global --add safe.directory '*'

BUILD_DIR="/tmp/release-$BACKEND"
STAGE_DIR="$BUILD_DIR/stage/entropic"
rm -rf "$BUILD_DIR"

# Arch list override via env (CUDA_ARCHES). Default mirrors the native
# release: Maxwell→Blackwell. Needs CUDA toolkit >= 12.8 for sm_100/120
# (devel base provides it). Trim via `inv release-check --cuda-arches=...`
# for faster iteration when you only target a known GPU fleet.
CUDA_ARCHES="${CUDA_ARCHES:-50;52;60;61;70;75;80;86;89;90;100;120}"

if [ "$BACKEND" = "cuda" ]; then
    BACKEND_FLAGS="-DENTROPIC_CUDA=ON -DENTROPIC_CPU_ONLY=OFF"
    BACKEND_FLAGS="$BACKEND_FLAGS -DCMAKE_CUDA_ARCHITECTURES=$CUDA_ARCHES"
    # GGML_CUDA_NO_VMM=ON: compile out ggml's Virtual Memory Management
    # pool, which is the ONLY thing that links the CUDA *driver* lib
    # (libcuda.so / CUDA::cuda_driver) for cuMem* symbols. libcuda ships
    # with the NVIDIA driver, NOT the toolkit, so it's absent in the
    # devel image and FindCUDAToolkit's stub resolution didn't populate
    # CUDA::cuda_driver — link failed on cuMemCreate/cuMemMap/etc.
    # Disabling VMM removes the dependency entirely (ggml falls back to
    # plain cudaMalloc — negligible for single-model loads). Deterministic
    # link with no driver/stub gymnastics.
    BACKEND_FLAGS="$BACKEND_FLAGS -DGGML_CUDA_NO_VMM=ON"
    # GGML_CUDA_NCCL defaults ON, and the nvidia/cuda devel image SHIPS
    # NCCL — so the container build links libnccl.so.2. NCCL is multi-GPU
    # collective comms; single-GPU consumers (the whole target fleet)
    # don't have libnccl.so.2 and the binary fails to load with
    # "libnccl.so.2: cannot open shared object file". The native build
    # avoided this only because the build host lacked NCCL. Force OFF so
    # the artifact never carries the dependency.
    BACKEND_FLAGS="$BACKEND_FLAGS -DGGML_CUDA_NCCL=OFF"
else
    BACKEND_FLAGS="-DENTROPIC_CUDA=OFF -DENTROPIC_CPU_ONLY=ON"
fi

# GGML_NATIVE=OFF disables ggml's default -march=native. -march=native
# bakes in the BUILD host's exact CPU ISA (e.g. AVX-512 on a modern
# Xeon/Threadripper); the resulting binary SIGILLs on an older target
# CPU that lacks those extensions (e.g. a 1650 Ti laptop's Coffee/Comet
# Lake — AVX2 but no AVX-512). OFF gives a portable baseline so the
# artifact runs on any x86-64. This is the CPU-ISA portability axis,
# orthogonal to the glibc one.
cmake -B "$BUILD_DIR" -S /src \
    $BACKEND_FLAGS \
    -DGGML_NATIVE=OFF \
    -DENTROPIC_SHARED=ON -DENTROPIC_STATIC=OFF \
    -DENTROPIC_BUILD_TESTS=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$STAGE_DIR"
cmake --build "$BUILD_DIR" --parallel "$JOBS"
cmake --install "$BUILD_DIR"

LIB="$STAGE_DIR/lib/librentropic.so.$VERSION"
[ -f "$LIB" ] || { echo "FAIL: $LIB not produced"; exit 1; }

# --- linkage validation -------------------------------------------------
echo "── linkage check ──"
# libcuda.so.1 "not found" is EXPECTED in the build container — it's the
# NVIDIA driver lib, provided by the consumer's GPU driver at runtime,
# never bundled. Exclude it from the unresolved-deps gate. Any OTHER
# "not found" is a real packaging bug.
MISSING=$(ldd "$LIB" | grep "not found" | grep -v "libcuda.so.1" || true)
if [ -n "$MISSING" ]; then
    echo "$MISSING"
    echo "FAIL: $LIB has unresolved runtime dependencies"
    exit 1
fi

# The portability gate: the highest GLIBC_x.y symbol the .so references
# must be <= 2.35 (Ubuntu 22.04). If a 2.36+ symbol leaked in, the
# binary would not load on 22.04 and the whole exercise is pointless.
MAX_GLIBC=$(objdump -T "$LIB" \
    | grep -oE 'GLIBC_[0-9]+\.[0-9]+' \
    | sort -V | tail -1 || true)
echo "   max glibc symbol referenced: ${MAX_GLIBC:-none}"
if [ -n "$MAX_GLIBC" ]; then
    ver="${MAX_GLIBC#GLIBC_}"
    if [ "$(printf '%s\n%s\n' "$ver" "2.35" | sort -V | tail -1)" != "2.35" ]; then
        echo "FAIL: $LIB references $MAX_GLIBC > GLIBC_2.35 — not 22.04-portable"
        exit 1
    fi
fi
echo "   ✓ glibc floor OK (<= 2.35, runs on Ubuntu 22.04+)"

if [ "$BACKEND" = "cuda" ] && ! ldd "$LIB" | grep -q "libcudart"; then
    echo "   WARNING: cuda backend but libcudart not linked — check CUDA build"
fi

# Portability guard: the binary must NOT carry a NEEDED entry for any
# lib the build container has but consumers won't. libcuda.so.1 is the
# one allowed exception (driver, runtime-provided). libnccl.so.2 in
# particular leaks in when GGML_CUDA_NCCL is left ON in an NCCL-bearing
# image — it loads fine here but breaks every single-GPU consumer.
SURPRISE=$(objdump -p "$LIB" | awk '/NEEDED/ {print $2}' \
    | grep -E 'libnccl' || true)
if [ -n "$SURPRISE" ]; then
    echo "FAIL: $LIB carries a non-portable NEEDED dependency: $SURPRISE"
    echo "      (consumer machines won't have it — disable the feature that links it)"
    exit 1
fi

# --- pack ---------------------------------------------------------------
ART="entropic-$VERSION-linux-x86_64-$BACKEND.tar.gz"
tar -C "$(dirname "$STAGE_DIR")" -czf "/out/$ART" "$(basename "$STAGE_DIR")"
( cd /out && sha256sum "$ART" > "$ART.sha256" )
chown "$HOST_UID:$HOST_GID" "/out/$ART" "/out/$ART.sha256"
echo "── packed /out/$ART ──"
