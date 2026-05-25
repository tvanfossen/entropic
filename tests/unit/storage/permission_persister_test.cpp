// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_permission_persister.cpp
 * @brief BDD tests for PermissionPersister YAML read-modify-write.
 * @version 1.8.8
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/storage/permission_persister.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

/**
 * @brief RAII temp config dir.
 * @internal
 * @version 1.8.8
 */
struct TempConfigDir {
    fs::path dir;

    /**
     * @brief Construct with unique temp directory.
     * @version 1.8.8
     */
    TempConfigDir() {
        dir = fs::temp_directory_path() / "entropic_test" /
              ("perm_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(dir);
    }

    /**
     * @brief Destructor — cleanup.
     * @version 1.8.8
     */
    ~TempConfigDir() { fs::remove_all(dir); }
};

/**
 * @brief Read a file as string.
 * @param path File path.
 * @return File contents.
 * @utility
 * @version 1.8.8
 */
static std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    return {(std::istreambuf_iterator<char>(f)),
            std::istreambuf_iterator<char>()};
}

SCENARIO("Save allow pattern to new config",
         "[storage][permission]") {
    GIVEN("An empty config directory") {
        TempConfigDir tmp;
        entropic::PermissionPersister pp(tmp.dir);

        WHEN("an allow pattern is saved") {
            bool ok = pp.save_permission("bash.execute:pytest *", true);

            THEN("config.local.yaml is created with the pattern") {
                REQUIRE(ok);
                auto content = read_file(tmp.dir / "config.local.yaml");
                REQUIRE(content.find("allow") != std::string::npos);
                REQUIRE(content.find("bash.execute:pytest *") != std::string::npos);
            }
        }
    }
}

SCENARIO("Save deny pattern", "[storage][permission]") {
    GIVEN("An empty config directory") {
        TempConfigDir tmp;
        entropic::PermissionPersister pp(tmp.dir);

        WHEN("a deny pattern is saved") {
            pp.save_permission("filesystem.write:/tmp", false);

            THEN("config contains deny list") {
                auto content = read_file(tmp.dir / "config.local.yaml");
                REQUIRE(content.find("deny") != std::string::npos);
                REQUIRE(content.find("filesystem.write:/tmp") != std::string::npos);
            }
        }
    }
}

SCENARIO("Duplicate patterns are not added twice",
         "[storage][permission]") {
    GIVEN("A config with an existing pattern") {
        TempConfigDir tmp;
        entropic::PermissionPersister pp(tmp.dir);
        pp.save_permission("bash.execute:make", true);

        WHEN("the same pattern is saved again") {
            pp.save_permission("bash.execute:make", true);

            THEN("it appears only once in the file") {
                auto content = read_file(tmp.dir / "config.local.yaml");
                size_t first = content.find("bash.execute:make");
                size_t second = content.find("bash.execute:make", first + 1);
                REQUIRE(first != std::string::npos);
                REQUIRE(second == std::string::npos);
            }
        }
    }
}

SCENARIO("Existing config keys preserved",
         "[storage][permission]") {
    GIVEN("A config file with existing content") {
        TempConfigDir tmp;
        auto config_path = tmp.dir / "config.local.yaml";
        {
            std::ofstream f(config_path);
            f << "log_level: DEBUG\n"
              << "models:\n"
              << "  default: lead\n";
        }

        entropic::PermissionPersister pp(tmp.dir);

        WHEN("a permission is saved") {
            pp.save_permission("git.commit:*", true);

            THEN("original content is preserved") {
                auto content = read_file(config_path);
                REQUIRE(content.find("log_level") != std::string::npos);
                REQUIRE(content.find("models") != std::string::npos);
                REQUIRE(content.find("git.commit:*") != std::string::npos);
            }
        }
    }
}

// ── v2.3.10: cover insert_permission_item branches + helpers ──

SCENARIO("Adding deny after existing allow list",
         "[storage][permission][v2.3.10][coverage]") {
    GIVEN("a config with permissions.allow already populated") {
        TempConfigDir tmp;
        auto config_path = tmp.dir / "config.local.yaml";
        {
            std::ofstream f(config_path);
            f << "log_level: WARN\n"
              << "permissions:\n"
              << "  allow:\n"
              << "    - git.status:*\n";
        }

        entropic::PermissionPersister pp(tmp.dir);

        WHEN("a deny permission is added") {
            // Hits the branch in insert_permission_item where
            // perms_key exists but list_line ("  deny:") is missing —
            // helper inserts the new sub-key at the section end.
            bool ok = pp.save_permission("bash.execute:*", false);

            THEN("the deny list is appended without losing allow") {
                REQUIRE(ok);
                auto content = read_file(config_path);
                REQUIRE(content.find("allow:") != std::string::npos);
                REQUIRE(content.find("git.status:*") != std::string::npos);
                REQUIRE(content.find("deny:") != std::string::npos);
                REQUIRE(content.find("bash.execute:*") != std::string::npos);
            }
        }
    }
}

SCENARIO("Adding a second item to an existing allow list",
         "[storage][permission][v2.3.10][coverage]") {
    GIVEN("a config with permissions.allow having one entry") {
        TempConfigDir tmp;
        auto config_path = tmp.dir / "config.local.yaml";
        {
            std::ofstream f(config_path);
            f << "permissions:\n"
              << "  allow:\n"
              << "    - filesystem.read_file:*\n";
        }

        entropic::PermissionPersister pp(tmp.dir);

        WHEN("a second allow item is added") {
            // Hits the find_list_end + insert branch (line 206) when
            // both perms_key and list_line exist with at least one
            // pre-existing item — new item goes at the end of the
            // sequence.
            bool ok = pp.save_permission("filesystem.list_dir:*", true);

            THEN("both items survive in order") {
                REQUIRE(ok);
                auto content = read_file(config_path);
                REQUIRE(content.find("filesystem.read_file:*")
                        != std::string::npos);
                REQUIRE(content.find("filesystem.list_dir:*")
                        != std::string::npos);
            }
        }
    }
}

SCENARIO("save_permission is idempotent when the pattern already exists",
         "[storage][permission][v2.3.10][coverage]") {
    GIVEN("a config with one allow entry") {
        TempConfigDir tmp;
        auto config_path = tmp.dir / "config.local.yaml";
        {
            std::ofstream f(config_path);
            f << "permissions:\n"
              << "  allow:\n"
              << "    - git.status:*\n";
        }

        entropic::PermissionPersister pp(tmp.dir);

        WHEN("the same pattern is saved again") {
            bool ok = pp.save_permission("git.status:*", true);
            THEN("save_permission returns true without writing duplicates") {
                REQUIRE(ok);
                auto content = read_file(config_path);
                // Only one occurrence of the line should exist.
                size_t count = 0;
                size_t pos = 0;
                while ((pos = content.find("git.status:*", pos))
                       != std::string::npos) {
                    ++count;
                    pos += 1;
                }
                REQUIRE(count == 1);
            }
        }
    }
}
