add_executable(mspv_tests mspv_tests.cpp)
target_compile_features(mspv_tests PRIVATE cxx_std_17)
target_link_libraries(mspv_tests PRIVATE mspv::verifier Threads::Threads)
add_test(NAME mspv-light COMMAND mspv_tests)
set_tests_properties(mspv-light PROPERTIES TIMEOUT 180 LABELS "unit;light")

add_executable(mspv_c_api_test mspv_c_api_test.c)
target_compile_features(mspv_c_api_test PRIVATE c_std_99)
set_target_properties(mspv_c_api_test PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(mspv_c_api_test PRIVATE mspv::verifier)
add_test(NAME mspv-c-api COMMAND mspv_c_api_test)
set_tests_properties(mspv-c-api PROPERTIES
    TIMEOUT 180 LABELS "unit;c-api")

if(TARGET mspv_benchmark)
    add_test(NAME mspv-benchmark-help COMMAND mspv_benchmark --help)
    set_tests_properties(mspv-benchmark-help PROPERTIES LABELS "unit;cli")
    add_test(NAME mspv-benchmark-smoke
        COMMAND mspv_benchmark
            --mode light
            --workers 1
            --inflight 1
            --warmup 0
            --seconds 1
            --timeout 120
            --large-pages off)
    set_tests_properties(mspv-benchmark-smoke PROPERTIES
        TIMEOUT 180 LABELS "integration;benchmark;light")
endif()

add_test(NAME mspv-install-consumer
    COMMAND "${CMAKE_COMMAND}"
        -DMSPV_BUILD_DIR=${CMAKE_BINARY_DIR}
        -DMSPV_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/install-consumer
        -DMSPV_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/install-consumer-work
        -DMSPV_CONFIG=$<CONFIG>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/install_consumer_test.cmake)
set_tests_properties(mspv-install-consumer PROPERTIES
    TIMEOUT 180 LABELS "integration;install;c-api")

if(MSPV_BUILD_FAST_TESTS)
    add_executable(mspv_fast_test mspv_fast_test.cpp)
    target_compile_features(mspv_fast_test PRIVATE cxx_std_17)
    target_link_libraries(mspv_fast_test PRIVATE mspv::verifier)
    add_test(NAME mspv-fast COMMAND mspv_fast_test)
    set_tests_properties(mspv-fast PROPERTIES
        TIMEOUT 600 LABELS "integration;fast;high-memory")
endif()
