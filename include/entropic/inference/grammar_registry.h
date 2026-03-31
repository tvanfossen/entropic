/**
 * @file grammar_registry.h
 * @brief GrammarRegistry — named grammar management and validation.
 *
 * @par Responsibilities:
 * - Load bundled grammars from data/grammars/ at startup
 * - Register/deregister grammars by name at runtime
 * - Resolve grammar_key to GBNF content string for generation
 * - Validate GBNF grammar syntax without generating
 *
 * @par Thread safety:
 * - get() and has() are lock-free (read under shared lock)
 * - register/deregister acquire registry_mutex_
 * - Validation is stateless and thread-safe
 *
 * @par Ownership:
 * Owned by ModelOrchestrator. One GrammarRegistry per engine instance.
 * Replaces the _grammar_cache dict and _resolve_grammar() method from
 * the Python orchestrator.
 *
 * @version 1.9.3
 */

#pragma once

#include <entropic/types/config.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace entropic {

/**
 * @brief Centralized grammar registry for named GBNF grammars.
 *
 * Single concrete class (no three-layer hierarchy — one implementation,
 * like AdapterManager and HookRegistry).
 *
 * @version 1.9.3
 */
class GrammarRegistry {
public:
    /**
     * @brief Load all bundled grammars from a directory.
     * @param grammar_dir Path to data/grammars/ directory.
     * @return Number of grammars loaded.
     *
     * Scans for *.gbnf files. Each file's stem becomes the key
     * (e.g., "compactor.gbnf" -> key "compactor"). Files are read,
     * validated, and registered. Invalid files are logged as WARNING
     * and still registered with error metadata.
     *
     * @version 1.9.3
     */
    size_t load_bundled(const std::filesystem::path& grammar_dir);

    /**
     * @brief Register a grammar by key with GBNF content string.
     * @param key Unique grammar name.
     * @param gbnf_content Raw GBNF grammar string.
     * @param source Origin tag ("runtime", "dynamic", "file").
     * @return true on success. false if key already exists.
     *
     * The grammar is validated on registration. If validation fails,
     * the grammar is still registered but entry.validated is false
     * and entry.error is set.
     *
     * @version 1.9.3
     */
    bool register_grammar(const std::string& key,
                          const std::string& gbnf_content,
                          const std::string& source = "runtime");

    /**
     * @brief Register a grammar from a file path.
     * @param key Unique grammar name (if empty, uses filename stem).
     * @param path Path to .gbnf file.
     * @return true on success. false if file unreadable or key exists.
     *
     * @version 1.9.3
     */
    bool register_from_file(const std::string& key,
                            const std::filesystem::path& path);

    /**
     * @brief Remove a grammar from the registry.
     * @param key Grammar name to remove.
     * @return true if removed. false if key not found.
     *
     * Bundled grammars can be deregistered (allows overriding defaults).
     *
     * @version 1.9.3
     */
    bool deregister(const std::string& key);

    /**
     * @brief Get GBNF content string for a grammar key.
     * @param key Grammar name.
     * @return GBNF content string, or empty string if not found.
     *
     * Returns content regardless of validation status.
     *
     * @version 1.9.3
     */
    std::string get(const std::string& key) const;

    /**
     * @brief Check if a grammar key exists in the registry.
     * @param key Grammar name.
     * @return true if registered.
     *
     * @version 1.9.3
     */
    bool has(const std::string& key) const;

    /**
     * @brief Get full entry metadata for a grammar key.
     * @param key Grammar name.
     * @return GrammarEntry, or entry with empty key if not found.
     *
     * @version 1.9.3
     */
    GrammarEntry entry(const std::string& key) const;

    /**
     * @brief List all registered grammar keys.
     * @return Vector of GrammarEntry metadata (content omitted for efficiency).
     *
     * @version 1.9.3
     */
    std::vector<GrammarEntry> list() const;

    /**
     * @brief Validate a GBNF grammar string.
     * @param gbnf_content Raw GBNF string to validate.
     * @return Empty string on success, error description on failure.
     *
     * Parses the grammar using llama_sampler_init_grammar() with a
     * throwaway sampler. Does not require a loaded model. This is a
     * static utility — can be called without registering the grammar.
     *
     * @version 1.9.3
     */
    static std::string validate(const std::string& gbnf_content);

    /**
     * @brief Number of registered grammars.
     * @return Count of registered grammars.
     *
     * @version 1.9.3
     */
    size_t size() const;

    /**
     * @brief Remove all registered grammars.
     *
     * @version 1.9.3
     */
    void clear();

private:
    /// @brief All registered grammars, keyed by name.
    std::unordered_map<std::string, GrammarEntry> grammars_;

    /// @brief Guards mutations to grammars_ map.
    mutable std::mutex registry_mutex_;
};

} // namespace entropic
