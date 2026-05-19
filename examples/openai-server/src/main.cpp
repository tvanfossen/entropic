// SPDX-License-Identifier: Apache-2.0
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
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>
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
 * @brief Per-request tempfile guard (gh#38, v2.1.8).
 *
 * Holds decoded `data:` image bytes on disk for the duration of one
 * Chat Completions request. Files unlink on destruction so a thrown
 * exception, an error response, or successful completion all leave
 * /tmp clean. Engine-side ImagePreprocessor needs file paths today;
 * a future preprocess_buffer hop could eliminate the disk write.
 *
 * @version 2.1.8
 */
class RequestTempfiles {
public:
    /**
     * @brief Create a new temp file and write `bytes` into it.
     * @return Filesystem path (owned for guard lifetime).
     * @utility
     * @version 2.1.8
     */
    std::filesystem::path write(const std::vector<uint8_t>& bytes,
                                const std::string& ext) {
        auto tmpl = std::filesystem::temp_directory_path()
            / ("entropic-img-XXXXXX" + ext);
        std::string buf = tmpl.string();
        // mkstemps wants a writable cstr + suffix length.
        int fd = mkstemps(buf.data(), static_cast<int>(ext.size()));
        if (fd < 0) {
            throw std::runtime_error("mkstemps failed for image tempfile");
        }
        ::close(fd);
        std::ofstream out(buf, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close();
        std::filesystem::path p(buf);
        paths_.push_back(p);
        return p;
    }

    /**
     * @brief Unlink all tempfiles created during the request.
     * @utility
     * @version 2.1.8
     */
    ~RequestTempfiles() {
        std::error_code ec;
        for (const auto& p : paths_) {
            std::filesystem::remove(p, ec);
        }
    }

    RequestTempfiles() = default;
    RequestTempfiles(const RequestTempfiles&) = delete;
    RequestTempfiles& operator=(const RequestTempfiles&) = delete;

private:
    std::vector<std::filesystem::path> paths_;
};

/**
 * @brief Decode a base64-encoded string (gh#38, v2.1.8).
 *
 * Standard base64 only (RFC 4648 §4). Whitespace inside the input is
 * tolerated; `=` padding is consumed. Throws on invalid characters.
 *
 * @param s Base64 source.
 * @return Decoded bytes.
 * @utility
 * @version 2.1.8
 */
std::vector<uint8_t> base64_decode(const std::string& s) {
    static constexpr std::array<int, 256> tbl = []() {
        std::array<int, 256> t{};
        for (auto& v : t) { v = -1; }
        const char* a =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) {
            t[static_cast<unsigned char>(a[i])] = i;
        }
        return t;
    }();
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4);
    int val = 0;
    int valb = -8;
    for (unsigned char c : s) {
        if (c == '=' || std::isspace(c)) { continue; }
        int d = tbl[c];
        if (d < 0) {
            throw std::runtime_error("invalid base64 character");
        }
        val = (val << 6) | d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xff));
            valb -= 8;
        }
    }
    return out;
}

/**
 * @brief Sniff a file extension from a `data:image/<mime>;base64,...` header.
 * @param data_url Full data URI string.
 * @return ".png" / ".jpg" / ".webp" / ".bin" fallback.
 * @utility
 * @version 2.1.8
 */
std::string sniff_image_ext(const std::string& data_url) {
    auto p = data_url.find('/');
    auto q = (p == std::string::npos)
        ? std::string::npos
        : data_url.find(';', p + 1);
    if (p == std::string::npos || p + 1 >= data_url.size()
            || q == std::string::npos) {
        return ".bin";
    }
    std::string mime = data_url.substr(p + 1, q - p - 1);
    return (mime == "jpeg" || mime == "jpg") ? ".jpg" : ("." + mime);
}

/**
 * @brief Process one OpenAI `image_url` value → canonical image path.
 *
 * - `data:image/<mime>;base64,<payload>` → base64-decode into a request
 *   tempfile and return its path.
 * - `file://` → strip scheme, return path.
 * - Absolute path (`/...`) → return as-is.
 * - `http(s)://...` → reject with std::runtime_error (mapped to HTTP 400
 *   by the caller).
 *
 * @param url Raw OpenAI image_url.url string.
 * @param tempfiles Per-request tempfile guard.
 * @return Filesystem path the engine can hand to ImagePreprocessor.
 * @utility
 * @version 2.1.8
 */
std::filesystem::path process_image_url(
    const std::string& url, RequestTempfiles& tempfiles) {
    if (url.rfind("data:", 0) == 0) {
        auto comma = url.find(',');
        if (comma == std::string::npos) {
            throw std::runtime_error("malformed data: URL (no comma)");
        }
        auto bytes = base64_decode(url.substr(comma + 1));
        return tempfiles.write(bytes, sniff_image_ext(url));
    }
    if (url.rfind("file://", 0) == 0) { return url.substr(7); }
    if (!url.empty() && url[0] == '/') { return url; }
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
        throw std::runtime_error(
            "http(s):// image URLs are not supported by the example server "
            "(SSRF surface). Use a data: URL, file:// path, or absolute path.");
    }
    throw std::runtime_error("unrecognized image_url scheme: " + url);
}

/**
 * @brief Convert one OpenAI message → engine canonical message (gh#38).
 *
 * `content` may be a string (passes through unchanged) or an array of
 * `{type: text|image_url, ...}` parts (rewritten to engine
 * `{type: text|image, ...}` shape with image paths resolved). Order
 * within a message is preserved.
 *
 * @param m OpenAI-shape message object.
 * @param tempfiles Per-request tempfile guard.
 * @return Engine-canonical message JSON.
 * @utility
 * @version 2.1.8
 */
