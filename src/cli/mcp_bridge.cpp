/**
 * @file mcp_bridge.cpp
 * @brief `entropic mcp-bridge` subcommand — exposes engine via MCP/stdio.
 *
 * Speaks JSON-RPC 2.0 over stdin/stdout following the Model Context
 * Protocol (MCP) so that external clients (Claude Code, VSCode, etc.)
 * can use the entropic engine as a tool provider with zero consumer
 * code. The user adds entropic to their .mcp.json with command
 * "entropic" and args ["mcp-bridge"]; the engine then appears as MCP
 * tools.
 *
 * Lifecycle:
 *   1. Parse --project-dir flag (default: cwd)
 *   2. Create EntropicEngine handle
 *   3. configure_dir(project_dir) — applies layered config and discovers .mcp.json
 *   4. Read JSON-RPC requests from stdin; dispatch to handlers
 *   5. On EOF or "shutdown" method, destroy engine and exit
 *
 * Exposed tools:
 *   entropic.ask              Single-turn run, returns full text
 *   entropic.status           Engine version + message count
 *   entropic.context_clear    Reset conversation
 *   entropic.context_get      Return conversation as JSON
 *   entropic.context_count    Return message count
 *
 * The engine is created once at startup and persisted across requests
 * within a bridge session, so multi-turn conversations work naturally.
 *
 * @version 2.0.3
 */

#include <entropic/entropic.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

using json = nlohmann::json;

namespace entropic::cli {

namespace {

/**
 * @brief Bridge state — engine handle + accumulator for streamed output.
 * @internal
 * @version 2.0.3
 */
struct BridgeState {
    entropic_handle_t handle = nullptr; ///< Engine handle
    std::string accumulator;            ///< Streaming token buffer
};

/**
 * @brief Streaming token callback — accumulates into the bridge state.
 * @callback
 * @version 2.0.3
 */
void on_token(const char* token, size_t len, void* user_data)
{
    auto* state = static_cast<BridgeState*>(user_data);
    state->accumulator.append(token, len);
}

/**
 * @brief Tool definitions exposed by the bridge.
 * @utility
 * @version 2.0.3
 */
json tool_definitions()
{
    return json::array({
        {{"name", "entropic.ask"},
         {"description",
          "Run a single agentic turn against the entropic engine. "
          "Returns the assistant's complete response. Conversation "
          "context is retained across calls within this bridge session."},
         {"inputSchema", {
            {"type", "object"},
            {"properties", {{"prompt", {
                {"type", "string"},
                {"description", "User message"}
            }}}},
            {"required", json::array({"prompt"})}
         }}},
        {{"name", "entropic.status"},
         {"description", "Get engine status: version and message count."},
         {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}},
        {{"name", "entropic.context_clear"},
         {"description", "Clear conversation history (start a new session)."},
         {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}},
        {{"name", "entropic.context_get"},
         {"description", "Return the current conversation as a JSON message array."},
         {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}},
        {{"name", "entropic.context_count"},
         {"description", "Return the number of messages in the conversation."},
         {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}},
    });
}

/**
 * @brief Build a JSON-RPC success response.
 * @utility
 * @version 2.0.3
 */
json rpc_ok(const json& id, const json& result)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

/**
 * @brief Build a JSON-RPC error response.
 * @utility
 * @version 2.0.3
 */
json rpc_err(const json& id, int code, const std::string& msg)
{
    return {{"jsonrpc", "2.0"}, {"id", id},
            {"error", {{"code", code}, {"message", msg}}}};
}

/**
 * @brief Wrap a text result in MCP tools/call response shape.
 * @utility
 * @version 2.0.3
 */
json tool_text_result(const std::string& text)
{
    return {{"content", json::array({
        {{"type", "text"}, {"text", text}}
    })}};
}

/**
 * @brief Handle the `entropic.ask` tool.
 *
 * Runs entropic_run_streaming, accumulates tokens into the bridge
 * state's buffer, then returns the accumulated text.
 *
 * @param state Bridge state (handle + accumulator).
 * @param args Tool arguments (must contain "prompt").
 * @return MCP tool result JSON.
 * @internal
 * @version 2.0.3
 */
json handle_ask(BridgeState& state, const json& args)
{
    auto prompt_it = args.find("prompt");
    if (prompt_it == args.end() || !prompt_it->is_string()) {
        return tool_text_result("error: missing 'prompt' argument");
    }
    std::string prompt = prompt_it->get<std::string>();
    state.accumulator.clear();
    auto err = entropic_run_streaming(
        state.handle, prompt.c_str(), on_token, &state, nullptr);
    if (err != ENTROPIC_OK) {
        const char* msg = entropic_last_error(state.handle);
        return tool_text_result(
            std::string("error: ") + (msg ? msg : "unknown"));
    }
    return tool_text_result(state.accumulator);
}

/**
 * @brief Handle the `entropic.status` tool.
 * @internal
 * @version 2.0.3
 */
json handle_status(BridgeState& state)
{
    size_t count = 0;
    entropic_context_count(state.handle, &count);
    std::ostringstream os;
    os << "entropic engine ready\n"
       << "messages in conversation: " << count;
    return tool_text_result(os.str());
}

/**
 * @brief Handle the `entropic.context_clear` tool.
 * @internal
 * @version 2.0.3
 */
json handle_clear(BridgeState& state)
{
    auto err = entropic_context_clear(state.handle);
    if (err != ENTROPIC_OK) {
        return tool_text_result("error: clear failed");
    }
    return tool_text_result("conversation cleared");
}

/**
 * @brief Handle the `entropic.context_get` tool.
 * @internal
 * @version 2.0.3
 */
json handle_context_get(BridgeState& state)
{
    char* messages = nullptr;
    auto err = entropic_context_get(state.handle, &messages);
    if (err != ENTROPIC_OK || !messages) {
        return tool_text_result("error: context_get failed");
    }
    std::string result(messages);
    entropic_free(messages);
    return tool_text_result(result);
}

/**
 * @brief Handle the `entropic.context_count` tool.
 * @internal
 * @version 2.0.3
 */
json handle_context_count(BridgeState& state)
{
    size_t count = 0;
    entropic_context_count(state.handle, &count);
    return tool_text_result(std::to_string(count));
}

/**
 * @brief Dispatch a tools/call request to the appropriate handler.
 * @internal
 * @version 2.0.3
 */
/**
 * @brief Dispatch a tools/call to a no-argument handler.
 * @internal
 * @version 2.0.3
 */
json dispatch_no_args(BridgeState& state, const std::string& name)
{
    json result = tool_text_result("error: unknown tool '" + name + "'");
    if (name == "entropic.status") { result = handle_status(state); }
    if (name == "entropic.context_clear") { result = handle_clear(state); }
    if (name == "entropic.context_get") { result = handle_context_get(state); }
    if (name == "entropic.context_count") { result = handle_context_count(state); }
    return result;
}

/**
 * @brief Dispatch a tools/call request to the appropriate handler.
 * @internal
 * @version 2.0.3
 */
json dispatch_tool(BridgeState& state, const json& params)
{
    std::string name = params.value("name", std::string{});
    json args = params.value("arguments", json::object());
    json result = (name == "entropic.ask")
        ? handle_ask(state, args)
        : dispatch_no_args(state, name);
    return result;
}

/**
 * @brief Dispatch a single JSON-RPC request and write the response.
 * @internal
 * @version 2.0.3
 */
void handle_request(BridgeState& state, const json& req)
{
    json id = req.value("id", json(nullptr));
    std::string method = req.value("method", std::string{});
    json params = req.value("params", json::object());
    json response;

    if (method == "initialize") {
        response = rpc_ok(id, {
            {"protocolVersion", "2024-11-05"},
            {"serverInfo", {{"name", "entropic"}, {"version", "2.0.3"}}},
            {"capabilities", {{"tools", json::object()}}}
        });
    } else if (method == "tools/list") {
        response = rpc_ok(id, {{"tools", tool_definitions()}});
    } else if (method == "tools/call") {
        response = rpc_ok(id, dispatch_tool(state, params));
    } else if (method == "shutdown" || method == "exit") {
        response = rpc_ok(id, json::object());
    } else {
        response = rpc_err(id, -32601, "Unknown method: " + method);
    }

    std::cout << response.dump() << '\n';
    std::cout.flush();
}

/**
 * @brief Parse --project-dir from argv.
 * @utility
 * @version 2.0.3
 */
std::string parse_project_dir(int argc, char* argv[])
{
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], "--project-dir") == 0) {
            return argv[i + 1];
        }
    }
    return {};
}

} // anonymous namespace

