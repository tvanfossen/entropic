---
version: 1.0.0
type: proposal
schema_version: 1
id: P2-20260225-020
title: "GPU Resource Management"
priority: P2
component: inference
author: tvanfossen
author_email: vanfosst@gmail.com
created: 2026-02-25
updated: 2026-02-25
tags: [gpu, performance, resource-management, cuda, laptop]
completed_date: null
scoped_files: []
---

# P2-20260225-020: GPU Resource Management

## Problem

During model inference, llama.cpp saturates GPU compute and memory bandwidth.
On laptops with shared thermal/power budgets, this starves other processes
(display compositor, video decode, input handling) making the system unusable
during generation. Affects both TUI usage and example consumers (e.g. pychess).

No engine-level mechanism exists to cap GPU utilization — the consumer must
manually tune llama.cpp kwargs or configure external tools.

## Scope

Engine-level configuration and startup logic to manage GPU resource consumption.
Not application-specific — any consumer benefits.

## Approaches

### 1. CUDA MPS Thread Percentage (Compute Cap)

`CUDA_MPS_ACTIVE_THREAD_PERCENTAGE` limits a process to N% of GPU streaming
multiprocessors. Must be set before CUDA context initialization.

- Engine sets env var early in orchestrator startup (before first model load)
- MPS daemon must be running externally (documented prerequisite)
- Config field: `inference.gpu_compute_percent` (default: 100, range: 10-100)

```yaml
inference:
  gpu_compute_percent: 80  # Use 80% of GPU SMs
```

**Constraint**: MPS daemon is a system service — engine can set the percentage
but can't start the daemon itself. Needs clear documentation.

### 2. Expose llama.cpp Resource Knobs as First-Class Config

Currently consumers pass these as opaque tier kwargs. Promote the most
impactful ones to documented, validated config fields:

| Field | llama.cpp param | Effect |
|-------|-----------------|--------|
| `n_batch` | `n_batch` | Tokens per eval step. Lower = less compute spike per step |
| `n_threads` | `n_threads` | CPU threads for prompt processing. Cap prevents CPU starvation |
| `n_gpu_layers` | `n_gpu_layers` | Layers offloaded to GPU. Fewer = less GPU pressure, slower inference |

These already work via kwargs — the value is validation, documentation, and
potential auto-tuning based on detected hardware.

### 3. Resource Profiles (Stretch)

Predefined profiles that set multiple knobs at once:

```yaml
inference:
  resource_profile: balanced  # or: maximum, background, minimal
```

| Profile | gpu_compute% | n_batch | n_gpu_layers | Use Case |
|---------|-------------|---------|--------------|----------|
| maximum | 100 | 512 | -1 (all) | Dedicated inference machine |
| balanced | 80 | 256 | -1 | Laptop with other apps running |
| background | 50 | 128 | 50% | Background service, minimal impact |
| minimal | 30 | 64 | 25% | Development/testing, GPU shared heavily |

## Hardware Context

- Target: RTX 4000 Pro Laptop GPU (16GB VRAM)
- Laptop GPUs share thermal/power envelope with CPU and display
- VRAM is not the bottleneck — compute saturation is
- MPS is available on all CUDA-capable GPUs (not MIG, which is A100/H100 only)

## Dependencies

- None for approach 2 (expose kwargs)
- MPS daemon availability for approach 1
- Hardware detection (optional, for auto-tuning)

## Acceptance Criteria

- [ ] Consumer can cap GPU compute percentage via config
- [ ] System remains responsive during inference on laptop hardware
- [ ] Config validated at load time (range checks, MPS availability warning)
- [ ] Documented in library-consumer-guide.md