json convert_openai_message(const json& m, RequestTempfiles& tempfiles) {
    json out;
    out["role"] = m.value("role", "user");
    if (!m.contains("content") || !m["content"].is_array()) {
        out["content"] = m.value("content", "");
        return out;
    }
    json parts = json::array();
    for (const auto& p : m["content"]) {
        const std::string t = p.value("type", "text");
        if (t == "image_url") {
            std::string url;
            if (p.contains("image_url") && p["image_url"].is_object()) {
                url = p["image_url"].value("url", "");
            }
            parts.push_back(json{
                {"type", "image"},
                {"path", process_image_url(url, tempfiles).string()},
            });
        } else {
            parts.push_back(json{
                {"type", "text"}, {"text", p.value("text", "")}});
        }
    }
    out["content"] = parts;
    return out;
}

/**
 * @brief Build canonical messages-JSON from an OpenAI request (gh#38).
 *
 * Walks the input messages array, rewrites each via
 * convert_openai_message, and returns the resulting JSON-array string
 * suitable for entropic_run_messages.
 *
 * @param messages OpenAI messages array.
 * @param tempfiles Per-request tempfile guard.
 * @return Serialized JSON array string.
 * @utility
 * @version 2.1.8
 */
std::string build_messages_json(
    const json& messages, RequestTempfiles& tempfiles) {
    json arr = json::array();
    for (const auto& m : messages) {
        arr.push_back(convert_openai_message(m, tempfiles));
    }
    return arr.dump();
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
 * @brief HTTP-error sentinel typed by entropic error code (gh#38, v2.1.8).
 *
 * Carries an HTTP status code so route handlers can map vision-tier
 * failures to 400 and other failures to 500 from one catch site.
 *
 * @internal
 * @version 2.1.8
 */
struct HttpStatusError : public std::runtime_error {
    int status;
    /**
     * @brief Construct a typed HTTP-status error.
     * @param s HTTP status code (400, 500, …).
     * @param m Diagnostic message bubbled to the OpenAI error body.
     * @utility
     * @version 2.1.8
     */
    HttpStatusError(int s, const std::string& m)
        : std::runtime_error(m), status(s) {}
};

/**
 * @brief Run the engine via entropic_run_messages (gh#38, v2.1.8).
 *
 * Multimodal-aware sibling of run_blocking. Maps the engine's
 * NO_VISION_TIER to HTTP 400; other engine errors to 500.
 *
 * @param handle Engine handle.
 * @param messages_json Canonical engine messages JSON.
 * @return Assistant text from the last completed turn.
 * @throws HttpStatusError with appropriate status on engine failure.
 * @utility
 * @version 2.1.8
 */
std::string run_blocking_messages(
    entropic_handle_t handle, const std::string& messages_json) {
    char* result = nullptr;
    auto err = entropic_run_messages(
        handle, messages_json.c_str(), &result);
    if (err != ENTROPIC_OK) {
        if (result != nullptr) { entropic_free(result); }
        if (err == ENTROPIC_ERROR_NO_VISION_TIER) {
            throw HttpStatusError(400,
                "image content provided but no vision-capable tier "
                "is configured. Configure a tier with capabilities: "
                "[text, vision] and a paired mmproj.");
        }
        throw HttpStatusError(500,
            "entropic_run_messages failed: code=" + std::to_string(err));
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
 * @version 2.1.8
 */
void stream_completion(entropic_handle_t handle,
                       const std::string& model,
                       const std::string& messages_json,
                       httplib::DataSink& sink) {
    StreamContext ctx{&sink, model};
    auto err = entropic_run_messages_streaming(
        handle, messages_json.c_str(), stream_on_token, &ctx, nullptr);
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
 * @version 2.1.8
 */
std::string complete_blocking(
    entropic_handle_t handle, const json& body, const std::string& model) {
    (void)model;
    RequestTempfiles tempfiles;
    const auto msgs_json = build_messages_json(
        body.value("messages", json::array()), tempfiles);
    return run_blocking_messages(handle, msgs_json);
}

/**
 * @brief Handle POST /v1/chat/completions in non-streaming mode.
 * @utility
 * @version 2.1.8
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
 * @version 2.1.8
 */
void handle_chat_streaming(entropic_handle_t handle,
                           const json& body,
                           httplib::Response& res) {
    const auto model = body.value("model", "primary");
    // Tempfile guard lives only inside the chunked-content provider —
    // by the time the engine finishes streaming, all decoded image
    // bytes have been read by ImagePreprocessor and the tempfiles can
    // be removed. The shared_ptr ensures the guard outlives the
    // lambda's captured copy through the stream lifecycle.
    auto tempfiles = std::make_shared<RequestTempfiles>();
    const auto messages_json = build_messages_json(
        body.value("messages", json::array()), *tempfiles);
    res.set_chunked_content_provider(
        "text/event-stream",
        [handle, model, messages_json, tempfiles]
        (size_t /*offset*/, httplib::DataSink& sink) {
            stream_completion(handle, model, messages_json, sink);
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
 * @version 2.1.8
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
    } catch (const HttpStatusError& e) {
        // gh#38: typed status passthrough — vision-tier missing → 400,
        // other engine failures → 500. send_error() picks 500 by
        // default, so handle non-500 statuses inline here.
        if (e.status == 400) {
            json b = {{"error", json{
                {"message", e.what()},
                {"type", "invalid_request_error"}}}};
            res.status = 400;
            res.set_content(b.dump(), "application/json");
        } else {
            send_error(res, e.what());
        }
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
