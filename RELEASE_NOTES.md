# entropic v2.1.9

Patch release expanding the bundled model registry and adding three
new chat-adapter classes — strictly additive, no facade ABI changes.
Bundles four issues as one cohesive registry-and-adapters slice
toward the v2.2.0 milestone: registry expansion (**gh#44**), new
adapter classes for Qwen 3.6 (**gh#45**), Gemma 4 (**gh#46**), and
Nemotron 3 (**gh#47**); partial ratchet of the gh#17 umbrella with
the remaining families (Llama 3.x, Gemma 2/3, Mistral / Mixtral,
Phi-3/4, DeepSeek, Granite, Command-R) deferred to 2.3.x.

> **Nemotron architecture-verification gate (gh#47): PASSES.** The
> Nemotron-3-Nano-4B model is a hybrid Mamba-Transformer with GGUF
> arch tag `nemotron_h`. llama.cpp ships full integration via
> `llm_build_nemotron_h : public llm_build_mamba_base`, so state
> handling is on the stable Mamba path rather than experimental.
> Adapter proceeds; gate documentation lives in
> `nemotron3_adapter.h`.

## Highlights

- **gh#44 (MEDIUM):** `data/bundled_models.yaml` adds 9 model-keyed
  entries using a `<family>_<size_or_variant>` naming convention:
  `qwen3_5_{0_8b,2b,4b,9b}`, `qwen3_6_a3b` (+ paired
  `qwen3_6_a3b_mmproj` for vision), `gemma4_{a4b,e4b,e2b}`,
  `nemotron3_nano_4b`. Tier-role keys (`primary`, `mid`,
  `lightweight`) are preserved for backward compatibility; new
  configs should reference the model-keyed entries directly. Each
  entry carries per-entry comments documenting license footprint
  (Apache-2.0 / Gemma Terms of Use / NVIDIA Open Model License) and
  the quant choice rationale.
- **gh#45 (MEDIUM):** `Qwen36Adapter` (registry key `qwen36`) —
  distinct adapter class for the Qwen 3.6 generation. Implements
  the qwen3_coder XML tool-call format (`<tool_call><function=name>
  <parameter=key>value</parameter></function></tool_call>`),
  `<tool_response>` result wrapping, OpenAI-native content-array for
  multimodal inputs, vision-aware system prompt extension. Kept
  structurally distinct from `Qwen35Adapter` per the "no version
  lumping" rule so future template divergence across the Qwen 3.x
  line lands without churning Qwen 3.5 callers.
- **gh#46 (MEDIUM):** `Gemma4Adapter` (registry key `gemma4`) —
  single adapter covers Gemma 4 26B-A4B, E4B, and E2B variants.
  Uses the GGUF-embedded chat template (`chat_format=""`) and a
  permissive parser: tagged JSON primary, bare-JSON fallback. The
  exact native tool-call syntax is to be refined empirically via
  the new model tests — the adapter header documents the open
  question and the decision criteria.
- **gh#47 (MEDIUM):** `Nemotron3Adapter` (registry key `nemotron3`)
  — first-class support for the Nemotron 3 family. Adapter header
  records the arch-verification outcome (hybrid Mamba-Transformer,
  `nemotron_h` GGUF arch, qwen3_coder XML tool-call format,
  `<think>` reasoning trace via separate special tokens). Tool
  parsing mirrors qwen3_coder; reasoning-trace stripping reuses the
  base-class primitives so no Nemotron-specific override is needed.

## Engineering changes

- **Adapter registry refactored to a dispatch table.** Going from
  two to five registered adapter families pushed
  `create_adapter()` past the knots 3-return ceiling. Lookup is
  now a `static const std::array<AdapterEntry, 4>` of
  `{key, factory}` pairs; the function stays at three returns and
  adding a sixth family is a one-row edit.
- **Tests:** 24+ new unit-test scenarios across
  `qwen36_adapter_test.cpp`, `gemma4_adapter_test.cpp`,
  `nemotron3_adapter_test.cpp` — render round-trip, well-formed +
  malformed tool-call parse, multi-turn with think+tool
  interleaving, reasoning-trace stripping, fallback ordering,
  vision content-parts (Qwen 3.6), vision system-prompt extension,
  and the `chat_format` / `format_tool_result` contracts. Full
  suite stays at 874 passing scenarios.
- **Developer-run model tests:** new
  `test_v219_{qwen36,gemma4,nemotron3}_family.cpp` exercise each
  bundled GGUF end-to-end (smoke generation + tool-call fixture)
  and SKIP cleanly when the model isn't on disk. Shared
  infrastructure in `tests/model/v219_family_test_helpers.h`
  lets a v2.1.9 family test override the default tier to load a
  specific bundled key. Run via `entropic download <key>` then
  `ctest -L model -R v219`.

## New features
- 9 new bundled model registry entries (gh#44)
- `Qwen36Adapter` (gh#45)
- `Gemma4Adapter` (gh#46)
- `Nemotron3Adapter` (gh#47)

## Breaking changes
- None. Registry additions only; adapter keys are new; existing
  tier-role keys (`primary`, `mid`, `lightweight`) and the
  `qwen35` / `generic` adapter keys all behave as before.

## Distribution
- CPU tarball: `entropic-2.1.9-linux-x86_64-cpu.tar.gz` (sha256 in
  companion file)
- CUDA tarball: `entropic-2.1.9-linux-x86_64-cuda.tar.gz` (sha256
  in companion file)
- Python wrapper: `pip install entropic-engine==2.1.9` then
  `entropic install-engine`

## Empirical validation (v2.1.9 model-test closure)

All 35 model tests pass (30 pre-existing + 5 new v2.1.9 family
tests), validated end-to-end against actual GGUFs on GPU:

- `qwen3_6_a3b` loaded; **Qwen 3.6 emits qwen3_coder XML** —
  `<tool_call><function=fs.read><parameter=path>...</parameter>
  </function></tool_call>`. `Qwen36Adapter` parsed it cleanly.
- `gemma4_e2b`, `gemma4_e4b`, `gemma4_a4b` all loaded; **Gemma 4
  emits tagged JSON tool calls** —
  `<tool_call>{"name":"fs.read","arguments":{...}}</tool_call>`.
  The permissive `Gemma4Adapter::parse_tool_calls` handles it via
  base-class `parse_tagged_tool_calls`. The "open question" flagged
  in `gemma4_adapter.h` is resolved: tagged JSON.
- `nemotron3_nano_4b` loaded (hybrid Mamba via `nemotron_h` arch);
  emits `<think>...</think>` reasoning then a qwen3_coder XML call.
  `Nemotron3Adapter` + base-class think-block stripping work as
  designed.

## Build / infrastructure changes bundled with v2.1.9

- **`extern/llama.cpp` bumped** from `7f2cbd9a4` (Mar 19) to
  `253ba110b` (May 14, 732 commits) — adds `LLM_ARCH_GEMMA4` which
  was required to load Gemma 4 GGUFs (arch tag `'gemma4'`). All 30
  pre-existing model tests still pass against the new pin.
- **Root `CMakeLists.txt`** now pre-defaults `CMAKE_CUDA_ARCHITECTURES`
  to `native` before `enable_language(CUDA)`. Previously CMake's
  CUDA module assigned `sm_52` (lowest supported for CUDA 12) when
  the variable wasn't set, and llama.cpp's own native-detection
  skipped its path because the value was already defined. After the
  llama.cpp bump, the new MoE-GEMV kernel could not execute on
  `sm_52` binary against `sm_120` (Blackwell) hardware. Release
  builds (`tasks.py::_build_and_stage`) still override with the
  comprehensive multi-arch list — only the dev/full preset path
  inherits the new `native` default.
- **`inv test --cpu` hardening** — the cpu-only ctest lane now
  passes `-LE model` so a stale model-test registration in
  `build/dev` (e.g., left over from a `--debug` cmake reconfigure)
  can't cause pre-commit to load a real GGUF on the CPU lane and
  hang.

## Known limitations

- **Adapter coverage** for the remaining gh#17 families (Llama 3.x,
  Gemma 2 / 3, Mistral / Mixtral, Phi-3 / Phi-4, DeepSeek,
  Granite, Command-R) is deferred to 2.3.x per the gh#17 triage —
  v2.1.9 is the first ratchet, not the close-out.
- **Gemma 4 A4B requires 24+ GB VRAM for full-context inference.**
  Its KV cache is fundamentally larger per token than Qwen 3.6 A3B
  (8 KV heads × 512 head_dim vs 2 × 256), so the same nominal MoE
  GGUF size hides a ~3× memory profile difference. The v2.1.9
  model-test harness overrides `tier.context_length = 4096` so the
  smoke + tool-call fixtures fit on a 16 GB dev GPU; production
  deployments load the full context via config.
