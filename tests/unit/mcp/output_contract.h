// SPDX-License-Identifier: Apache-2.0
/**
 * @file output_contract.h
 * @brief Strict check of a tool's emitted result against its outputSchema
 *        (gh#203, v2.14.0).
 *
 * Stricter than JSON Schema's defaults ON PURPOSE — "no undocumented
 * behaviour" (decision 4 of the v2.14.0 plan):
 *   - an object is CLOSED unless the schema says `additionalProperties`;
 *     an emitted key the schema does not declare is a violation;
 *   - `oneOf` must match exactly one branch;
 *   - `enum` (error codes) must contain the value.
 *
 * Supports the subset the bundled schemas use: type (name or array),
 * properties, required, additionalProperties (false / schema / true),
 * items, enum, oneOf, minimum.
 *
 * @version 2.14.0
 */
#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

namespace entropic::test {

/**
 * @brief Whether `value` has JSON type `type`.
 * @utility
 * @version 2.14.0
 */
inline bool json_is_type(const nlohmann::json& value, const std::string& type) {
    if (type == "object") { return value.is_object(); }
    if (type == "array") { return value.is_array(); }
    if (type == "string") { return value.is_string(); }
    if (type == "integer") { return value.is_number_integer(); }
    if (type == "number") { return value.is_number(); }
    if (type == "boolean") { return value.is_boolean(); }
    if (type == "null") { return value.is_null(); }
    return false;
}

inline std::string contract_violation(const nlohmann::json& schema,
                                      const nlohmann::json& value,
                                      const std::string& where = "$");

/**
 * @brief Object rule: required keys present, every key declared, each valid.
 * @utility
 * @version 2.14.0
 */
inline std::string object_violation(const nlohmann::json& schema,
                                    const nlohmann::json& value,
                                    const std::string& where) {
    const auto props = schema.value("properties", nlohmann::json::object());
    for (const auto& req : schema.value("required", nlohmann::json::array())) {
        if (!value.contains(req.get<std::string>())) {
            return where + ": missing required key '" + req.get<std::string>() + "'";
        }
    }
    const auto extra = schema.value("additionalProperties", nlohmann::json(false));
    for (const auto& [key, v] : value.items()) {
        std::string err;
        if (props.contains(key)) {
            err = contract_violation(props[key], v, where + "." + key);
        } else if (extra.is_object()) {
            err = contract_violation(extra, v, where + "." + key);
        } else if (extra != true) {
            err = where + ": undeclared key '" + key + "'";
        }
        if (!err.empty()) { return err; }
    }
    return "";
}

/**
 * @brief Validate `value` against `schema`; "" when it conforms.
 * @param schema Schema node.
 * @param value Emitted value.
 * @param where JSON path, for the message.
 * @return The first violation, or "".
 * @utility
 * @version 2.14.0
 */
inline std::string contract_violation(const nlohmann::json& schema,
                                      const nlohmann::json& value,
                                      const std::string& where) {
    if (schema.contains("oneOf")) {
        int matched = 0;
        std::string last;
        for (const auto& branch : schema["oneOf"]) {
            last = contract_violation(branch, value, where);
            matched += last.empty() ? 1 : 0;
        }
        return matched == 1 ? "" : where + ": matches " + std::to_string(matched)
            + " oneOf branches (last mismatch: " + last + ")";
    }
    if (schema.contains("enum")) {
        const auto& e = schema["enum"];
        if (std::find(e.begin(), e.end(), value) == e.end()) {
            return where + ": " + value.dump() + " not in enum " + e.dump();
        }
    }
    if (schema.contains("type")) {
        const auto& t = schema["type"];
        bool ok = false;
        if (t.is_array()) {
            for (const auto& one : t) { ok = ok || json_is_type(value, one.get<std::string>()); }
        } else {
            ok = json_is_type(value, t.get<std::string>());
        }
        if (!ok) { return where + ": expected " + t.dump() + ", got " + value.dump(); }
    }
    if (schema.contains("minimum") && value.is_number()
        && value.get<double>() < schema["minimum"].get<double>()) {
        return where + ": below minimum";
    }
    if (value.is_object() && schema.value("type", "") == "object") {
        return object_violation(schema, value, where);
    }
    if (value.is_array() && schema.contains("items")) {
        for (size_t i = 0; i < value.size(); ++i) {
            auto err = contract_violation(schema["items"], value[i],
                                          where + "[" + std::to_string(i) + "]");
            if (!err.empty()) { return err; }
        }
    }
    return "";
}

}  // namespace entropic::test
