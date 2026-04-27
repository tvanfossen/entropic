# entropic-openai-server

OpenAI-compatible HTTP front-end over `librentropic.so`. Drop-in for
clients that speak OpenAI Chat Completions (LangChain, llama-index,
custom code with the `openai` SDK, etc.). Demonstrates that the
entropic C ABI is sufficient to build a full HTTP bridge — no C++
internal headers required, no C++ ABI crossing.

> **Status:** v2.1.0 example pinned to OpenAI API 2024-06-01.
> Endpoints implemented: `/v1/chat/completions` (streaming +
> non-streaming), `/v1/completions` (legacy single-turn), `/v1/models`,
> `/v1/models/{model}`, `/health`. Embeddings, fine-tuning, files,
> images, audio, and assistants are out of scope.

## Build

Prerequisites: a working `librentropic.so` install (see
[../../GETTING_STARTED.md](../../GETTING_STARTED.md)).

```bash
cd examples/openai-server
cmake -B build -DCMAKE_PREFIX_PATH=$HOME/.local/entropic
cmake --build build
```

The build pulls `cpp-httplib v0.18.3` and `nlohmann/json v3.11.3` via
CMake `FetchContent`. HTTP-only — no OpenSSL dependency. Add
`-DCPPHTTPLIB_OPENSSL_SUPPORT=ON` and link against OpenSSL if you need
TLS, but that is outside the example's scope.

## Run

```bash
./build/entropic-openai-server 8080
# [openai-server] listening on http://0.0.0.0:8080
```

## Try it

Non-streaming:

```bash
curl -s http://localhost:8080/v1/chat/completions \
    -H 'Content-Type: application/json' \
    -d '{
      "model": "primary",
      "messages": [{"role": "user", "content": "Say hi in five words."}]
    }' | jq .
```

Streaming (Server-Sent Events):

```bash
curl -N http://localhost:8080/v1/chat/completions \
    -H 'Content-Type: application/json' \
    -d '{
      "model": "primary",
      "stream": true,
      "messages": [{"role": "user", "content": "Count to ten."}]
    }'
```

List models:

```bash
curl -s http://localhost:8080/v1/models | jq .
```

Get a single model:

```bash
curl -s http://localhost:8080/v1/models/primary | jq .
# 404 + OpenAI-shape error body when the id is unknown.
```

Legacy completions (single-turn, non-chat):

```bash
curl -s http://localhost:8080/v1/completions \
    -H 'Content-Type: application/json' \
    -d '{"model": "primary", "prompt": "Once upon a time"}' | jq .
```

Health check:

```bash
curl -s http://localhost:8080/health
# {"status":"ok"}
```

## Wiring to the OpenAI Python SDK

```python
from openai import OpenAI

client = OpenAI(
    base_url="http://localhost:8080/v1",
    api_key="not-used",     # entropic does not authenticate
)
resp = client.chat.completions.create(
    model="primary",
    messages=[{"role": "user", "content": "hi"}],
)
print(resp.choices[0].message.content)
```

Streaming variant uses `stream=True`; the server emits OpenAI-shape
chunked SSE.

## Implementation notes

The model registry is hard-coded to `["primary", "lightweight",
"compactor"]` in `src/main.cpp::known_models()`. A production bridge
would mirror the engine's configured tier list — add a function to
`librentropic.so` that returns it (PR welcome).

The bridge is **stateless per request**: each request flattens the
incoming `messages` array into a single prompt and calls
`entropic_run` / `entropic_run_streaming`. Multi-turn context tracked
across requests is therefore the *client's* responsibility, matching
OpenAI's own contract. If you want server-side conversation memory,
key on a header (e.g., `X-Entropic-Conversation`) and route to per-key
engine handles.

`entropic_run_streaming` registers a token callback that pushes a
correctly-formatted `chat.completion.chunk` JSON onto the SSE stream.
`data: [DONE]\n\n` is emitted once the engine returns or errors.

The example respects all knots quality gates (SLOC ≤ 50, returns ≤ 3,
nesting ≤ 4, cognitive complexity ≤ 15) — small handler functions plus
a per-concern helper layout. If you fork it, keep them inside those
limits or add `^examples/openai-server/` to the knots `exclude` list.
