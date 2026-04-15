# entropic CLI binary — top-level dispatcher with mcp-bridge subcommand.
#
# Ships with the engine. Provides the `entropic mcp-bridge` integration
# point that lets external MCP clients (Claude Code, VSCode, etc.) use
# the engine as a tool provider with zero consumer code.

add_executable(entropic
    main.cpp
    mcp_bridge.cpp
    version.cpp
)

target_include_directories(entropic
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_BINARY_DIR}/include
)

target_link_libraries(entropic
    PRIVATE
        entropic-facade
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)

set_target_properties(entropic PROPERTIES
    OUTPUT_NAME "entropic"
)
