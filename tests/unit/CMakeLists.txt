# SPDX-License-Identifier: Apache-2.0
add_executable(entropic-tests
    types/error_test.cpp
    types/config_structs_test.cpp
    types/logging_test.cpp
    types/content_test.cpp
    types/message_multimodal_test.cpp
    types/messages_json_test.cpp
    types/session_logger_test.cpp
)

target_link_libraries(entropic-tests PRIVATE
    entropic-types
    # gh#59 (v2.3.1): entropic_last_error moved to facade in v2.2.6.
    # entropic-tests' error_test.cpp still tests it (NULL-handle path),
    # so link the facade. Latent link break since v2.2.6 — surfaced
    # when the gh#59 logging rewrite forced this target to rebuild.
    entropic-facade
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.8.1: Config + Prompts tests
add_executable(entropic-config-tests
    config/bundled_models_test.cpp
    config/validate_test.cpp
    config/loader_test.cpp
    config/yaml_util_test.cpp
    config/env_overrides_test.cpp
    prompts/prompt_parse_test.cpp
    prompts/classification_prompt_test.cpp
)

target_include_directories(entropic-config-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/config
)

target_link_libraries(entropic-config-tests PRIVATE
    entropic-prompts
    entropic-config
    entropic-types
    ryml::ryml
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# Pass test data directory as a compile-time define
target_compile_definitions(entropic-config-tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../data"
)

# v1.8.2: Inference tests
# v1.8.3: Added prompt_cache_test.cpp
# v1.9.10: Added logprob_evaluation_test.cpp
# v1.9.13: Added backend_capabilities_test.cpp
add_executable(entropic-inference-tests
    inference/backend_lifecycle_test.cpp
    inference/adapter_base_test.cpp
    inference/qwen35_adapter_test.cpp
    inference/qwen36_adapter_test.cpp
    inference/gemma4_adapter_test.cpp
    inference/nemotron3_adapter_test.cpp
    # v2.3.8 (gh#71): per-adapter acceptance gate — verbatim consumer
    # emits, strict asserts, no SKIP. Catches gh#69 + gh#70 format
    # mismatches at the CPU layer on every commit.
    inference/adapter_acceptance_test.cpp
    inference/orchestrator_test.cpp
    inference/generation_params_test.cpp
    inference/prompt_cache_test.cpp
    inference/logprob_evaluation_test.cpp
    inference/vision_adapter_test.cpp
    inference/image_preprocessor_test.cpp
    inference/backend_capabilities_test.cpp
    # v2.2.4 (gh#57): VRAM-aware tier residency surface — metadata-only
    # tests that don't require a real GGUF.
    inference/residency_test.cpp
    # v2.3.10: C ABI surface coverage via MockBackend reinterpret_cast
    inference/inference_c_api_test.cpp
    # v2.3.10 top-up (gh#23): InferenceInterface factory coverage —
    # drives parse_msgs / parse_params / extract_tier / iface_* C
    # callbacks against an empty ModelOrchestrator (no real model
    # required — orchestrator returns build_no_model_error).
    inference/interface_factory_test.cpp
    # v2.3.10: LlamaCppBackend ↔ Tokenizer seam coverage via MockTokenizer
    inference/tokenizer_seam_test.cpp
    # v2.3.10: LlamaCppBackend ↔ Sampler seam coverage via MockSamplerFactory
    inference/sampler_seam_test.cpp
    # v2.3.10: REAL llama.cpp paths for LlamaCppTokenizer + do_load /
    # do_unload using vocab-only ggml-vocab-*.gguf fixtures (CPU-only,
    # sub-100MB). Pushes llama_cpp_tokenizer.cpp 0% → ~100% and the
    # load/unload portions of llama_cpp_backend.cpp 4% → ~20-35%
    # without needing real-weight models. Links `llama` directly for
    # the vocab-only model load helper.
    inference/vocab_only_smoke_test.cpp
    # v2.3.10: Unit-scope real-model smoke. Loads
    # $HOME/.entropic/models/Qwen3.5-0.8B-Q8_0.gguf (~0.8GB, CPU-only)
    # to drive load → activate → generate → deactivate → unload paths
    # in llama_cpp_backend.cpp that vocab-only smoke can't reach.
    # WARN-skips when the model is absent so the suite still runs.
    inference/backend_real_model_smoke_test.cpp
)

target_include_directories(entropic-inference-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference
)

target_link_libraries(entropic-inference-tests PRIVATE
    entropic::inference
    entropic-types
    # v2.3.10: vocab_only_smoke_test.cpp calls the llama.cpp C API
    # directly (llama_model_load_from_file with vocab_only=true) to
    # build a borrowed llama_vocab* for the LlamaCppTokenizer real-vocab
    # smoke. The other inference tests only need entropic::inference,
    # so this link is specifically for the vocab-only smoke path.
    llama
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.9.11: Test data directory for image preprocessor tests
# v2.3.10: ENTROPIC_SOURCE_DIR resolves extern/llama.cpp/models/
# ggml-vocab-*.gguf fixtures for vocab_only_smoke_test.cpp.
target_compile_definitions(entropic-inference-tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../data"
    ENTROPIC_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

# v1.9.2: Adapter manager tests (uses mock llama.cpp adapter symbols)
add_executable(entropic-adapter-tests
    inference/adapter_manager_test.cpp
    inference/adapter_orchestrator_test.cpp
    ${PROJECT_SOURCE_DIR}/src/inference/adapter_manager.cpp
)

target_include_directories(entropic-adapter-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference
)

target_link_libraries(entropic-adapter-tests PRIVATE
    entropic-types
    llama
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.9.3: Grammar registry tests
add_executable(entropic-grammar-tests
    inference/grammar_registry_test.cpp
    inference/grammar_orchestrator_test.cpp
    ${PROJECT_SOURCE_DIR}/src/inference/grammar_registry.cpp
)

target_include_directories(entropic-grammar-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference
)

target_link_libraries(entropic-grammar-tests PRIVATE
    entropic-types
    llama
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v2.1.11: Speculative compat helper tests (mock llama_vocab_* symbols
# at link time — no real model required).
add_executable(entropic-speculative-compat-tests
    inference/speculative_compat_test.cpp
    ${PROJECT_SOURCE_DIR}/src/inference/speculative_compat.cpp
)

target_include_directories(entropic-speculative-compat-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference
)

target_link_libraries(entropic-speculative-compat-tests PRIVATE
    entropic-types
    llama
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v2.1.11: SecondaryModelLoader role-state tests
add_executable(entropic-secondary-loader-tests
    inference/secondary_model_loader_test.cpp
)

target_link_libraries(entropic-secondary-loader-tests PRIVATE
    entropic::inference
    entropic-types
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.9.7: Profile registry + throughput tracker tests
add_executable(entropic-cap-tests
    inference/profile_registry_test.cpp
    inference/throughput_tracker_test.cpp
    ${PROJECT_SOURCE_DIR}/src/inference/profile_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/inference/throughput_tracker.cpp
)

target_include_directories(entropic-cap-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference
)

target_link_libraries(entropic-cap-tests PRIVATE
    entropic-types
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.8.4: Core engine tests
# v1.8.6: Added delegation, worktree, auto-chain tests
# v2.1.5: worktree replaced by sandbox (gh#29)
add_executable(entropic-core-tests
    core/compaction_test.cpp
    core/compactor_registry_test.cpp
    core/directives_test.cpp
    core/context_manager_test.cpp
    core/engine_test.cpp
    core/sandbox_test.cpp
    core/delegation_manager_test.cpp
    core/auto_chain_test.cpp
    core/hook_registry_test.cpp
    core/constitutional_validator_test.cpp
    core/validation_hook_test.cpp
    core/response_generator_test.cpp
    core/message_queue_test.cpp  # gh#40 (v2.1.10)
    # v2.3.10: backstop coverage for streaming <think> tag filter
    core/stream_think_filter_test.cpp
)

target_include_directories(entropic-core-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
)

target_link_libraries(entropic-core-tests PRIVATE
    entropic-core
    entropic-types
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.8.5: MCP tool system tests
# v1.8.7: Added external client, reconnect, discovery tests
add_executable(entropic-mcp-tests
    mcp/tool_registry_test.cpp
    mcp/server_base_test.cpp
    mcp/permission_manager_test.cpp
    mcp/tool_executor_test.cpp
    mcp/server_manager_test.cpp
    mcp/tool_calling_loop_test.cpp
    # Per-server behavioral tests
    mcp/servers/filesystem_test.cpp
    mcp/servers/ignore_matcher_test.cpp
    mcp/servers/bash_test.cpp
    mcp/servers/git_test.cpp
    mcp/servers/diagnostics_test.cpp
    mcp/servers/web_test.cpp
    mcp/servers/entropic_test.cpp
    # v1.8.7: External MCP tests
    mcp/external_client_test.cpp
    mcp/reconnect_policy_test.cpp
    mcp/mcp_json_discovery_test.cpp
    # v2.3.10: SSE transport coverage
    mcp/transport_sse_test.cpp
    # v1.9.4: MCP authorization tests
    mcp/mcp_key_set_test.cpp
    mcp/mcp_authorization_test.cpp
    mcp/tool_executor_authorization_test.cpp
    # v1.9.12: Diagnose + self-inspection
    mcp/tool_call_history_test.cpp
    mcp/servers/diagnose_test.cpp
    mcp/servers/inspect_test.cpp
    # v2.0.6-rc16: Context window inspection (P2-16)
    mcp/servers/context_inspect_test.cpp
    # v2.1.0 #47: UTF-8 sanitizer for inbound tool-result strings
    mcp/utf8_sanitize_test.cpp
    # v2.1.0 #44: tool-result content classifiers (empty/error)
    mcp/tool_result_classify_test.cpp
    # v2.1.5 gh#19: StdioTransport display_name sanitization
    mcp/stdio_transport_test.cpp
)

target_include_directories(entropic-mcp-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Pass source-tree data dir for tool JSON resolution in tests
target_compile_definitions(entropic-mcp-tests PRIVATE
    TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/data"
)

target_link_libraries(entropic-mcp-tests PRIVATE
    entropic-mcp
    entropic-core
    entropic-types
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.8.8: Storage tests
if(ENTROPIC_STORAGE_SQLITE)
    add_executable(entropic-storage-tests
        storage/database_test.cpp
        storage/backend_test.cpp
        storage/c_interface_test.cpp
        storage/permission_persister_test.cpp
        storage/audit_entry_test.cpp
        storage/audit_logger_test.cpp
    )

    target_link_libraries(entropic-storage-tests PRIVATE
        entropic-storage
        entropic-types
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        Catch2::Catch2WithMain
    )

    # database_test.cpp uses sqlite3 API directly for stmt binding
    target_link_libraries(entropic-storage-tests PRIVATE SQLite::SQLite3)
endif()

# v1.8.9: C API stabilization tests
add_executable(entropic-api-tests
    api/api_version_test.cpp
    api/identity_api_test.cpp
    api/hook_stub_test.cpp
    api/error_enum_test.cpp
    api/facade_integration_test.cpp
    # v2.1.1 (#3): UTF-8 sanitization at the C-API outbound boundary.
    api/serialize_messages_utf8_test.cpp
    # v2.1.10 (gh#40): mid-gen user-message queue ABI surface.
    api/message_queue_abi_test.cpp
    # v2.2.3 (gh#56): sp_get_history must truncate on UTF-8 codepoint
    # boundaries — direct unit test of the facade-local helper.
    api/utf8_safe_substr_test.cpp
    # v2.2.5 (gh#58): two-handles-in-one-process probe + re-entry guard.
    api/multi_handle_test.cpp
    # v2.3.8: facade coverage gap — JSON serializers private header.
    api/json_serializers_test.cpp
    # v2.3.8: facade coverage gap — MCP C API NULL/INVALID_STATE branches.
    api/entropic_mcp_test.cpp
    # v2.3.10: facade coverage gap — entropic.cpp NULL/unconfigured-handle
    # branches across the full C API surface.
    api/entropic_capi_test.cpp
    # v2.0.11: External bridge tests (links facade for ENTROPIC_EXPORT symbols)
    mcp/external_bridge_test.cpp
    # v2.1.7 (gh#34): mcp-bridge CLI relay round-trip tests.
    mcp/mcp_bridge_relay_test.cpp
)

# v2.1.1 (#3): serialize_messages_utf8_test.cpp #includes the private
# json_serializers.h header from src/facade.
target_include_directories(entropic-api-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/facade
)

# v2.1.2 (#4): api_version_test.cpp reads the repo-root VERSION file at
# test time to verify that runtime entropic_version() matches the
# canonical declared version. Inject the absolute path here so the test
# stays portable across out-of-tree builds.
target_compile_definitions(entropic-api-tests PRIVATE
    ENTROPIC_VERSION_FILE_PATH="${PROJECT_SOURCE_DIR}/VERSION"
    # v2.1.7 (gh#34): mcp-bridge relay round-trip test forks the entropic
    # binary and exchanges bytes through a fake unix-socket server. The
    # binary lives next to the test in the build tree.
    ENTROPIC_CLI_BIN_PATH="$<TARGET_FILE:entropic>"
)
add_dependencies(entropic-api-tests entropic)

target_link_libraries(entropic-api-tests PRIVATE
    entropic-facade
    entropic-core
    entropic-types
    spdlog::spdlog
    Catch2::Catch2WithMain
)

# v1.8.9: C99 header compilation test (pure C, not C++)
add_executable(entropic-c99-test api/header_c99_test.c)
target_link_libraries(entropic-c99-test PRIVATE entropic-facade entropic-types)
set_target_properties(entropic-c99-test PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)
# C99 test just needs to compile and link — run it as a test
add_test(NAME entropic-c99-header-test COMMAND entropic-c99-test)

# v1.9.6: Identity manager tests (cross-library: core + inference + mcp).
# v2.0.5: sublibs are OBJECT libraries now — link them in directly
# rather than compiling individual .cpp files (which would collide with
# the OBJECT libs' own copies when both get absorbed).
add_executable(entropic-identity-tests
    core/identity_manager_test.cpp
    core/identity_validation_test.cpp
)

target_include_directories(entropic-identity-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/inference
    ${PROJECT_SOURCE_DIR}/src/mcp
)

target_link_libraries(entropic-identity-tests PRIVATE
    entropic-core
    entropic-types
    entropic-mcp
    entropic::inference
    llama
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Catch2::Catch2WithMain
)

include(CTest)
include(Catch)
catch_discover_tests(entropic-tests)
catch_discover_tests(entropic-config-tests)
catch_discover_tests(entropic-inference-tests)
catch_discover_tests(entropic-adapter-tests)
catch_discover_tests(entropic-speculative-compat-tests)
catch_discover_tests(entropic-secondary-loader-tests)
catch_discover_tests(entropic-grammar-tests)
catch_discover_tests(entropic-cap-tests)
catch_discover_tests(entropic-core-tests)
catch_discover_tests(entropic-mcp-tests)
catch_discover_tests(entropic-api-tests)
catch_discover_tests(entropic-identity-tests)
if(ENTROPIC_STORAGE_SQLITE)
    catch_discover_tests(entropic-storage-tests)
endif()
