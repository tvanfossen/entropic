// SPDX-License-Identifier: Apache-2.0
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
    std::string family;  // gh#62 selector
    std::string size;    // gh#62 selector
    std::string quant;   // gh#62 selector
    bool want_list = false;
    bool error = false;
};

/**
 * @brief Match a `--flag VALUE` pair and consume the value (gh#62).
 * @return true if `arg` matched `flag` and a value was consumed into `dst`.
 * @utility
 * @version 2.8.0
 */
static bool match_value_flag(const std::string& arg, const char* flag,
                             int& i, int argc, char* argv[], std::string& dst)
{
    if (arg == flag && i + 1 < argc) {
        dst = argv[++i];
        return true;
    }
    return false;
}

/**
 * @brief Try the gh#62 selector value-flags (--family/--size/--quant).
 *
 * Short-circuits so the first match consumes its value; grouping the three
 * keeps parse_download_args under the knots cognitive gate.
 * @utility
 * @return true if `arg` matched one selector flag and consumed its value.
 * @version 2.8.0
 */
static bool match_any_value_flag(const std::string& arg, int& i, int argc,
                                 char* argv[], DownloadArgs& out)
{
    return match_value_flag(arg, "--family", i, argc, argv, out.family)
        || match_value_flag(arg, "--size", i, argc, argv, out.size)
        || match_value_flag(arg, "--quant", i, argc, argv, out.quant);
}

/**
 * @brief Parse command-line args for `entropic download`.
 * @utility
 * @return Parsed args with .error=true on malformed input.
 * @version 2.8.0
 */
DownloadArgs parse_download_args(int argc, char* argv[])
{
    DownloadArgs out;
    // Early-continue (flat) rather than an else-if chain — keeps the knots
    // nesting gate happy as selector flags are added.
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--list" || arg == "-l") { out.want_list = true; continue; }
        if (arg == "--dir" && i + 1 < argc) { out.override_dir = argv[++i]; continue; }
        if (match_any_value_flag(arg, i, argc, argv, out)) { continue; }
        if (!arg.empty() && arg[0] != '-') { out.key = arg; continue; }
        std::fprintf(stderr, "entropic download: unknown option '%s'\n",
                     arg.c_str());
        out.error = true;
    }
    return out;
}

/**
 * @brief Resolve the target model key from an explicit key or a
 *        `--family/--size/--quant` selector (gh#62).
 * @return Registry key, or "" if unresolved. A partial or unmatched selector
 *         prints a specific error; an absent selector is silent (caller
 *         prints usage).
 * @utility
 * @version 2.8.0
 */
std::string resolve_selector_key(
    const DownloadArgs& args,
    const entropic::config::BundledModels& registry)
{
    if (!args.key.empty()) { return args.key; }
    const bool hf = !args.family.empty();
    const bool hs = !args.size.empty();
    const bool hq = !args.quant.empty();
    if (!hf && !hs && !hq) { return ""; }
    std::string key;
    if (!(hf && hs && hq)) {
        std::fprintf(stderr, "entropic download: --family/--size/--quant "
                     "must be given together (or pass a model key).\n");
    } else {
        key = registry.find_by(args.family, args.size, args.quant);
        if (key.empty()) {
            std::fprintf(stderr, "entropic download: no bundled model matches "
                         "--family %s --size %s --quant %s. Try --list.\n",
                         args.family.c_str(), args.size.c_str(),
                         args.quant.c_str());
        }
    }
    return key;
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
 * @brief Paired-mmproj follow-up fetch (gh#42, v2.1.8).
 *
 * If `entry.mmproj_key` names a registry entry, downloads it into
 * the same `target_dir` as the base model. Best-effort: missing
 * paired key returns 0 (text-only mode still works); a failed
 * download propagates the curl exit code so the caller surfaces it.
 *
 * @param entry Already-downloaded base model entry.
 * @param registry Bundled models registry.
 * @param target_dir Directory the base GGUF landed in.
 * @return 0 on success or no-pairing, non-zero on mmproj fetch error.
 * @utility
 * @version 2.1.8
 */
int fetch_mmproj_if_paired(
    const entropic::config::BundledModelEntry& entry,
    const entropic::config::BundledModels& registry,
    const std::filesystem::path& target_dir)
{
    if (entry.mmproj_key.empty()) { return 0; }
    const auto* mm = resolve_entry(registry, entry.mmproj_key);
    if (mm == nullptr) { return 0; }
    std::printf("Fetching paired mmproj '%s' for vision support\n",
                entry.mmproj_key.c_str());
    return fetch_to(*mm, target_dir, entry.mmproj_key);
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
 * @version 2.8.0
 */
int dispatch(const DownloadArgs& args,
             const entropic::config::BundledModels& registry)
{
    int rc = 0;
    const bool has_selector = !args.family.empty() || !args.size.empty()
                              || !args.quant.empty();
    if (args.want_list) {
        rc = list_models(registry);
    } else if (const std::string key = resolve_selector_key(args, registry);
               key.empty()) {
        if (!has_selector) {
            std::fprintf(stderr,
                         "Usage: entropic download <model-key>\n"
                         "       entropic download --family F --size S --quant Q\n"
                         "       entropic download --list\n"
                         "       entropic download --dir DIR <model-key>\n");
        }
        rc = 1;
    } else if (const auto* entry = resolve_entry(registry, key); !entry) {
        rc = 1;
    } else {
        auto target_dir = args.override_dir.empty()
            ? default_model_dir() : args.override_dir;
        rc = fetch_to(*entry, target_dir, key);
        if (rc == 0) {
            rc = fetch_mmproj_if_paired(*entry, registry, target_dir);
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
