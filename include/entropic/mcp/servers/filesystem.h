// SPDX-License-Identifier: Apache-2.0
/**
 * @file filesystem.h
 * @brief Filesystem MCP server — read/write/edit/glob/grep/list_directory.
 *
 * Enforces read-before-write via FileAccessTracker, size gate on reads,
 * ContextAnchor on read_file, path security (no traversal outside root).
 *
 * @version 1.8.5
 */

#pragma once

#include <entropic/mcp/server_base.h>
#include <entropic/mcp/servers/ignore_matcher.h>
#include <entropic/types/config.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace entropic {

/**
 * @brief Tracks file read state for read-before-write enforcement.
 * @version 1.8.5
 */
class FileAccessTracker {
public:
    /**
     * @brief Record that a file was read.
     * @param path Canonical file path.
     * @param hash Content hash at time of read.
     * @version 1.8.5
     */
    void record_read(const std::string& path, size_t hash);


    /**
     * @brief Check if a file was ever read.
     * @param path Canonical file path.
     * @return true if previously read.
     * @version 1.8.5
     */
    bool was_read(const std::string& path) const;

private:
    std::unordered_map<std::string, size_t> reads_; ///< path → hash
};

/**
 * @brief What a filesystem tool is about to do with a path (v2.13.0).
 *
 * Carried to the host's approver so it can say "wants to WRITE /etc/…",
 * not just "wants /etc/…". read_file and list_directory read; write_file
 * and edit_file write.
 *
 * @version 2.13.0
 */
enum class PathAccess {
    read,   ///< read_file, list_directory
    write,  ///< write_file, edit_file
};

/**
 * @brief One outside-root access awaiting the host's decision (v2.13.0).
 * @version 2.13.0
 */
struct OutsideRootRequest {
    std::string path;   ///< Canonical absolute path the tool resolved
    std::string root;   ///< Canonical root the path lies outside
    std::string tool;   ///< Fully-qualified tool ("filesystem.write_file")
    PathAccess access = PathAccess::read; ///< Read or write
};

/**
 * @brief The host's answer to an OutsideRootRequest (v2.13.0).
 *
 * `no_approver` is distinct from `rejected` so the refusal can tell the
 * operator the truth: "nobody was asked" and "somebody said no" are
 * fixed by different configuration.
 *
 * @version 2.13.0
 */
enum class OutsideRootVerdict {
    approved,     ///< Serve this one call
    rejected,     ///< The approver said no
    no_approver,  ///< No approver is registered — refuse, fail loud
};

/**
 * @brief Approver the server consults under `allow_outside_root: optional`.
 * @param request The pending access (valid for the call only).
 * @param user_data Opaque pointer given with the approver.
 * @return The verdict for this one call.
 * @version 2.13.0
 */
using OutsideRootApprover = OutsideRootVerdict (*)(
    const OutsideRootRequest& request, void* user_data);

// Forward declarations for tool classes
class ReadFileTool;
class WriteFileTool;
class EditFileTool;
class GlobTool;
class GrepTool;
class ListDirectoryTool;

/**
 * @brief Filesystem MCP server with read-before-write enforcement.
 * @version 1.8.5
 */
class FilesystemServer : public MCPServerBase {
public:
    /**
     * @brief Construct with root directory, config, and data dir.
     * @param root_dir Project root directory.
     * @param config Filesystem configuration.
     * @param data_dir Path to bundled data directory (for tool JSONs).
     * @param model_context_bytes Model context window in bytes (for size gate).
     * @version 1.8.5
     */
    FilesystemServer(const std::filesystem::path& root_dir,
                     const FilesystemConfig& config,
                     const std::string& data_dir,
                     int model_context_bytes = 0);

    ~FilesystemServer() override;

    /**
     * @brief read_file must always execute (updates FileAccessTracker).
     * @param tool_name Tool name.
     * @return true for read_file.
     * @version 1.8.5
     */
    bool skip_duplicate_check(const std::string& tool_name) const override;

    /**
     * @brief Set working directory (changes root_dir).
     * @param path New root directory.
     * @return true on success.
     * @version 1.8.5
     */
    bool set_working_dir(const std::string& path) override;

    /**
     * @brief Get the root directory.
     * @return Root directory path.
     * @version 1.8.5
     */
    const std::filesystem::path& root_dir() const;

    /**
     * @brief Get the file access tracker.
     * @return Tracker reference.
     * @version 1.8.5
     */
    FileAccessTracker& tracker();

