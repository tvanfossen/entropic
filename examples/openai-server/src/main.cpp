// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file main.cpp
 * @brief OpenAI-compat HTTP server example bridging librentropic.
 *
 * Endpoints (HTTP-only — no TLS):
 *   - GET  /health                  — liveness probe
 *   - GET  /v1/models               — list configured tiers as model IDs
 *   - GET  /v1/models/{model}       — single-tier metadata (404 if unknown)
 *   - POST /v1/chat/completions     — OpenAI Chat Completions (stream + non-stream)
 *   - POST /v1/completions          — OpenAI legacy single-turn completion
 *
 * Pinned to OpenAI API 2024-06-01. Out of scope: embeddings, fine-tuning,
 * files, images, audio, assistants. The point is to prove the C ABI is
 * sufficient for an OpenAI-shaped HTTP front-end; the rest is downstream.
 *
 * Knots-compliant: every function ≤ 50 SLOC, ≤ 3 returns,
 * cognitive complexity ≤ 15. Achieved via small helpers per concern.
 *
 * @version 2.1.0
 */
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <entropic/entropic.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using nlohmann::json;

namespace {

/**
 * @brief Hard-coded model registry used to satisfy /v1/models.
 *
 * The real entropic model registry is consulted via configuration; this
 * stub keeps the example self-contained. A production bridge would
 * mirror the engine's tier list.
 *
 * @utility
 * @version 2.1.0
 */
const std::vector<std::string>& known_models() {
    static const std::vector<std::string> ids = {
        "primary", "lightweight", "compactor",
    };
    return ids;
}

/**
 * @brief UTC seconds since epoch — fills the OpenAI ``created`` field.
 * @utility
 * @version 2.1.0
 */
int64_t epoch_now() {
    using namespace std::chrono;
    return duration_cast<seconds>(
        system_clock::now().time_since_epoch()).count();
}

/**
 * @brief Flatten OpenAI-shape ``messages`` into a single prompt string.
 *
 * Produces a transcript suitable for entropic_run(). Roles are inlined
 * via a "role: content\n\n" separator. The engine's own conversation
 * tracking layers on top — this is a stateless bridge per request.
 *
 * @utility
 * @version 2.1.0
 */
std::string flatten_messages(const json& messages) {
    std::string out;
    for (const auto& m : messages) {
        const auto role = m.value("role", "user");
        const auto content = m.value("content", "");
        out += role + ": " + content + "\n\n";
    }
    return out;
}

/**
 * @brief Build the OpenAI-shape Chat Completions response JSON.
 *
 * Mirrors the 2024-06-01 schema: id / object / created / model / choices,
 * with a single choice containing assistant content and a stop reason.
 *
 * @utility
 * @version 2.1.0
 */
json make_chat_response(const std::string& model, const std::string& content) {
    return json{
        {"id", "chatcmpl-entropic"},
        {"object", "chat.completion"},
        {"created", epoch_now()},
        {"model", model},
        {"choices", json::array({
            json{
                {"index", 0},
                {"message", json{
                    {"role", "assistant"}, {"content", content}}},
                {"finish_reason", "stop"},
            },
        })},
    };
}

/**
 * @brief Run the engine non-streaming and return the assistant content.
 *
 * Calls ``entropic_run`` once and frees the engine-allocated result
 * before returning. Errors surface as a runtime_error so the handler
 * can map them to HTTP 5xx.
 *
 * @utility
 * @version 2.1.0
 */
std::string run_blocking(entropic_handle_t handle, const std::string& prompt) {
    char* result = nullptr;
    auto err = entropic_run(handle, prompt.c_str(), &result);
    if (err != ENTROPIC_OK) {
        if (result != nullptr) { entropic_free(result); }
        throw std::runtime_error("entropic_run failed: code=" + std::to_string(err));
    }
    std::string out = result != nullptr ? std::string(result) : std::string();
    entropic_free(result);
    return out;
}

/**
 * @brief Format one OpenAI SSE chunk for ``stream=true`` responses.
 *
 * SSE protocol: ``data: {json}\n\n``. The terminal sentinel
 * ``data: [DONE]\n\n`` is emitted by the caller, not here.
 *
 * @utility
 * @version 2.1.0
 */
std::string make_sse_chunk(const std::string& model, const std::string& delta) {
    json obj = {
        {"id", "chatcmpl-entropic"},
        {"object", "chat.completion.chunk"},
        {"created", epoch_now()},
        {"model", model},
        {"choices", json::array({
            json{
                {"index", 0},
                {"delta", json{{"content", delta}}},
                {"finish_reason", nullptr},
            },
        })},
    };
    return "data: " + obj.dump() + "\n\n";
}

/**
 * @brief Streaming context closed over by the on_token callback.
 * @internal
 * @version 2.1.0
 */
struct StreamContext {
    httplib::DataSink* sink;
    std::string model;
};

/**
 * @brief on_token thunk for entropic_run_streaming → SSE writer.
 * @callback
 * @version 2.1.0
 */
void stream_on_token(const char* token, size_t len, void* user_data) {
    auto* ctx = static_cast<StreamContext*>(user_data);
    std::string chunk = make_sse_chunk(ctx->model, std::string(token, len));
    ctx->sink->write(chunk.data(), chunk.size());
}

/**
 * @brief Drive the engine in streaming mode, writing SSE chunks to ``sink``.
 *
 * Closes the SSE stream with ``data: [DONE]\n\n`` whether the engine
 * succeeded or errored. Errors are logged to stderr; partial output
 * already on the wire is preserved.
 *
 * @utility
 * @version 2.1.0
 */
void stream_completion(entropic_handle_t handle,
                       const std::string& model,
                       const std::string& prompt,
                       httplib::DataSink& sink) {
    StreamContext ctx{&sink, model};
    auto err = entropic_run_streaming(
        handle, prompt.c_str(), stream_on_token, &ctx, nullptr);
    if (err != ENTROPIC_OK) {
        std::cerr << "[openai-server] stream error: code=" << err << '\n';
    }
    static const std::string done = "data: [DONE]\n\n";
    sink.write(done.data(), done.size());
}

/**
 * @brief Map an exception inside a handler onto an HTTP 500 response.
 * @utility
 * @version 2.1.0
 */
void send_error(httplib::Response& res, const std::string& msg) {
    json body = {{"error", json{
        {"message", msg}, {"type", "internal_error"}}}};
    res.status = 500;
    res.set_content(body.dump(), "application/json");
}

/**
 * @brief Resolve the assistant text from a non-streaming chat request.
 *
 * Wraps :func:`flatten_messages` + :func:`run_blocking` so the
 * route handler stays compact. Throws on engine error.
 *
 * @utility
 * @version 2.1.0
 */
std::string complete_blocking(
    entropic_handle_t handle, const json& body, const std::string& model) {
    const auto prompt = flatten_messages(body.value("messages", json::array()));
    (void)model;
    return run_blocking(handle, prompt);
}

/**
 * @brief Handle POST /v1/chat/completions in non-streaming mode.
 * @utility
 * @version 2.1.0
 */
void handle_chat_blocking(entropic_handle_t handle,
                          const json& body,
                          httplib::Response& res) {
    const auto model = body.value("model", "primary");
    const auto content = complete_blocking(handle, body, model);
    res.set_content(make_chat_response(model, content).dump(),
                    "application/json");
}

/**
 * @brief Handle POST /v1/chat/completions with ``stream=true``.
 *
 * httplib's chunked-content writer keeps the connection open for the
 * duration of the engine call. The provider lambda dispatches to
 * :func:`stream_completion` once and returns.
 *
 * @utility
 * @version 2.1.0
 */
void handle_chat_streaming(entropic_handle_t handle,
                           const json& body,
                           httplib::Response& res) {
    const auto model = body.value("model", "primary");
    const auto prompt = flatten_messages(body.value("messages", json::array()));
    res.set_chunked_content_provider(
        "text/event-stream",
        [handle, model, prompt](size_t /*offset*/, httplib::DataSink& sink) {
            stream_completion(handle, model, prompt, sink);
            sink.done();
            return true;
        });
}

/**
 * @brief Decode the body of POST /v1/chat/completions or fail with 400.
 * @utility
 * @version 2.1.0
 */
bool parse_chat_body(const httplib::Request& req,
                     httplib::Response& res, json& out) {
    try {
        out = json::parse(req.body);
        return true;
    } catch (const std::exception& e) {
        json body = {{"error", json{
            {"message", std::string("invalid JSON: ") + e.what()},
            {"type", "invalid_request_error"}}}};
        res.status = 400;
        res.set_content(body.dump(), "application/json");
        return false;
    }
}

/**
 * @brief Route entry: POST /v1/chat/completions.
 * @utility
 * @version 2.1.0
 */
void route_chat_completions(entropic_handle_t handle,
                            const httplib::Request& req,
                            httplib::Response& res) {
    json body;
    if (!parse_chat_body(req, res, body)) { return; }
    const auto stream = body.value("stream", false);
    try {
        if (stream) { handle_chat_streaming(handle, body, res); }
        else { handle_chat_blocking(handle, body, res); }
    } catch (const std::exception& e) {
        send_error(res, e.what());
    }
}

/**
 * @brief Build a single model entry matching the OpenAI ``model`` shape.
 * @utility
 * @version 2.1.0
 */
json make_model_entry(const std::string& id) {
    return json{
        {"id", id}, {"object", "model"},
        {"created", epoch_now()}, {"owned_by", "entropic"},
    };
}

/**
 * @brief Build the /v1/models response from :func:`known_models`.
 * @utility
 * @version 2.1.0
 */
json make_models_listing() {
    json data = json::array();
    for (const auto& id : known_models()) {
        data.push_back(make_model_entry(id));
    }
    return json{{"object", "list"}, {"data", data}};
}

/**
 * @brief Build a 404 OpenAI-shape error body for an unknown model id.
 * @utility
 * @version 2.1.0
 */
json make_unknown_model_error(const std::string& model) {
    return json{{"error", json{
        {"message", "model '" + model + "' not found"},
        {"type", "invalid_request_error"},
        {"code", "model_not_found"},
    }}};
}

/**
 * @brief Build the OpenAI Completions (legacy) response shape.
 *
 * Differs from chat.completion: top-level ``choices[].text`` instead of
 * ``choices[].message``, and ``object: text_completion``.
 *
 * @utility
 * @version 2.1.0
 */
json make_completion_response(const std::string& model, const std::string& text) {
    return json{
        {"id", "cmpl-entropic"},
        {"object", "text_completion"},
        {"created", epoch_now()},
        {"model", model},
        {"choices", json::array({
            json{
                {"index", 0}, {"text", text}, {"logprobs", nullptr},
                {"finish_reason", "stop"},
            },
        })},
    };
}

/**
 * @brief Route entry: GET /v1/models/{model}.
 * @utility
 * @version 2.1.0
 */
void route_model_get(const httplib::Request& req, httplib::Response& res) {
    const auto& model = req.path_params.at("model");
    const auto& ids = known_models();
    if (std::find(ids.begin(), ids.end(), model) == ids.end()) {
        res.status = 404;
        res.set_content(make_unknown_model_error(model).dump(),
                        "application/json");
        return;
    }
    res.set_content(make_model_entry(model).dump(), "application/json");
}

/**
 * @brief Route entry: POST /v1/completions (legacy single-turn).
 * @utility
 * @version 2.1.0
 */
void route_completions(entropic_handle_t handle,
                       const httplib::Request& req,
                       httplib::Response& res) {
    json body;
    if (!parse_chat_body(req, res, body)) { return; }
    const auto model = body.value("model", "primary");
    const auto prompt = body.value("prompt", std::string());
    try {
        const auto text = run_blocking(handle, prompt);
        res.set_content(make_completion_response(model, text).dump(),
                        "application/json");
    } catch (const std::exception& e) {
        send_error(res, e.what());
    }
}

/**
 * @brief Wire all routes onto a configured server instance.
 * @utility
 * @version 2.1.0
 */
void register_routes(httplib::Server& server, entropic_handle_t handle) {
    server.Get("/health", [](const auto&, auto& res) {
        res.set_content(json{{"status", "ok"}}.dump(), "application/json");
    });
    server.Get("/v1/models", [](const auto&, auto& res) {
        res.set_content(make_models_listing().dump(), "application/json");
    });
    server.Get("/v1/models/:model", route_model_get);
    server.Post("/v1/chat/completions", [handle](const auto& req, auto& res) {
        route_chat_completions(handle, req, res);
    });
    server.Post("/v1/completions", [handle](const auto& req, auto& res) {
        route_completions(handle, req, res);
    });
}

/**
 * @brief Initialize the engine handle from the project working directory.
 * @utility
 * @version 2.1.0
 */
entropic_handle_t open_engine() {
    entropic_handle_t handle = nullptr;
    if (entropic_create(&handle) != ENTROPIC_OK || handle == nullptr) {
        std::cerr << "[openai-server] entropic_create failed\n";
        return nullptr;
    }
    if (entropic_configure_dir(handle, "") != ENTROPIC_OK) {
        std::cerr << "[openai-server] entropic_configure_dir failed\n";
        entropic_destroy(handle);
        return nullptr;
    }
    return handle;
}

} // namespace

/**
 * @brief Program entry — bind to PORT (default 8080) and serve forever.
 * @return 0 on clean shutdown, 1 if engine initialization failed.
 * @utility
 * @version 2.1.0
 */
int main(int argc, char** argv) {
    const int port = (argc > 1) ? std::atoi(argv[1]) : 8080;
    auto handle = open_engine();
    if (handle == nullptr) { return 1; }
    httplib::Server server;
    register_routes(server, handle);
    std::cerr << "[openai-server] listening on http://0.0.0.0:" << port << '\n';
    server.listen("0.0.0.0", port);
    entropic_destroy(handle);
    return 0;
}
