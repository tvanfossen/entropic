# ── Inference library ────────────────────────────────────────
# Build variant based on compute backend. Links against llama.cpp,
# nlohmann/json, and spdlog.
#
# v1.8.2: Initial inference backend implementation.

set(INFERENCE_SOURCES
    backend.cpp
    llama_cpp_backend.cpp
    prompt_cache.cpp
    orchestrator.cpp
    adapter_manager.cpp
    grammar_registry.cpp
    profile_registry.cpp
    throughput_tracker.cpp
    inference_c_api.cpp
    interface_factory.cpp
    image_preprocessor.cpp
    adapters/adapter_base.cpp
    adapters/generic_adapter.cpp
    adapters/qwen35_adapter.cpp
    adapters/adapter_registry.cpp
)

# Determine library name based on compute backend
if(ENTROPIC_HAS_CUDA)
    set(INFERENCE_LIB_NAME entropic-inference-cuda)
    set(INFERENCE_BACKEND_DEF ENTROPIC_INFERENCE_CUDA)
elseif(ENTROPIC_VULKAN)
    set(INFERENCE_LIB_NAME entropic-inference-vulkan)
    set(INFERENCE_BACKEND_DEF ENTROPIC_INFERENCE_VULKAN)
else()
    set(INFERENCE_LIB_NAME entropic-inference-cpu)
    set(INFERENCE_BACKEND_DEF ENTROPIC_INFERENCE_CPU)
endif()

add_library(${INFERENCE_LIB_NAME} ${INFERENCE_SOURCES})

target_include_directories(${INFERENCE_LIB_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/extern/llama.cpp/vendor  # stb_image.h for image preprocessing
)

target_link_libraries(${INFERENCE_LIB_NAME}
    PUBLIC
        entropic-types
    PRIVATE
        llama
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)

target_compile_definitions(${INFERENCE_LIB_NAME}
    PRIVATE
        ENTROPIC_BUILDING_DLL
        ${INFERENCE_BACKEND_DEF}
)

set_target_properties(${INFERENCE_LIB_NAME} PROPERTIES
    OUTPUT_NAME "rentropic-inference-${INFERENCE_LIB_NAME}"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)

# Create an alias for tests and other targets to link against
add_library(entropic::inference ALIAS ${INFERENCE_LIB_NAME})
