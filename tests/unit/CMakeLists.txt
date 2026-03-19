add_executable(entropic-tests
    test_error.cpp
    test_config_structs.cpp
    test_logging.cpp
)

target_link_libraries(entropic-tests PRIVATE
    entropic-types
    Catch2::Catch2WithMain
)

include(CTest)
include(Catch)
catch_discover_tests(entropic-tests)
