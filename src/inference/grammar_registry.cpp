// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file grammar_registry.cpp
 * @brief GrammarRegistry implementation — named grammar management.
 *
 * @version 1.9.3
 */

#include <entropic/inference/grammar_registry.h>
#include <entropic/types/logging.h>

#include <llama.h>

#include <fstream>
#include <sstream>

namespace entropic {

namespace {
auto logger = entropic::log::get("inference.grammar_registry");

/**
 * @brief Read a file into a string.
 * @param path File path.
 * @return File contents, or empty string on failure.
 * @utility
 * @version 1.9.3
 */
std::string read_file_contents(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

} // anonymous namespace

/**
 * @brief Load all bundled grammars from a directory.
 * @param grammar_dir Path to data/grammars/ directory.
 * @return Number of grammars loaded.
 * @internal
 * @version 1.9.3
 */
size_t GrammarRegistry::load_bundled(
    const std::filesystem::path& grammar_dir)
{
    if (!std::filesystem::is_directory(grammar_dir)) {
        logger->warn("Grammar directory not found: {}",
                     grammar_dir.string());
        return 0;
    }

    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(grammar_dir)) {
        if (entry.path().extension() != ".gbnf") {
            continue;
        }

        std::string key = entry.path().stem().string();
        std::string content = read_file_contents(entry.path());
        if (content.empty()) {
            logger->warn("Empty or unreadable grammar file: {}",
                         entry.path().string());
            continue;
        }

        std::string error = validate(content);
        GrammarEntry ge;
        ge.key = key;
        ge.gbnf_content = std::move(content);
        ge.source = "bundled";
        ge.validated = error.empty();
        ge.error = std::move(error);

        if (!ge.validated) {
            logger->warn("Bundled grammar '{}' has validation errors: {}",
                         key, ge.error);
        }

        std::lock_guard<std::mutex> lock(registry_mutex_);
        grammars_[key] = std::move(ge);
        ++count;
    }

    logger->info("Loaded {} bundled grammar(s)", count);
    return count;
}

/**
 * @brief Register a grammar by key with GBNF content string.
 * @param key Unique grammar name.
 * @param gbnf_content Raw GBNF grammar string.
 * @param source Origin tag.
 * @return true on success. false if key already exists.
 * @internal
 * @version 1.9.3
 */
bool GrammarRegistry::register_grammar(
    const std::string& key,
    const std::string& gbnf_content,
    const std::string& source)
{
    std::lock_guard<std::mutex> lock(registry_mutex_);
    if (grammars_.count(key) > 0) {
        logger->warn("Grammar key '{}' already registered", key);
        return false;
    }

    std::string error = validate(gbnf_content);
    GrammarEntry ge;
    ge.key = key;
    ge.gbnf_content = gbnf_content;
    ge.source = source;
    ge.validated = error.empty();
    ge.error = std::move(error);

    grammars_[key] = std::move(ge);
    logger->info("Registered grammar '{}' (source={}, valid={})",
                 key, source, grammars_[key].validated);
    return true;
}

/**
 * @brief Register a grammar from a file path.
 * @param key Unique grammar name (if empty, uses filename stem).
 * @param path Path to .gbnf file.
 * @return true on success. false if file unreadable or key exists.
 * @internal
 * @version 1.9.3
 */
bool GrammarRegistry::register_from_file(
    const std::string& key,
    const std::filesystem::path& path)
{
    std::string content = read_file_contents(path);
    if (content.empty()) {
        logger->error("Cannot read grammar file: {}", path.string());
        return false;
    }

    std::string resolved_key = key.empty()
        ? path.stem().string() : key;
    return register_grammar(resolved_key, content, "file");
}

/**
 * @brief Remove a grammar from the registry.
 * @param key Grammar name to remove.
 * @return true if removed. false if key not found.
 * @internal
 * @version 1.9.3
 */
bool GrammarRegistry::deregister(const std::string& key) {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    auto it = grammars_.find(key);
    if (it == grammars_.end()) {
        return false;
    }
    grammars_.erase(it);
    logger->info("Deregistered grammar '{}'", key);
    return true;
}

/**
 * @brief Get GBNF content string for a grammar key.
 * @param key Grammar name.
 * @return GBNF content string, or empty string if not found.
 * @internal
 * @version 1.9.3
 */
std::string GrammarRegistry::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    auto it = grammars_.find(key);
    if (it == grammars_.end()) {
        return "";
    }
    return it->second.gbnf_content;
}

/**
 * @brief Check if a grammar key exists in the registry.
 * @param key Grammar name.
 * @return true if registered.
 * @internal
 * @version 1.9.3
 */
bool GrammarRegistry::has(const std::string& key) const {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    return grammars_.count(key) > 0;
}

/**
 * @brief Get full entry metadata for a grammar key.
 * @param key Grammar name.
 * @return GrammarEntry, or entry with empty key if not found.
 * @internal
 * @version 1.9.3
 */
GrammarEntry GrammarRegistry::entry(const std::string& key) const {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    auto it = grammars_.find(key);
    if (it == grammars_.end()) {
        return {};
    }
    return it->second;
}

/**
 * @brief List all registered grammar keys.
 * @return Vector of GrammarEntry metadata (content omitted).
 * @internal
 * @version 1.9.3
 */
std::vector<GrammarEntry> GrammarRegistry::list() const {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    std::vector<GrammarEntry> result;
    result.reserve(grammars_.size());
    for (const auto& [k, e] : grammars_) {
        GrammarEntry meta;
        meta.key = e.key;
        meta.source = e.source;
        meta.validated = e.validated;
        meta.error = e.error;
        result.push_back(std::move(meta));
    }
    return result;
}

/**
 * @brief Validate a GBNF grammar string.
 * @param gbnf_content Raw GBNF string to validate.
 * @return Empty string on success, error description on failure.
 * @internal
 * @version 1.9.3
 */
std::string GrammarRegistry::validate(const std::string& gbnf_content) {
    if (gbnf_content.empty()) {
        return "empty grammar string";
    }

    // Use llama_sampler_init_grammar with nullptr vocab for validation.
    // Grammar parsing is independent of vocabulary — the GBNF parser
    // only needs the grammar string. Returns nullptr on parse failure.
    llama_sampler* sampler = llama_sampler_init_grammar(
        nullptr, gbnf_content.c_str(), "root");

    if (sampler == nullptr) {
        return "GBNF parse failed";
    }

    llama_sampler_free(sampler);
    return "";
}

/**
 * @brief Number of registered grammars.
 * @return Count of registered grammars.
 * @internal
 * @version 1.9.3
 */
size_t GrammarRegistry::size() const {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    return grammars_.size();
}

/**
 * @brief Remove all registered grammars.
 * @internal
 * @version 1.9.3
 */
void GrammarRegistry::clear() {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    grammars_.clear();
    logger->info("Cleared all grammars");
}

} // namespace entropic
