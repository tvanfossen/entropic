/**
 * @file permission_persister.cpp
 * @brief PermissionPersister implementation using string-based YAML editing.
 *
 * Uses simple line-based YAML parsing for the narrow use case of
 * appending patterns to permissions.allow/deny lists.
 *
 * @version 1.8.8
 */

#include <entropic/storage/permission_persister.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace entropic {

/**
 * @brief Construct with config directory path.
 * @param config_dir Path to app config directory.
 * @internal
 * @version 1.8.8
 */
PermissionPersister::PermissionPersister(
        const std::filesystem::path& config_dir)
    : config_path_(config_dir / "config.local.yaml") {}

/**
 * @brief Read file contents as string.
 * @param path File path.
 * @return File contents or empty string.
 * @utility
 * @version 1.8.8
 */
static std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/**
 * @brief Write string to file.
 * @param path File path.
 * @param content Content to write.
 * @return true on success.
 * @utility
 * @version 1.8.8
 */
static bool write_file(const std::filesystem::path& path,
                       const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    if (!out.is_open()) return false;
    out << content;
    return out.good();
}

/**
 * @brief Split string into lines.
 * @param content Input string.
 * @return Vector of lines.
 * @utility
 * @version 1.8.8
 */
static std::vector<std::string> split_lines(const std::string& content) {
    std::vector<std::string> lines;
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }
    return lines;
}

/**
 * @brief Check if a YAML sequence already contains a pattern.
 * @param lines File lines.
 * @param start Index of the list key line.
 * @param pattern Pattern to search for.
 * @return true if found.
 * @utility
 * @version 1.8.8
 */
static bool list_contains(const std::vector<std::string>& lines,
                          size_t start, std::string_view pattern) {
    std::string needle = "- " + std::string(pattern);
    for (size_t i = start + 1; i < lines.size(); ++i) {
        auto pos = lines[i].find_first_not_of(' ');
        if (pos == std::string::npos) continue;
        auto trimmed = lines[i].substr(pos);
        if (!trimmed.starts_with("- ")) break;
        if (trimmed == needle) return true;
    }
    return false;
}

/**
 * @brief Find the end of a YAML sequence (last "- " item).
 * @param lines File lines.
 * @param list_start Index of the list key line.
 * @return Iterator past the last sequence item.
 * @utility
 * @version 1.8.8
 */
static auto find_list_end(std::vector<std::string>& lines,
                          size_t list_start) {
    auto ins = lines.begin()
             + static_cast<long>(list_start) + 1;
    while (ins != lines.end()) {
        auto pos = ins->find_first_not_of(' ');
        bool is_item = pos != std::string::npos
                    && ins->substr(pos).starts_with("- ");
        if (!is_item) break;
        ++ins;
    }
    return ins;
}

/**
 * @brief Find the end of a YAML section (indented block).
 * @param lines File lines.
 * @param section_start Iterator to section key.
 * @return Iterator past the indented block.
 * @utility
 * @version 1.8.8
 */
static auto find_section_end(std::vector<std::string>& lines,
                             std::vector<std::string>::iterator start) {
    auto it = start + 1;
    while (it != lines.end() && !it->empty()
           && ((*it)[0] == ' ' || (*it)[0] == '\t')) {
        ++it;
    }
    return it;
}

/**
 * @brief Join lines back into a string with trailing newline.
 * @param lines Lines to join.
 * @return Joined string.
 * @utility
 * @version 1.8.8
 */
static std::string join_lines(const std::vector<std::string>& lines) {
    std::ostringstream out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) out << '\n';
    }
    auto result = out.str();
    if (!result.empty() && result.back() != '\n') {
        result += '\n';
    }
    return result;
}

/**
 * @brief Save a permission pattern to the YAML config.
 * @param pattern Permission pattern.
 * @param allow true for allow list, false for deny list.
 * @return true on success.
 * @internal
 * @version 1.8.8
 */
bool PermissionPersister::save_permission(std::string_view pattern,
                                          bool allow) {
    std::lock_guard lock(write_mutex_);

    auto lines = split_lines(read_file(config_path_));
    const char* list_key = allow ? "allow" : "deny";
    std::string perms_key = "permissions:";
    std::string list_line = std::string("  ") + list_key + ":";
    std::string item = "    - " + std::string(pattern);

    auto perms_it = std::find(lines.begin(), lines.end(), perms_key);
    if (perms_it == lines.end()) {
        lines.push_back(perms_key);
        lines.push_back(list_line);
        lines.push_back(item);
    } else {
        auto list_it = std::find(perms_it, lines.end(), list_line);
        if (list_it == lines.end()) {
            auto pos = find_section_end(lines, perms_it);
            lines.insert(pos, item);
            lines.insert(pos, list_line);
        } else {
            size_t idx = static_cast<size_t>(
                std::distance(lines.begin(), list_it));
            if (list_contains(lines, idx, pattern)) {
                return true;
            }
            lines.insert(find_list_end(lines, idx), item);
        }
    }

    if (!write_file(config_path_, join_lines(lines))) {
        spdlog::error("Failed to write permission to {}",
                      config_path_.string());
        return false;
    }

    spdlog::info("Saved permission {}: {}",
                 allow ? "allow" : "deny", pattern);
    return true;
}

} // namespace entropic
