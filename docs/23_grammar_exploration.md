# GBNF Grammar Exploration

This document explores how GBNF (GGML BNF) grammars could be used to constrain model output across all model tiers, potentially replacing or simplifying much of the custom adapter parsing code.

## Current Implementation

GBNF grammar support was added to `llama_cpp.py` and is currently used for:
- **MICRO model classification**: Constrains output to exactly `CODE` or `REASONING`

```python
# orchestrator.py
CLASSIFICATION_GRAMMAR = 'root ::= "CODE" | "REASONING"'
```

## Current Challenges

1. **Tool call detection is fragile**
   - Regex-based parsing in adapters (Qwen2, Qwen3)
   - False positives when model explains code containing tool-like patterns
   - Different models use different output formats

2. **Each adapter has custom parsing logic**
   - `parse_tool_calls()` with multiple fallback patterns
   - JSON recovery for malformed output
   - Shell-style and Python-style syntax parsing

3. **Models sometimes output malformed tool calls**
   - Trailing commas in JSON
   - Unquoted keys
   - Mixed formats within same response

## Grammar Solution

GBNF constrains output at the token-generation level. The model can ONLY produce tokens that match the grammar pattern. This guarantees:
- Valid output format
- No need for recovery/retry logic
- Consistent behavior across model tiers

## Per-Tier Grammar Ideas

### MICRO (Classification Router)
**Status: Implemented**

```gbnf
root ::= "CODE" | "REASONING"
```

Benefits:
- Guaranteed valid classification
- No string parsing needed
- Eliminates "hello" misclassification issue

### THINKING (Deep Reasoning)

Could enforce think-then-respond pattern:

```gbnf
root ::= think response
think ::= "<think>" content "</think>"
response ::= text | tool_call
content ::= [^<]+ | "<" [^/] content
text ::= [^{<]+
tool_call ::= tool_call_json | tool_call_tagged
tool_call_json ::= "{" ws "\"name\":" ws string "," ws "\"arguments\":" ws object "}"
tool_call_tagged ::= "<tool_call>" ws tool_call_json ws "</tool_call>"
```

Considerations:
- Forces thinking even when not needed
- May need to be optional

### NORMAL/CODE (Tool Calling)

Constrain to valid response OR tool call format:

```gbnf
root ::= response | tool_call

response ::= [^{<]+

tool_call ::= "{" ws "\"name\":" ws string "," ws "\"arguments\":" ws object "}" ws

# JSON primitives
ws ::= [ \t\n]*
string ::= "\"" [^"]* "\""
object ::= "{" ws (pair (ws "," ws pair)*)? ws "}"
pair ::= string ws ":" ws value
value ::= string | number | object | array | "true" | "false" | "null"
array ::= "[" ws (value (ws "," ws value)*)? ws "]"
number ::= "-"? [0-9]+ ("." [0-9]+)?
```

This would:
- Guarantee valid JSON for tool calls
- Eliminate malformed output recovery
- Remove need for regex patterns

## Example: Full Tool Call Grammar

A comprehensive grammar for Qwen2.5-Coder style output:

```gbnf
# Root: either explanation text or a tool call
root ::= (explanation tool_call*) | tool_call+

# Free-form explanation (no { at start of line)
explanation ::= line*
line ::= [^\n{]* "\n"

# Tool call must be valid JSON
tool_call ::= "{" ws name_field "," ws arguments_field "}" ws

name_field ::= "\"name\"" ws ":" ws string
arguments_field ::= "\"arguments\"" ws ":" ws object

# JSON primitives
ws ::= [ \t\n]*
string ::= "\"" chars "\""
chars ::= char*
char ::= [^"\\] | "\\" escape
escape ::= ["\\nrtbf/]

object ::= "{" ws "}" | "{" ws members ws "}"
members ::= pair | pair "," ws members
pair ::= string ws ":" ws value

array ::= "[" ws "]" | "[" ws elements ws "]"
elements ::= value | value "," ws elements

value ::= string | number | object | array | "true" | "false" | "null"

number ::= int frac? exp?
int ::= "-"? ("0" | [1-9] [0-9]*)
frac ::= "." [0-9]+
exp ::= [eE] [+-]? [0-9]+
```

## Benefits of Grammar Approach

1. **Guaranteed valid output format**
   - No parsing failures
   - No malformed JSON
   - No ambiguous patterns

2. **Reduced adapter complexity**
   - Simpler `parse_tool_calls()` - just JSON decode
   - No regex fallbacks
   - No recovery logic

3. **Consistent behavior across models**
   - Same output format regardless of model
   - Easier testing and debugging

4. **Better error handling**
   - If output doesn't match, we know immediately
   - No false positives from code explanations

## Considerations

1. **Grammar affects generation speed**
   - Usually minimal overhead
   - May slow down long responses slightly
   - Need benchmarking

2. **Grammar must match model's natural style**
   - Too restrictive = forced unnatural output
   - Too loose = doesn't provide constraint benefit
   - May need per-model-family tuning

3. **Complex grammars are hard to debug**
   - Start simple, expand gradually
   - Test with edge cases
   - Keep grammars in separate files for version control

4. **Some flexibility is lost**
   - Model can't freely mix formats
   - May need multiple grammars for different contexts
   - Consider when to apply vs when to skip

## Implementation Path

### Phase 1: Classification (Done)
- MICRO router uses grammar
- Validates approach works

### Phase 2: Simple Tool Calls
- Add grammar option to main models
- Start with simple JSON-only grammar
- Compare output quality vs current regex approach

### Phase 3: Complex Responses
- Grammar that allows explanation + tool calls
- Handle thinking blocks
- Support streaming with grammar

### Phase 4: Evaluation
- Benchmark generation speed
- Compare parsing reliability
- Document model-specific quirks

## File Locations

If grammar files are added:

```
src/entropi/data/grammars/
  classification.gbnf   # CODE | REASONING
  tool_call.gbnf        # JSON tool call format
  thinking.gbnf         # <think>...</think> + response
```

Load via:
```python
from pathlib import Path

GRAMMAR_DIR = Path(__file__).parent.parent / "data" / "grammars"

def load_grammar(name: str) -> str:
    path = GRAMMAR_DIR / f"{name}.gbnf"
    return path.read_text()
```

## References

- [llama.cpp GBNF documentation](https://github.com/ggerganov/llama.cpp/blob/master/grammars/README.md)
- [llama-cpp-python grammar support](https://llama-cpp-python.readthedocs.io/en/latest/)
- [JSON grammar example](https://github.com/ggerganov/llama.cpp/blob/master/grammars/json.gbnf)
