// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file permission_persister.h
 * @brief Persist permission allow/deny patterns to project config.
 *
 * Read-modify-write of YAML config files using ryml. Thread-safe
 * via internal mutex.
 *
 * @version 1.8.8
 */

#pragma once

#include <filesystem>
#include <mutex>
#include <string_view>

namespace entropic {

/**
 * @brief Read-modify-write permission patterns in YAML config.
 *
 * Loads the project's config.local.yaml, adds patterns to the
 * permissions.allow or permissions.deny list, and writes back.
 * Thread-safe via internal mutex.
 *
 * @par Example:
 * @code
 *   PermissionPersister pp(".entropic/");
 *   pp.save_permission("bash.execute:pytest *", true);   // allow
 *   pp.save_permission("filesystem.write:/etc/", false);  // deny
 * @endcode
 *
 * @version 1.8.8
 */
class PermissionPersister {
public:
    /**
     * @brief Construct with config directory path.
     * @param config_dir Path to app directory (e.g., ".entropic/").
     * @version 1.8.8
     */
    explicit PermissionPersister(const std::filesystem::path& config_dir);

    /**
     * @brief Save a permission pattern.
     * @param pattern Permission pattern (e.g., "bash.execute:pytest *").
     * @param allow true for allow list, false for deny list.
     * @return true on success, false on I/O error.
     * @version 1.8.8
     */
    bool save_permission(std::string_view pattern, bool allow);

private:
    std::filesystem::path config_path_; ///< Path to config.local.yaml
    std::mutex write_mutex_;            ///< Serializes writes
};

} // namespace entropic
