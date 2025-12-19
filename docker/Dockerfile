# ============================================================================
# Entropi Production Dockerfile
# Multi-stage build with CUDA support
# ============================================================================

# Stage 1: Build llama-cpp-python wheel
FROM nvidia/cuda:12.4-devel-ubuntu24.04 AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    python3 \
    python3-pip \
    python3-venv \
    python3-dev \
    build-essential \
    cmake \
    git \
    && rm -rf /var/lib/apt/lists/*

# Create virtual environment
RUN python3 -m venv /opt/venv
ENV PATH="/opt/venv/bin:$PATH"

# Upgrade pip
RUN pip install --upgrade pip setuptools wheel

# Build llama-cpp-python with CUDA
# Using sm_89 for Ada architecture, adjust for your GPU
ENV CMAKE_ARGS="-DGGML_CUDA=on -DCMAKE_CUDA_ARCHITECTURES=89"
RUN pip install llama-cpp-python --no-cache-dir

# Install entropi dependencies
COPY pyproject.toml /build/
WORKDIR /build
RUN pip install . --no-cache-dir

# Stage 2: Runtime image
FROM nvidia/cuda:12.4-runtime-ubuntu24.04 AS runtime

# Install Python runtime
RUN apt-get update && apt-get install -y \
    python3 \
    python3-venv \
    git \
    && rm -rf /var/lib/apt/lists/*

# Copy virtual environment from builder
COPY --from=builder /opt/venv /opt/venv
ENV PATH="/opt/venv/bin:$PATH"

# Copy application code
COPY src/ /app/src/
COPY pyproject.toml /app/
WORKDIR /app

# Install application
RUN pip install -e . --no-deps

# Create non-root user
RUN useradd -m -s /bin/bash entropi
USER entropi

# Model volume mount point
VOLUME /models

# Workspace volume mount point
VOLUME /workspace

# Default working directory
WORKDIR /workspace

# Environment
ENV ENTROPI_MODELS_DIR=/models
ENV ENTROPI_CONFIG_DIR=/home/entropi/.entropi

# Create config directory
RUN mkdir -p /home/entropi/.entropi

# Entry point
ENTRYPOINT ["entropi"]
CMD ["--help"]
