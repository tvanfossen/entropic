# ============================================================================
# Entropi Dockerfile
# CUDA 12.8 with Blackwell (compute_120) support
# ============================================================================

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

# Copy project files and install (non-editable for proper permissions)
COPY pyproject.toml /app/
COPY src/ /app/src/
WORKDIR /app
RUN pip install ".[dev]" --no-cache-dir

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

WORKDIR /workspace

# Override NVIDIA entrypoint to run entropi directly
ENTRYPOINT ["entropi"]
CMD []
