# Entropic 3.0.0 Roadmap Context: Embedded Robotics Consumer Requirements

## Source

Distilled from exploratory session on 2026-03-12. Related: game proposal P2-20260310-001, 2.0.0 game requirements doc.

## What This Document Is

A future-looking capture of engine requirements surfaced by an embedded robotics consumer (robot vacuum on Axera AX630C NPU). **This is NOT 2.0.0 scope.** These are ideas and requirements for a 3.0.0 cycle that builds on the 2.0.0 C++ engine with additional backend and routing abstractions.

Entropic is NOT a robotics framework. It is a local-first agentic inference engine. Every feature here must be designed as a general-purpose engine capability. The robot vacuum is one consumer.

---

## Target Hardware

| Spec | Value |
|---|---|
| Chip | Axera AX630C |
| NPU | 3.6 TOPS (INT8) |
| CPU | Dual Cortex-A53 @ 1.2 GHz |
| RAM | 2-4 GB LPDDR4x |
| Power | ~2-5W |
| Quantization | INT4, INT8 |
| Model ceiling | ~3-4B params (INT4), practical ~0.6B |
| Toolchain | Pulsar2 (ONNX → .axmodel), ax-llm / AXCL runtime |
| Model format | .axmodel (NOT GGUF) |
| Inference runtime | AXCL (NOT llama.cpp) |

**Key constraint**: No GPU. No CUDA. No llama.cpp. The entire inference path is different from 2.0.0's primary target.

---

## Architecture: Identity-Conditioned Subsystem Models

Same pattern as game NPC architecture, mapped to robot subsystems:

### Consumer Prompt Flow

```
User: "Go clean in front of my couch, there is a mess"
                    │
                    ▼
            ┌──────────────┐
            │ Router       │  "What subsystems needed?"
            │ (<0.1B)      │  → navigation + cleaning
            └──────┬───────┘
                   │
        ┌──────────┴──────────┐
        ▼                     ▼
┌──────────────┐      ┌──────────────┐
│ Nav Model    │      │ Cleaning     │
│ ~0.2B        │      │ Model ~0.2B  │
│ MCP: map,    │      │ MCP: sensors │
│      SLAM    │      │ Grammar:     │
│ Grammar:     │      │ {suction,    │
│ {target,     │      │  brush_speed,│
│  path_type}  │      │  water_flow} │
└──────┬───────┘      └──────┬───────┘
       │                     │
       ▼                     ▼
┌──────────────────────────────────┐
│ Coordinator / Safety Interlock   │
│ Validates combined outputs       │
│ Grammar: {approved, mods}        │
└──────────────┬───────────────────┘
               ▼
┌──────────────────────────────────┐
│ Motor Controller Model ~0.15B   │
│ MCP: motors, wheels, bumpers    │
│ Grammar: {pwm, brush, pump}     │
└──────────────────────────────────┘
```

### Subsystem Model Design

Each subsystem is a fine-tuned micro model (<0.3B) with:

