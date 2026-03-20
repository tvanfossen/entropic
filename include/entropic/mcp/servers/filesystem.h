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
#include <entropic/types/config.h>

#include <filesystem>
#include <memory>
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
     * @brief Check if a file was read and content unchanged.
     * @param path Canonical file path.
     * @param current_hash Current content hash.
     * @return true if read and unchanged.
     * @version 1.8.5
     */
    bool was_read_unchanged(const std::string& path,
                            size_t current_hash) const;

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
     * @brief Resolve and validate a path against root.
     * @param requested User-requested path.
     * @return Resolved canonical path.
     * @throws std::runtime_error if path escapes root.
     * @version 1.8.5
     */
    std::filesystem::path resolve_path(const std::string& requested) const;

private:
    std::filesystem::path root_dir_;  ///< Project root
    FilesystemConfig config_;         ///< Filesystem config
    int max_read_bytes_ = 0;          ///< Size gate limit
    FileAccessTracker tracker_;       ///< Read tracking

    // Owned tool instances
    std::unique_ptr<ReadFileTool> read_file_;
    std::unique_ptr<WriteFileTool> write_file_;
    std::unique_ptr<EditFileTool> edit_file_;
    std::unique_ptr<GlobTool> glob_;
    std::unique_ptr<GrepTool> grep_;
    std::unique_ptr<ListDirectoryTool> list_dir_;
};

} // namespace entropic
