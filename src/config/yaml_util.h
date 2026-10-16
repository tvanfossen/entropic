// SPDX-License-Identifier: Apache-2.0
/**
 * @file yaml_util.h
 * @brief ryml extraction helpers for config parsing.
 *
 * Wraps ryml's tree API with type-safe extraction functions that
 * provide clear error messages for type mismatches. Each function
 * returns true if the key was found and extracted, false if absent.
 * The output parameter is unchanged when the key is absent.
 *
 * @dg_internal Implementation detail of librentropic-config. Not a public header.
 * @version 1.8.1
 */

#pragma once

#include <entropic/entropic_export.h>
#include <ryml.hpp>
#include <c4/std/string.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace entropic::config {

/**
 * @brief Extract a string value from a YAML node.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output string. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 */
bool extract(ryml::ConstNodeRef node, c4::csubstr key, std::string& out);

/**
 * @brief Extract an int value from a YAML node.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output int. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 */
bool extract(ryml::ConstNodeRef node, c4::csubstr key, int& out);

/**
 * @brief Extract a float value from a YAML node.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output float. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 */
bool extract(ryml::ConstNodeRef node, c4::csubstr key, float& out);

/**
 * @brief Extract a double value from a YAML node.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output double. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 */
bool extract(ryml::ConstNodeRef node, c4::csubstr key, double& out);

/**
 * @brief Extract a bool value from a YAML node.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output bool. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 */
bool extract(ryml::ConstNodeRef node, c4::csubstr key, bool& out);

/**
 * @brief Extract a filesystem path with ~ expansion.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output path with ~ expanded. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 */
bool extract_path(ryml::ConstNodeRef node, c4::csubstr key,
                  std::filesystem::path& out);

/**
 * @brief Extract a tri-state path (null = default, false = disabled, string = path).
 *
 * Maps to Python's TriStatePath: None | False | Path.
 * YAML values: absent/null = default (out=nullopt, disabled=false),
 *              false = disabled (out=nullopt, disabled=true),
 *              string = custom path (out=expanded path, disabled=false).
 *
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output path. nullopt if absent/null/false.
 * @param[out] disabled Set to true if value is explicitly false.
 * @return true if key was found.
 * @version 1.8.1
 */
bool extract_tri_state_path(
    ryml::ConstNodeRef node, c4::csubstr key,
    std::optional<std::filesystem::path>& out, bool& disabled);

/**
 * @brief Extract inline text from an object-form value (gh#141).
 *
 * Recognises `key: { content: "..." }` and yields the content. Any other shape
 * — a scalar, a sequence, or a map without `content` — is left alone and
 * reported as not-found, so the caller can fall through to its existing parse
 * and every pre-gh#141 spelling keeps its meaning.
 *
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Receives the content. Untouched unless the object form matched.
 * @return true only if the object form matched and content was extracted.
 * @req REQ-TYPE-005
 * @version 2.11.0
 */
bool extract_inline_content(
    ryml::ConstNodeRef node, c4::csubstr key, std::optional<std::string>& out);

/**
 * @brief Extract a vector of strings from a YAML sequence node.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output vector. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 */
bool extract_string_list(ryml::ConstNodeRef node, c4::csubstr key,
                         std::vector<std::string>& out);

/**
 * @brief Extract an optional vector of strings.
 *
 * If key absent: out unchanged (remains nullopt).
 * If key present with sequence: out = vector of strings.
 * If key present with null: out = nullopt.
 *
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output optional vector.
 * @return true if key was found.
 * @version 1.8.1
 */
bool extract_string_list_opt(ryml::ConstNodeRef node, c4::csubstr key,
                             std::optional<std::vector<std::string>>& out);

/**
 * @brief Extract a map of string to string from a YAML mapping node.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output map. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 */
bool extract_string_map(ryml::ConstNodeRef node, c4::csubstr key,
                        std::unordered_map<std::string, std::string>& out);

/**
 * @brief Extract a map of string to list of strings from a YAML mapping.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output map. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 */
bool extract_string_list_map(
    ryml::ConstNodeRef node, c4::csubstr key,
    std::unordered_map<std::string, std::vector<std::string>>& out);

/**
 * @brief Expand ~ to home directory in a path.
 * @param p Input path (may start with ~).
 * @return Expanded path. If p doesn't start with ~, returns p unchanged.
 * @version 1.8.1
 */
std::filesystem::path expand_home(const std::filesystem::path& p);

/**
 * @brief Parse YAML text into `tree`, returning an error instead of aborting.
 *
 * gh#211 (v2.14.4): every parse in the engine used ryml's default error
 * handler, which aborts the process — one typo in a config file or an
 * identity's frontmatter took the host down. This installs (once, process-
 * wide) an error callback that throws, and converts the throw to a message.
 *
 * @param name Source name for messages (file path, "<json>").
 * @param text YAML text.
 * @param[out] tree Parsed tree (meaningful only on success).
 * @return "" on success, else "<name>: invalid YAML: <reason>".
 * @utility
 * @version 2.14.4
 */
ENTROPIC_EXPORT std::string parse_yaml(const std::string& name,
                                       const std::string& text, ryml::Tree& tree);

/**
 * @brief Convert ryml csubstr to std::string.
 * @param s ryml substring.
 * @return Equivalent std::string.
 * @version 1.8.1
 */
std::string to_string(c4::csubstr s);

/**
 * @brief Read a file into a string.
 * @param path File path.
 * @return File contents, or empty string on failure.
 * @version 1.8.1
 */
std::string read_file(const std::filesystem::path& path);

} // namespace entropic::config