/**
 * @brief Run the MCP bridge — main stdio loop.
 *
 * Creates an engine, configures it from the project directory (which
 * triggers .mcp.json discovery for any external servers), then reads
 * JSON-RPC requests one per line until EOF.
 *
 * @param argc Argument count (after the "mcp-bridge" subcommand name).
 * @param argv Argument vector. argv[0] is "mcp-bridge".
 * @return 0 on clean exit, 1 on initialization failure.
 *
 * @internal
 * @version 2.0.3
 */
int run_mcp_bridge(int argc, char* argv[])
{
    // CRITICAL: stdout is reserved for JSON-RPC. Silence spdlog's default
    // stdout sink before any engine call (logs still flow to session log
    // file when project_dir is set).
    spdlog::set_level(spdlog::level::off);

    BridgeState state;
    if (entropic_create(&state.handle) != ENTROPIC_OK) {
        std::fprintf(stderr, "entropic mcp-bridge: create failed\n");
        return 1;
    }
    std::string project_dir = parse_project_dir(argc, argv);
    auto err = entropic_configure_dir(
        state.handle,
        project_dir.empty() ? nullptr : project_dir.c_str());
    if (err != ENTROPIC_OK) {
        std::fprintf(stderr, "entropic mcp-bridge: configure failed: %s\n",
                     entropic_last_error(state.handle));
        entropic_destroy(state.handle);
        return 1;
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) { continue; }
        auto req = json::parse(line, nullptr, false);
        if (req.is_discarded()) { continue; }
        handle_request(state, req);
    }

    entropic_destroy(state.handle);
    return 0;
}

} // namespace entropic::cli
