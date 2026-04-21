# SPDX-License-Identifier: LGPL-3.0-or-later
# entropic CLI binary — top-level dispatcher with mcp-bridge subcommand.
#
# Ships with the engine. Provides the `entropic mcp-bridge` integration
# point that lets external MCP clients (Claude Code, VSCode, etc.) use
# the engine as a tool provider with zero consumer code.

add_executable(entropic
    main.cpp
    mcp_bridge.cpp
    mcp_connect.cpp
    version.cpp
    download.cpp
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
    # v2.0.5: relative RPATH so bin/entropic finds ../lib/librentropic.so.2
    # from any install prefix without LD_LIBRARY_PATH. $ORIGIN is resolved
    # by ld.so at load time to the directory containing the executable.
    INSTALL_RPATH "$ORIGIN/../lib"
    # Don't auto-populate the install RPATH from link paths (would bake
    # build-tree paths into the distributed binary).
    INSTALL_RPATH_USE_LINK_PATH FALSE
)

# v2.0.5: ship the CLI in the distribution.
include(GNUInstallDirs)
install(TARGETS entropic
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
