# ── librentropic-storage.so ──────────────────────────────
# v1.8.8: SQLite conversation persistence, session log files,
# delegation record storage, permission persistence.
#
# Links: librentropic-types, sqlite3, spdlog, nlohmann/json
# Implements: IStorageBackend interface contract (C boundary)

if(NOT ENTROPIC_STORAGE_SQLITE)
    return()
endif()

add_library(entropic-storage STATIC
    database.cpp
    backend.cpp
    permission_persister.cpp
    c_interface.cpp
    audit_entry.cpp
    audit_logger.cpp
)

target_include_directories(entropic-storage PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}/include
)

target_link_libraries(entropic-storage
    PUBLIC
        entropic-types
    PRIVATE
        SQLite::SQLite3
        spdlog::spdlog
        nlohmann_json::nlohmann_json
)


# Storage is NOT a plugin boundary — use default visibility so tests
# can link directly (same pattern as core, design decision #22).
set_target_properties(entropic-storage PROPERTIES
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)

target_compile_definitions(entropic-storage PRIVATE
    ENTROPIC_BUILDING_DLL
)
