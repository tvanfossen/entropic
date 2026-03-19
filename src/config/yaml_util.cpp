/**
 * @file yaml_util.cpp
 * @brief ryml extraction helper implementations.
 * @version 1.8.1
 * @utility
 */

#include "yaml_util.h"

#include <entropic/types/logging.h>
#include <cstdlib>
#include <fstream>
#include <sstream>

static auto s_log = entropic::log::get("config");

namespace entropic::config {

/**
 * @brief Convert ryml csubstr to std::string.
 * @param s ryml substring.
 * @return Equivalent std::string.
 * @version 1.8.1
 * @utility
 */
std::string to_string(c4::csubstr s)
{
    return std::string(s.str, s.len);
}

/**
 * @brief Read a file into a string.
 * @param path File path.
 * @return File contents, or empty string on failure.
 * @version 1.8.1
 * @utility
 */
std::string read_file(const std::filesystem::path& path)
{
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        s_log->error("Cannot open file: {}", path.string());
        return "";
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

/**
 * @brief Expand ~ to home directory in a path.
 * @param p Input path (may start with ~).
 * @return Expanded path. If p doesn't start with ~, returns p unchanged.
 * @version 1.8.1
 * @utility
 */
std::filesystem::path expand_home(const std::filesystem::path& p)
{
    auto str = p.string();
    if (str.empty() || str[0] != '~') {
        return p;
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return p;
    }
    if (str.size() == 1) {
        return std::filesystem::path(home);
    }
    // Skip "~/" prefix
    return std::filesystem::path(home) / str.substr(2);
}

/**
 * @brief Extract a string value from a YAML node.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output string. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 * @utility
 */
bool extract(ryml::ConstNodeRef node, c4::csubstr key, std::string& out)
{
    if (!node.is_map() || !node.has_child(key)) {
        return false;
    }
    auto child = node[key];
    if (!child.has_val() || child.val_is_null()) {
        return false;
    }
    out = to_string(child.val());
    return true;
}

/**
 * @brief Extract an int value from a YAML node.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output int. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 * @utility
 */
bool extract(ryml::ConstNodeRef node, c4::csubstr key, int& out)
{
    if (!node.is_map() || !node.has_child(key)) {
        return false;
    }
    auto child = node[key];
    if (!child.has_val() || child.val_is_null()) {
        return false;
    }
    auto val = to_string(child.val());
    out = std::stoi(val);
    return true;
}

/**
 * @brief Extract a float value from a YAML node.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output float. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 * @utility
 */
bool extract(ryml::ConstNodeRef node, c4::csubstr key, float& out)
{
    if (!node.is_map() || !node.has_child(key)) {
        return false;
    }
    auto child = node[key];
    if (!child.has_val() || child.val_is_null()) {
        return false;
    }
    auto val = to_string(child.val());
    out = std::stof(val);
    return true;
}

/**
 * @brief Extract a double value from a YAML node.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output double. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 * @utility
 */
bool extract(ryml::ConstNodeRef node, c4::csubstr key, double& out)
{
    if (!node.is_map() || !node.has_child(key)) {
        return false;
    }
    auto child = node[key];
    if (!child.has_val() || child.val_is_null()) {
        return false;
    }
    auto val = to_string(child.val());
    out = std::stod(val);
    return true;
}

/**
 * @brief Extract a bool value from a YAML node.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output bool. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 * @utility
 */
bool extract(ryml::ConstNodeRef node, c4::csubstr key, bool& out)
{
    if (!node.is_map() || !node.has_child(key)) {
        return false;
    }
    auto child = node[key];
    if (!child.has_val() || child.val_is_null()) {
        return false;
    }
    auto val = to_string(child.val());
    out = (val == "true" || val == "True" || val == "TRUE"
           || val == "yes" || val == "Yes" || val == "YES"
           || val == "on" || val == "On" || val == "ON"
           || val == "1");
    return true;
}

/**
 * @brief Extract a filesystem path with ~ expansion.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output path with ~ expanded. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 * @utility
 */
bool extract_path(ryml::ConstNodeRef node, c4::csubstr key,
                  std::filesystem::path& out)
{
    std::string val;
    if (!extract(node, key, val)) {
        return false;
    }
    out = expand_home(std::filesystem::path(val));
    return true;
}

/**
 * @brief Extract a tri-state path (null = default, false = disabled, string = path).
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output path. nullopt if absent/null/false.
 * @param[out] disabled Set to true if value is explicitly false.
 * @return true if key was found.
 * @version 1.8.1
 * @utility
 */
bool extract_tri_state_path(
    ryml::ConstNodeRef node, c4::csubstr key,
    std::optional<std::filesystem::path>& out, bool& disabled)
{
    if (!node.is_map() || !node.has_child(key)) {
        return false;
    }
    auto child = node[key];
    if (!child.has_val() || child.val_is_null()) {
        out = std::nullopt;
        disabled = false;
        return true;
    }
    auto val = to_string(child.val());
    if (val == "false" || val == "False" || val == "FALSE") {
        out = std::nullopt;
        disabled = true;
        return true;
    }
    out = expand_home(std::filesystem::path(val));
    disabled = false;
    return true;
}

/**
 * @brief Extract a vector of strings from a YAML sequence node.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output vector. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 * @utility
 */
bool extract_string_list(ryml::ConstNodeRef node, c4::csubstr key,
                         std::vector<std::string>& out)
{
    if (!node.is_map() || !node.has_child(key)) {
        return false;
    }
    auto child = node[key];
    if (!child.is_seq()) {
        return false;
    }
    out.clear();
    for (auto item : child) {
        out.push_back(to_string(item.val()));
    }
    return true;
}

/**
 * @brief Extract an optional vector of strings.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output optional vector.
 * @return true if key was found.
 * @version 1.8.1
 * @utility
 */
bool extract_string_list_opt(ryml::ConstNodeRef node, c4::csubstr key,
                             std::optional<std::vector<std::string>>& out)
{
    if (!node.is_map() || !node.has_child(key)) {
        return false;
    }
    auto child = node[key];
    if (child.is_seq()) {
        // Fall through to parse the sequence below
    } else if (!child.has_val() || child.val_is_null()) {
        out = std::nullopt;
        return true;
    } else {
        return false;
    }
    std::vector<std::string> vec;
    for (auto item : child) {
        vec.push_back(to_string(item.val()));
    }
    out = std::move(vec);
    return true;
}

/**
 * @brief Extract a map of string to string from a YAML mapping node.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output map. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 * @utility
 */
bool extract_string_map(ryml::ConstNodeRef node, c4::csubstr key,
                        std::unordered_map<std::string, std::string>& out)
{
    if (!node.is_map() || !node.has_child(key)) {
        return false;
    }
    auto child = node[key];
    if (!child.is_map()) {
        return false;
    }
    out.clear();
    for (auto item : child) {
        out[to_string(item.key())] = to_string(item.val());
    }
    return true;
}

/**
 * @brief Extract a map of string to list of strings from a YAML mapping.
 * @param node The ryml node to extract from.
 * @param key The key to look up.
 * @param[out] out Output map. Unchanged if key is absent.
 * @return true if key was found and extracted.
 * @version 1.8.1
 * @utility
 */
bool extract_string_list_map(
    ryml::ConstNodeRef node, c4::csubstr key,
    std::unordered_map<std::string, std::vector<std::string>>& out)
{
    if (!node.is_map() || !node.has_child(key)) {
        return false;
    }
    auto child = node[key];
    if (!child.is_map()) {
        return false;
    }
    out.clear();
    for (auto item : child) {
        std::vector<std::string> vec;
        if (item.is_seq()) {
            for (auto elem : item) {
                vec.push_back(to_string(elem.val()));
            }
        }
        out[to_string(item.key())] = std::move(vec);
    }
    return true;
}

} // namespace entropic::config
