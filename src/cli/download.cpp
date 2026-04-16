// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file download.cpp
 * @brief `entropic download` subcommand — fetch GGUF models to the
 *        canonical install path.
 *
 * Simulates what the Python wheel's cli_download.py does, for tarball
 * consumers who don't have pip. Shells out to curl — always present on
 * Linux targets, avoids linking libcurl or an SSL backend into the
 * main library.
 *
 * Usage:
 *   entropic download <model-key>        # e.g., `entropic download primary`
 *   entropic download --list             # list available keys
 *   entropic download --dir DIR <key>    # override target directory
 *
 * Target: $ENTROPIC_MODEL_DIR → ~/.entropic/models → /opt/entropic/models
 * (first writable; default ~/.entropic/models). Skips the download if
 * the target file already exists and is non-empty, so reruns are cheap.
 *
 * @version 2.0.5
 */

#include <entropic/config/bundled_models.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace entropic::cli {

namespace {

/**
 * @brief Default download target — mirrors BundledModels::resolve order.
 * @utility
 * @return Directory path (created if it doesn't exist).
 * @version 1
 */
std::filesystem::path default_model_dir()
{
    if (const char* env = std::getenv("ENTROPIC_MODEL_DIR"); env && *env) {
        return std::filesystem::path(env);
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::filesystem::path(home) / ".entropic" / "models";
    }
    return std::filesystem::path("/opt/entropic/models");
}

/**
 * @brief Print the registry as a table.
 * @utility
 * @return 0.
 * @version 1
 */
int list_models(const entropic::config::BundledModels& registry)
{
    std::printf("%-12s  %-6s  %s\n", "KEY", "SIZE", "DESCRIPTION");
    std::printf("%-12s  %-6s  %s\n", "---", "----", "-----------");
    for (const auto& [key, entry] : registry.entries()) {
        std::printf("%-12s  %4.1fGB  %s\n",
                    key.c_str(), entry.size_gb, entry.description.c_str());
    }
    return 0;
}

/**
 * @brief Shell out to `curl` to stream a URL to a destination path.
 *
 * Uses --fail-with-body (treat HTTP 4xx/5xx as errors), -L (follow
 * redirects — HuggingFace resolve URLs redirect to a CDN), --progress-bar
 * for human-visible progress, and --continue-at - for resume on retry.
 *
 * @param url Source URL.
 * @param dest Destination file path.
 * @return 0 on success, non-zero on curl failure.
 * @internal
 * @version 1
 */
int curl_download(const std::string& url, const std::filesystem::path& dest)
{
    pid_t pid = fork();
    if (pid < 0) {
        std::fprintf(stderr, "fork failed\n");
        return 1;
    }
    if (pid == 0) {
        execlp("curl", "curl",
               "--fail-with-body",
               "-L",
               "--progress-bar",
               "--continue-at", "-",
               "-o", dest.c_str(),
               url.c_str(),
               static_cast<char*>(nullptr));
        std::fprintf(stderr, "execlp(curl) failed — is curl installed?\n");
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 1;
}

} // anonymous namespace

struct DownloadArgs {
    std::string key;
    std::filesystem::path override_dir;
    bool want_list = false;
    bool error = false;
};

/**
 * @brief Parse command-line args for `entropic download`.
 * @utility
 * @return Parsed args with .error=true on malformed input.
 * @version 1
 */
DownloadArgs parse_download_args(int argc, char* argv[])
{
    DownloadArgs out;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--list" || arg == "-l") {
            out.want_list = true;
        } else if (arg == "--dir" && i + 1 < argc) {
            out.override_dir = argv[++i];
        } else if (!arg.empty() && arg[0] != '-') {
            out.key = arg;
        } else {
            std::fprintf(stderr, "entropic download: unknown option '%s'\n",
                         arg.c_str());
            out.error = true;
        }
    }
    return out;
}

/**
 * @brief Resolve a registry entry by key with user-friendly error output.
 * @utility
 * @return Pointer to entry, or nullptr on failure (message printed).
 * @version 1
 */