    /**
     * @brief Get the filesystem config.
     * @return Config reference.
     * @version 1.8.5
     */
    const FilesystemConfig& config() const;

    /**
     * @brief Get max read bytes (size gate).
     * @return Max bytes, or 0 for unlimited.
     * @version 1.8.5
     */
    int max_read_bytes() const;

    /**
     * @brief Resolve a path and apply the outside-root policy (v2.13.0).
     *
     * The single confinement point for every path-taking tool. A path
     * under the root is served. One outside it is decided by, in order:
     * `outside_root_deny`, `outside_root_allow`, then
     * `allow_outside_root` (`false` refuse / `true` serve / `optional`
     * ask the approver).
     *
     * @param requested User-requested path (absolute or root-relative).
     * @param access Whether the calling tool reads or writes it.
     * @param tool Bare tool name, for the approval request and logs.
     * @return Resolved canonical path.
     * @throws std::runtime_error on any refusal — thrown rather than
     *         returned so MCPServerBase's barrier also skips the
     *         ContextAnchor for a refused read.
     * @version 2.13.0
     */
    std::filesystem::path resolve_path(const std::string& requested,
                                       PathAccess access,
                                       const std::string& tool) const;

    /**
     * @brief Install the approver consulted under `optional` (v2.13.0).
     *
     * NOT installed on a named workspace's servers, which are confined
     * unconditionally (gh#166). Thread-safe against a concurrent call.
     *
     * @param fn Approver, or nullptr to clear (escapes then refuse with
     *        `outside_root_approval_required`).
     * @param user_data Forwarded to `fn`.
     * @version 2.13.0
     */
    void set_outside_root_approver(OutsideRootApprover fn, void* user_data);

    /**
     * @brief Get the ignore matcher (#15, v2.1.4).
     *
     * Loaded from `.gitignore` (recursive) + `.explorerignore` at
     * server construction and on `set_working_dir`. Used by glob,
     * grep, and read_file to filter results / refuse access.
     *
     * @return Matcher reference.
     * @version 2.1.4
     */
    const IgnoreMatcher& ignore() const;

private:
    /**
     * @brief Construct the six filesystem tool instances (ctor step 1).
     * @param data_dir Directory holding tool JSON definitions.
     * @dg_internal
     * @version 2.3.7
     */
    void create_fs_tools(const std::string& data_dir);

    /**
     * @brief Register the six filesystem tools (ctor step 2).
     * @dg_internal
     * @version 2.3.7
     */
    void register_fs_tools();

    /**
     * @brief Decide a path that resolved outside the root (v2.13.0).
     * @param resolved Canonical path outside the root.
     * @param access Read or write.
     * @param tool Bare tool name.
     * @throws std::runtime_error when refused.
     * @dg_internal
     * @version 2.13.0
     */
    void authorize_outside_root(const std::filesystem::path& resolved,
                                PathAccess access,
                                const std::string& tool) const;

    /**
     * @brief Put one outside-root access to the approver (v2.13.0).
     * @param resolved Canonical path outside the root.
     * @param access Read or write.
     * @param tool Bare tool name.
     * @throws std::runtime_error unless the verdict is `approved`.
     * @dg_internal
     * @version 2.13.0
     */
    void ask_outside_root_approver(const std::filesystem::path& resolved,
                                   PathAccess access,
                                   const std::string& tool) const;

    /**
     * @brief Log the outside-root policy, warning on ineffective entries.
     * @dg_internal
     * @version 2.13.0
     */
    void log_outside_root_policy() const;

    std::filesystem::path root_dir_;  ///< Project root
    FilesystemConfig config_;         ///< Filesystem config
    int max_read_bytes_ = 0;          ///< Size gate limit
    FileAccessTracker tracker_;       ///< Read tracking
    IgnoreMatcher ignore_;            ///< gitignore + explorerignore (#15)

    /// @brief Guards the approver pair against a concurrent install.
    mutable std::mutex approver_mutex_;
    OutsideRootApprover approver_ = nullptr; ///< Under `optional` (v2.13.0)
    void* approver_data_ = nullptr;          ///< Forwarded to approver_

    // Owned tool instances
    std::unique_ptr<ReadFileTool> read_file_;
    std::unique_ptr<WriteFileTool> write_file_;
    std::unique_ptr<EditFileTool> edit_file_;
    std::unique_ptr<GlobTool> glob_;
    std::unique_ptr<GrepTool> grep_;
    std::unique_ptr<ListDirectoryTool> list_dir_;
};

} // namespace entropic
