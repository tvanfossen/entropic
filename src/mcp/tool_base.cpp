// SPDX-License-Identifier: Apache-2.0
/**
 * @file tool_base.cpp
 * @brief ToolBase implementation, load_tool_definition, and the
 *        response-format renderer (gh#203).
 * @version 2.14.0
 */

#include <entropic/mcp/tool_base.h>
#include <entropic/mcp/server_base.h>
#include <entropic/types/logging.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

static auto logger = entropic::log::get("mcp.tool_base");

namespace entropic {

/**
 * @brief Construct with a pre-built definition.
 * @param def Tool definition.
 * @dg_internal
 * @version 1.8.5
 */
ToolBase::ToolBase(ToolDefinition def)
    : definition_(std::move(def)) {}

/**
 * @brief Get the tool name.
 * @return Tool name from definition.
 * @dg_internal
 * @version 1.8.5
 */
const std::string& ToolBase::name() const {
    return definition_.name;
}

/**
 * @brief Get the full tool definition.
 * @return Tool definition reference.
 * @dg_internal
 * @version 1.8.5
 */
const ToolDefinition& ToolBase::definition() const {
    return definition_;
}

/**
 * @brief Default anchor_key — no anchoring.
 *
 * MCPServerBase injects a context_anchor directive only for a non-empty
 * key, so the default leaves the envelope's directives array empty.
 *
 * @param args_json Tool call arguments (unused in the default).
 * @return Empty string — no ContextAnchor is injected for this tool.
 * @req REQ-MCP-001
 * @version 1.8.5
 */
std::string ToolBase::anchor_key(
    const std::string& /*args_json*/) const {
    return "";
}

/**
 * @brief Default required access level — WRITE (safe default).
 *
 * A newly added tool is treated as privileged until someone deliberately
 * relaxes it; read-only tools override this to READ.
 *
 * @return MCPAccessLevel::WRITE for every tool that does not override.
 * @req REQ-MCP-011
 * @version 1.9.4
 */
MCPAccessLevel ToolBase::required_access_level() const {
    return MCPAccessLevel::WRITE;
}

namespace {

using ojson = nlohmann::ordered_json;

std::string render_shape(const ojson& schema);

/**
 * @brief Render a `type` keyword (a name, or an array of names).
 * @param type The keyword's value.
 * @return e.g. "integer" or "string|null".
 * @dg_internal
 * @version 2.14.0
 */
std::string render_type(const ojson& type) {
    if (!type.is_array()) { return type.get<std::string>(); }
    std::string out;
    for (const auto& t : type) {
        out += (out.empty() ? "" : "|") + t.get<std::string>();
    }
    return out;
}

/**
 * @brief Render alternatives (`enum` values or `oneOf` branches).
 * @param alts Array of alternatives.
 * @param literal true for enum values, false for sub-schemas.
 * @return Alternatives joined with `|`.
 * @dg_internal
 * @version 2.14.0
 */
std::string render_alternatives(const ojson& alts, bool literal) {
    std::string out;
    for (const auto& a : alts) {
        out += (out.empty() ? "" : " | ")
            + (literal ? a.dump() : render_shape(a));
    }
    return out;
}

/**
 * @brief Render an object schema as `{"key": shape, "opt"?: shape}`.
 * @param schema Object schema.
 * @return Rendered object.
 * @dg_internal
 * @version 2.14.0
 */
std::string render_object(const ojson& schema) {
    const auto required = schema.value("required", ojson::array());
    const auto props = schema.value("properties", ojson::object());
    std::string out = "{";
    for (const auto& [key, prop] : props.items()) {
        bool req = std::find(required.begin(), required.end(), key) != required.end();
        out += (out.size() > 1 ? ", " : "") + ojson(key).dump()
            + (req ? "" : "?") + ": " + render_shape(prop);
    }
    const auto extra = schema.value("additionalProperties", ojson(false));
    if (extra.is_object()) {
        out += (out.size() > 1 ? ", " : "") + std::string("<string>: ")
            + render_shape(extra);
    }
    return out + "}";
}

/**
 * @brief Render a schema node by its `type` (object, array, or a name).
 * @param schema Schema node without enum/oneOf.
 * @return Rendered shape; "any" when no type is declared.
 * @dg_internal
 * @version 2.14.0
 */
std::string render_typed(const ojson& schema) {
    const auto type = schema.value("type", ojson());
    std::string out = "any";
    if (type == "object") {
        out = render_object(schema);
    } else if (type == "array") {
        out = "[" + render_shape(schema.value("items", ojson::object())) + ", ...]";
    } else if (!type.is_null()) {
        out = render_type(type);
    }
    return out;
}

/**
 * @brief Render any supported schema node compactly.
 * @param schema Schema node.
 * @return Rendered shape.
 * @dg_internal
 * @version 2.14.0
 */
std::string render_shape(const ojson& schema) {
    std::string out;
    if (schema.contains("enum")) {
        out = render_alternatives(schema["enum"], true);
    } else if (schema.contains("oneOf")) {
        out = render_alternatives(schema["oneOf"], false);
    } else {
        out = render_typed(schema);
    }
    return out;
}

/**
 * @brief Collect "`key`: description" notes, depth first.
 * @param schema Schema node.
 * @param[out] notes Accumulated notes.
 * @dg_internal
 * @version 2.14.0
 */
void collect_notes(const ojson& schema, std::vector<std::string>& notes) {
    const auto props = schema.value("properties", ojson::object());
    for (const auto& [key, prop] : props.items()) {
        if (prop.contains("description")) {
            notes.push_back("- `" + key + "`: " + prop["description"].get<std::string>());
        }
        collect_notes(prop, notes);
    }
    if (schema.contains("items")) { collect_notes(schema["items"], notes); }
    const auto branches = schema.value("oneOf", ojson::array());
    for (const auto& branch : branches) {
        collect_notes(branch, notes);
    }
}

/**
 * @brief Render the variant lines: one per top-level oneOf branch.
 * @param schema Top-level output schema.
 * @return Lines, each "- <title>: <shape>".
 * @dg_internal
 * @version 2.14.0
 */
std::string render_variants(const ojson& schema) {
    std::string out;
    if (!schema.contains("oneOf")) {
        return render_shape(schema) + "\n";
    }
    int n = 0;
    for (const auto& branch : schema["oneOf"]) {
        ++n;
        out += "- " + branch.value("title", "variant " + std::to_string(n))
            + ": " + render_shape(branch) + "\n";
    }
    return out;
}

}  // namespace

/**
 * @brief Render an outputSchema as the "Response format" text (gh#203).
 *
 * Generated rather than hand-written so the prose a model reads cannot
 * drift from the schema the contract tests hold handlers to — the defect
 * class gh#120 → gh#201 showed.
 *
 * @param output_schema_json JSON Schema (raw string).
 * @return The rendered section, or "" for an empty/unparseable schema.
 * @utility
 * @req REQ-MCP-013
 * @version 2.14.0
 */
std::string render_response_format(const std::string& output_schema_json) {
    auto schema = ojson::parse(output_schema_json, nullptr, false);
    std::string out;
    if (schema.is_object()) {
        out = "\n\n**Response format** (JSON):\n" + render_variants(schema);
        std::vector<std::string> notes;
        collect_notes(schema, notes);
        for (const auto& note : notes) { out += note + "\n"; }
        // Engine-wide, not per tool: a call refused before or thrown out of
        // the handler (bad arguments, a refused path, an I/O fault) is
        // reported by the server base as plain text (server_base.cpp).
        out += "Other failures: plain text `Error: ...`.\n";
    }
    return out;
}

/**
 * @brief A declared tool error: `{"error": code, "message": message}`.
 * @param code Error code (declared in the tool's outputSchema).
 * @param message Human-readable message.
 * @return Serialized JSON object.
 * @utility
 * @req REQ-MCP-015
 * @version 2.14.0
 */
std::string tool_error_json(const std::string& code, const std::string& message) {
    nlohmann::json err;
    err["error"] = code;
    err["message"] = message;
    return err.dump();
}

/**
 * @brief Load a tool definition from a JSON file.
 *
 * Reads the bundled `data/tools/<server>/<tool>.json` descriptor, whose
 * `inputSchema` (camelCase) is the schema ToolExecutor later validates
 * arguments against.
 *
 * @param tool_name Tool name (e.g., "read_file").
 * @param server_prefix Server directory name (e.g., "filesystem"); an
 *                      empty prefix reads directly from data_dir.
 * @param data_dir Base directory for tool JSON files.
 * @return Parsed ToolDefinition carrying name, description and the
 *         serialised inputSchema; when the descriptor declares an
 *         `outputSchema` (v2.14.0, gh#203) it is kept, and its rendered
 *         "Response format" section is appended to the description.
 * @throws std::runtime_error when the descriptor file cannot be opened;
 *         nlohmann parse errors propagate for malformed JSON or a
 *         missing `name`/`inputSchema` key.
 * @req REQ-MCP-013
 * @version 2.14.0
 */
ToolDefinition load_tool_definition(
    const std::string& tool_name,
    const std::string& server_prefix,
    const std::string& data_dir) {

    std::string path = data_dir;
    if (!server_prefix.empty()) {
        path += "/" + server_prefix;
    }
    path += "/" + tool_name + ".json";

    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error(
            "Tool definition not found: " + path);
    }

    // ordered: the rendered Response format keeps the authored key order.
    auto json = nlohmann::ordered_json::parse(file);

    ToolDefinition def;
    def.name = json.at("name").get<std::string>();
    def.description = json.value("description", "");
    def.input_schema = json.at("inputSchema").dump();
    if (json.contains("outputSchema")) {
        def.output_schema = json["outputSchema"].dump();
        def.description += render_response_format(def.output_schema);
    }

    logger->info("Loaded tool definition: {} from {}",
                 def.name, path);
    return def;
}

} // namespace entropic
