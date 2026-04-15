# Manual Integration Tests

Developer-run tests that require a live model, GPU, and human evaluation
of output quality. These tests validate end-to-end behavior that cannot
be fully assessed by automated assertions.

## Test Inventory

### test1: Basic Generation

- **What:** Send simple prompts, verify coherent responses
- **Validates:** REQ-INFER-003 (streaming), REQ-LOOP-009 (finish reason)
- **Automation candidate:** Partial — response coherence needs model tests,
  but basic generation flow can use mock backend

### test2: Tier Routing

- **What:** Send prompts that should route to specific identity tiers
- **Validates:** REQ-IDEN-002 (classification), REQ-IDEN-004 (interleaving)
- **Automation candidate:** Yes — classification tests in tests/model/
  (model_routing_test.cpp when implemented)

### test3: Tool Calling Loop

- **What:** Trigger tool calls via prompts, verify tool execution and
  result injection
- **Validates:** REQ-TOOL-001 (dispatch), REQ-TOOL-002 (permissions),
  REQ-LOOP-006 (directives)
- **Automation candidate:** Partial — tool dispatch is unit-tested, but
  end-to-end model-driven tool selection needs GPU

### test4: Delegation Pipeline

- **What:** Trigger delegation from lead to child identity, verify
  child executes and returns
- **Validates:** REQ-DELEG-001 (single), REQ-DELEG-002 (pipeline),
  REQ-DELEG-003 (depth limit)
- **Automation candidate:** Partial — delegation mechanics are unit-tested,
  but model-driven delegation decisions need GPU

### test5: Grammar Constraints

- **What:** Verify grammar-constrained output (JSON, GBNF)
- **Validates:** REQ-INFER-005 (tool parsing), grammar registry
- **Automation candidate:** Yes — grammar constraint tests in tests/model/
  (model_inference_test.cpp when implemented)

### test6: Context Compaction

- **What:** Long session that triggers compaction, verify conversation
  continuity
- **Validates:** REQ-LOOP-005 (compaction), REQ-CACHE-001 (KV cache)
- **Automation candidate:** Partial — compaction logic is unit-tested,
  but quality of compacted context needs human evaluation

## Running Manual Tests

Manual tests are run via the model test scripts:

```bash
scripts/run-model-tests.sh          # Full suite (42 tests, ~7 min)
scripts/model-test-cache-check.sh   # Cache validation only
```

Reports are written to `test-reports/` with per-test sequence diagrams.

## Automation Roadmap (v1.10.1)

Tests marked "Yes" above are candidates for the v1.10.1 regression suite
(`tests/regression/`). Tests marked "Partial" need a mock-vs-model split:
mock-driven flow testing in unit/integration, model-driven quality
testing in tests/model/.