const entropic::config::BundledModelEntry* resolve_entry(
    const entropic::config::BundledModels& registry, const std::string& key)
{
    const auto* entry = registry.get(key);
    if (!entry) {
        std::fprintf(stderr,
                     "entropic download: unknown model key '%s'. "
                     "Run `entropic download --list` for available keys.\n",
                     key.c_str());
        return nullptr;
    }
    if (entry->url.empty()) {
        std::fprintf(stderr,
                     "entropic download: model '%s' has no URL in the registry.\n",
                     key.c_str());
        return nullptr;
    }
    return entry;
}

/**
 * @brief Download one registry entry to a target directory.
 * @utility
 * @return 0 on success (or already-present), non-zero on failure.
 * @version 1
 */
int fetch_to(const entropic::config::BundledModelEntry& entry,
             const std::filesystem::path& target_dir,
             const std::string& key)
{
    int rc = 0;
    std::error_code ec;
    std::filesystem::create_directories(target_dir, ec);
    auto target_file = target_dir / (entry.name + ".gguf");
    const bool already = std::filesystem::exists(target_file)
        && std::filesystem::file_size(target_file) > 0;

    if (ec) {
        std::fprintf(stderr, "entropic download: cannot create %s: %s\n",
                     target_dir.c_str(), ec.message().c_str());
        rc = 1;
    } else if (already) {
        std::printf("Model '%s' already at %s — nothing to do.\n",
                    key.c_str(), target_file.c_str());
    } else {
        std::printf("Downloading %s (%.1f GB)\n  from: %s\n  to:   %s\n",
                    entry.name.c_str(), entry.size_gb,
                    entry.url.c_str(), target_file.c_str());
        rc = curl_download(entry.url, target_file);
        if (rc != 0) {
            std::fprintf(stderr,
                         "entropic download: curl exited with status %d\n"
                         "Partial file left at %s — rerun to resume.\n",
                         rc, target_file.c_str());
        } else {
            std::printf("\nDone. Point the engine at this dir with:\n"
                        "  export ENTROPIC_MODEL_DIR=%s\n",
                        target_dir.c_str());
        }
    }
    return rc;
}

/**
 * @brief Handle `entropic download` subcommand.
 *
 * @param argc argc-like count starting at subcommand name.
 * @param argv argv-like vector starting at subcommand name.
 * @return 0 on success, non-zero on usage/runtime error.
 *
 * @internal
 * @version 2.0.5
 */
/**
 * @brief Dispatch after args parsed + registry loaded.
 * @utility
 * @return Subcommand exit code.
 * @version 1
 */
int dispatch(const DownloadArgs& args,
             const entropic::config::BundledModels& registry)
{
    int rc = 0;
    if (args.want_list) {
        rc = list_models(registry);
    } else if (args.key.empty()) {
        std::fprintf(stderr,
                     "Usage: entropic download <model-key>\n"
                     "       entropic download --list\n"
                     "       entropic download --dir DIR <model-key>\n");
        rc = 1;
    } else if (const auto* entry = resolve_entry(registry, args.key); !entry) {
        rc = 1;
    } else {
        auto target_dir = args.override_dir.empty()
            ? default_model_dir() : args.override_dir;
        rc = fetch_to(*entry, target_dir, args.key);
    }
    return rc;
}

/**
 * @brief Handle `entropic download` subcommand.
 *
 * @param argc argc-like count starting at subcommand name.
 * @param argv argv-like vector starting at subcommand name.
 * @return 0 on success, non-zero on usage/runtime error.
 *
 * @internal
 * @version 2.0.5
 */
int run_download(int argc, char* argv[])
{
    auto args = parse_download_args(argc, argv);
    int rc = 0;
    if (args.error) {
        rc = 1;
    } else {
        entropic::config::BundledModels registry;
        auto err = registry.auto_discover_and_load();
        if (!err.empty()) {
            std::fprintf(stderr,
                         "entropic download: cannot find bundled_models.yaml: %s\n"
                         "(Expected under <install-prefix>/share/entropic/ or the source tree's data/.)\n",
                         err.c_str());
            rc = 1;
        } else {
            rc = dispatch(args, registry);
        }
    }
    return rc;
}

} // namespace entropic::cli
