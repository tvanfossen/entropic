// SPDX-License-Identifier: LGPL-3.0-or-later
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
