# ============================================================================
# Entropi Dockerfile
# CUDA 12.8 with Blackwell (compute_120) support
# ============================================================================
# syntax=docker/dockerfile:1

FROM nvidia/cuda:12.8.0-devel-ubuntu24.04

# Install system dependencies
RUN apt-get update && apt-get install -y \
    python3 \
    python3-pip \
    python3-venv \
    python3-dev \
    build-essential \
    cmake \
    git \
    curl \
    vim \
    libgomp1 \
    # LSP servers for code diagnostics
    clangd \
    nodejs \
    npm \
    # Audio libraries for voice mode (PortAudio + ALSA)
    libportaudio2 \
    portaudio19-dev \
    libasound2-dev \
    && rm -rf /var/lib/apt/lists/*

# Install pyright LSP for Python
RUN npm install -g pyright

# Create virtual environment
RUN python3 -m venv /opt/venv
ENV PATH="/opt/venv/bin:$PATH"

# Upgrade pip
RUN pip install --upgrade pip setuptools wheel

# Build llama-cpp-python with CUDA for Blackwell (compute_120)
# Link against CUDA stubs during build (driver not available in build container)
RUN ln -s /usr/local/cuda/lib64/stubs/libcuda.so /usr/local/cuda/lib64/stubs/libcuda.so.1 && \
    echo "/usr/local/cuda/lib64/stubs" > /etc/ld.so.conf.d/cuda-stubs.conf && \
    ldconfig
ENV CMAKE_ARGS="-DGGML_CUDA=on -DCMAKE_CUDA_ARCHITECTURES=120"
ENV CUDACXX=/usr/local/cuda/bin/nvcc
RUN pip install llama-cpp-python --no-cache-dir && \
    rm /usr/local/cuda/lib64/stubs/libcuda.so.1 && \
    rm /etc/ld.so.conf.d/cuda-stubs.conf && \
    ldconfig

# Install dependencies first (cached separately from source changes)
# Copy only pyproject.toml to install deps, then copy source
COPY pyproject.toml /app/
WORKDIR /app

# Create minimal src structure for pip to parse pyproject.toml
RUN mkdir -p /app/src/entropi && \
    echo '__version__ = "0.0.0"' > /app/src/entropi/__init__.py

# Install dependencies with pip cache mount (much faster rebuilds)
RUN --mount=type=cache,target=/root/.cache/pip \
    pip install ".[dev,voice]"

# Now copy actual source (this layer changes often but deps are cached)
COPY src/ /app/src/
COPY vendor/ /app/vendor/

# Reinstall to register the actual package (uses cached deps)
RUN --mount=type=cache,target=/root/.cache/pip \
    pip install ".[dev,voice]"

# Create user directory for volume mounting (UID set by docker-compose)
RUN mkdir -p /home/user && chmod 777 /home/user

# Volume mount points (configured in docker-compose.yaml):
# ~/models/gguf -> /home/user/models/gguf (read-only)
# ~/.entropi -> /home/user/.entropi (read-only, template source)
# . -> /workspace (read-write, project directory)
VOLUME /workspace

# Environment
ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONUNBUFFERED=1
# Ensure container venv is used, not any mounted .venv from host
ENV VIRTUAL_ENV=/opt/venv
ENV PYTHONPATH=/opt/venv/lib/python3.12/site-packages

WORKDIR /workspace

# Override NVIDIA entrypoint to run entropi directly
ENTRYPOINT ["entropi"]
CMD []