- **Identity markdown** — behavioral profile (cleaning aggressiveness, obstacle sensitivity, edge behavior)
- **Scoped MCP keys** — vision can't write to motors, nav can't control suction
- **Domain-only training** — nav model doesn't know what a brush motor is (knowledge boundary = safety boundary)
- **Grammar-constrained output** — can only emit valid commands for its subsystem
- **Independent state** — per-subsystem context via per-sequence state isolation (2.0.0 feature #13)

### Identity Conditioning for Robots

Same format as game NPCs and BBEG. The identity markdown becomes the robot's behavioral profile:

```markdown
---
name: nav_subsystem
role: navigation
sensitivity: high
obstacle_avoidance: conservative
edge_behavior: slow_approach
clearance_minimum_mm: 50
---

You navigate a robot vacuum. You receive map data and SLAM
positioning via MCP tools. You output waypoints and path types.
You never command motors directly. When clearance is below
{{clearance_minimum_mm}}mm, you reroute.
```

Consumer (robot manufacturer) authors identity docs per product variant. Same base model, different behavioral profiles = different cleaning behaviors. Premium model: aggressive, thorough. Budget model: conservative, battery-preserving. Same weights, different identity doc.

### Memory Architecture (Mirrors NPC Design)

| Layer | Mechanism | Robot Analogy |
|---|---|---|
| Working memory | SSM state | Current cleaning session context |
| Episodic memory | Vector store | Room history, obstacle locations, stuck-spots |
| World state | MCP tool calls | Live sensor feeds, battery level, bin status |

The robot remembers where it got stuck last time (vector), what it was just doing (SSM), and what it sees right now (MCP). Same three-layer stack as game NPCs.

---

## New Engine Requirements (3.0.0)

### 1. Fan-Out Routing

**2.0.0 state:** Router classifies input → routes to ONE identity.

**Required:** Router classifies input → routes to MULTIPLE identities that execute in parallel. A single user prompt may require navigation + cleaning + scheduling subsystems simultaneously.

**Engine design:**
- Router output changes from `tier_id` to `vector<tier_id>` with optional dependency ordering
- Fan-out generates in parallel (batched inference, 2.0.0 feature #12)
- Results collected and passed to coordinator before actuation
- Hooks: `on_fan_out` (consumer controls which identities activate), `on_fan_in` (consumer inspects combined results)

**General-purpose value:**
- Multi-agent: user prompt activates research agent + code agent + review agent simultaneously
- IoT: "set the house to night mode" → lighting + thermostat + security subsystems
- Any scenario where one prompt maps to multiple independent actions

### 2. Coordination / Safety Interlock Layer

**2.0.0 state:** No mechanism to validate combined outputs from multiple identities before they take effect.

**Required:** After fan-out, a coordinator model (or rule engine) validates that combined subsystem outputs don't conflict before actuation. Nav says "go under couch" + Cleaning says "max water" → coordinator rejects (low clearance + water = damage).

**Engine design:**
- `CoordinatorConfig` — registered per-deployment, receives combined fan-out outputs
- Coordinator can be: a model (grammar-constrained validation pass), a rule engine (deterministic), or a hook (consumer logic)
- Coordinator output: `{approved: bool, modifications: [...], reason: string}`
- If rejected, fan-out results are discarded and the engine can retry with modifications or escalate

**General-purpose value:**
- Safety-critical systems: any deployment where combined actions could cause harm
- Workflow validation: multiple agent outputs checked for consistency before execution
- Game: BBEG validates NPC actions don't violate world laws (already partially covered by grammar, but cross-NPC conflicts need coordination)

### 3. Backend Abstraction (InferenceBackend Interface) — MOVED TO 2.0.0

**Moved to 2.0.0 roadmap as Feature #17 (P1).** The `InferenceBackend` concrete base class is designed into the C++ rewrite from the start. `LlamaCppBackend` ships as the only 2.0.0 implementation. 3.0.0 adds `AxeraBackend` as a subclass — no engine refactoring needed.

### 4. Physical Actuator Safety Constraints

**2.0.0 state:** MCP tool execution has permission checking but no concept of physical safety limits.

**Required:** Tool-level safety constraints that enforce physical limits regardless of what the model requests. Even if grammar allows `{brush_speed: 100}`, the safety layer clamps to hardware limits.

**Engine design:**
- `ToolSafetyConfig` per tool: min/max ranges, rate-of-change limits, forbidden combinations
- Enforced at the MCP execution layer, below hooks — cannot be overridden by consumer code
- Safety violations logged at ERROR level with full context
- Emergency stop: `engine.emergency_halt()` — immediately cancels all pending tool executions and generation

**General-purpose value:**
- Any deployment with physical actuators (robotics, CNC, drones)
- Rate limiting for API-calling tools (prevent runaway API costs)
- The principle: grammar constrains what the model CAN ask for, safety constrains what the system WILL do

---

## Relationship to 2.0.0

Most of the engine infrastructure needed for robotics is being built in 2.0.0 for the game consumer:

| 2.0.0 Feature | Robotics Use |
|---|---|
| Identity system | Subsystem behavioral profiles |
| MCP framework + per-caller keys | Sensor/actuator scoping |
| Grammar constraints | Valid command enforcement |
| Batched inference | Parallel subsystem models |
| Per-sequence state isolation | Per-subsystem context |
| Hooks | Safety interlocks, fault handling |
| Audit logging | Full command history |
| Blackwell runtime detection | Future NPU detection generalization |
| Multi-grammar pipeline | Perception → planning → actuation chain |

**3.0.0-specific additions:** Fan-out routing, coordination layer, physical safety constraints. These build on 2.0.0 — they don't replace it.

**NOTE: Backend abstraction should be a 2.0.0 architectural decision, not a 3.0.0 retrofit.** If `InferenceBackend` is designed as a concrete base class (per project coding standards — base class holds 80%+ of logic, subclasses override specifics) with `LlamaCppBackend` as the first implementation, adding `AxeraBackend` in 3.0.0 is writing a subclass. The DRY/KISS/concrete-base-class methodology exists precisely for this: design for extensibility by making the right abstractions early, not by predicting every future backend. The base class owns model lifecycle, state management, capability queries. Subclasses own format-specific loading and runtime-specific inference calls.

---

## VRAM/RAM Budget (AX630C, 2-4 GB)

```
Total RAM: 2-4 GB LPDDR4x (shared with OS + application)

Available for models: ~1.5-3 GB

Router (0.1B INT4):          ~0.05 GB
Nav model (0.2B INT4):       ~0.1 GB
Cleaning model (0.2B INT4):  ~0.1 GB
Motor model (0.15B INT4):    ~0.08 GB
Coordinator (0.1B INT4):     ~0.05 GB
Embedding model (50M):       ~0.03 GB
SSM state per model (×5):    ~0.05 GB
─────────────────────────────────────
Total model footprint:       ~0.46 GB

Headroom for OS + app + sensors: ~1.5-3.5 GB
```

All models fit simultaneously. No swapping needed. The AX630C's constraint is TOPS (throughput), not memory.

---

## Model Training Pipeline

Same pipeline as game NPC models, different domain corpus:

| Phase | What | Notes |
|---|---|---|
| 1. Domain corpus | Robot vacuum operations, navigation algorithms, cleaning patterns, sensor interpretation, motor control | Proprietary + synthetic |
| 2. Continued pre-training | Base model (Zamba2 or SmolLM2 at <0.5B) on domain corpus | Remove general knowledge |
| 3. Identity-conditioned fine-tune | Subsystem identity markdown → command output pairs | Same format as game NPCs |
| 4. Quantize | BF16 → INT4 via Pulsar2 for .axmodel | Different toolchain from game |
| 5. Validate | Hardware-in-the-loop testing | Physical robot, not just benchmarks |

---

## Open Questions

- Can Mamba hybrid models compile through Pulsar2? (Axera's toolchain may only support standard transformer architectures)
- Does AXCL support batched inference across multiple small models?
- Grammar constraint enforcement on NPU — is GBNF equivalent available, or does this need a CPU-side constraint layer?
- Latency requirements: robot control loops typically need <100ms response. Is 0.2B INT4 on 3.6 TOPS fast enough?
- Regulatory: does an LLM-controlled robot vacuum need safety certification beyond standard product testing?
