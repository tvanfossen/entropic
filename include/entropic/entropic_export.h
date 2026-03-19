/**
 * @file entropic_export.h
 * @brief Symbol visibility macro for all exported symbols.
 *
 * Applied ONLY to:
 * - Factory functions (entropic_create_server, entropic_create_inference_backend)
 * - C API functions in entropic.h
 * - entropic_plugin_api_version()
 *
 * All other symbols are hidden by default (CMAKE_CXX_VISIBILITY_PRESET=hidden).
 *
 * @version 1.8.0
 */

#pragma once

#if defined(ENTROPIC_STATIC_BUILD)
    /* Static build — no import/export decoration */
    #define ENTROPIC_EXPORT
#elif defined(_WIN32)
    #if defined(ENTROPIC_BUILDING_DLL)
        #define ENTROPIC_EXPORT __declspec(dllexport)
    #else
        #define ENTROPIC_EXPORT __declspec(dllimport)
    #endif
#else
    #define ENTROPIC_EXPORT __attribute__((visibility("default")))
#endif
