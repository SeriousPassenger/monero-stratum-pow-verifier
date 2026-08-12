cmake_minimum_required(VERSION 3.16)

project(monero-stratum-pow-verifier
        VERSION 0.1.0
        DESCRIPTION "Threaded RandomX proof-of-work verification library"
        LANGUAGES C CXX)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(MSPV_STANDALONE_DEFAULT ON)
else()
    set(MSPV_STANDALONE_DEFAULT OFF)
endif()

option(MSPV_BUILD_TESTS "Build verifier tests" ${MSPV_STANDALONE_DEFAULT})
option(MSPV_BUILD_EXAMPLES "Build verifier examples" ${MSPV_STANDALONE_DEFAULT})
option(MSPV_BUILD_BENCHMARK "Build the public-API benchmark"
       ${MSPV_STANDALONE_DEFAULT})
option(MSPV_BUILD_FAST_TESTS
       "Build test using a 2080 MiB dataset (~2336 MiB preparation peak)" OFF)
option(MSPV_ENABLE_TRACE_LOGGING
       "Compile high-volume per-job TRACE diagnostics" OFF)
option(MSPV_ENABLE_SANITIZERS
       "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/RandomX/CMakeLists.txt")
    message(FATAL_ERROR
        "RandomX submodule is missing. Run: "
        "git submodule update --init --recursive")
endif()

set(MSPV_RANDOMX_REVISION
    "6c4340ba4561aec9a3611c1aedf9931239777fb3")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/RandomX/.git")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/RandomX"
            OUTPUT_VARIABLE _mspv_randomx_revision
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT _mspv_randomx_revision STREQUAL MSPV_RANDOMX_REVISION)
            message(FATAL_ERROR
                "RandomX must be checked out at ${MSPV_RANDOMX_REVISION}; "
                "found ${_mspv_randomx_revision}")
        endif()
        unset(_mspv_randomx_revision)
    endif()
endif()

find_package(Threads REQUIRED)
include(GNUInstallDirs)

# RandomX leaves its library type to BUILD_SHARED_LIBS. Keep this dependency
# static without changing a parent project's value outside this directory.
if(DEFINED BUILD_SHARED_LIBS)
    set(_mspv_build_shared_libs_was_defined TRUE)
    set(_mspv_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
else()
    set(_mspv_build_shared_libs_was_defined FALSE)
endif()
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(third_party/RandomX EXCLUDE_FROM_ALL)
# The exported dependency is link-only; keep RandomX's header out of the
# verifier package so the installed public surface remains the MSPV C header.
set_property(TARGET randomx PROPERTY PUBLIC_HEADER "")
if(_mspv_build_shared_libs_was_defined)
    set(BUILD_SHARED_LIBS "${_mspv_saved_build_shared_libs}")
else()
    unset(BUILD_SHARED_LIBS)
endif()
unset(_mspv_saved_build_shared_libs)
unset(_mspv_build_shared_libs_was_defined)

add_library(monero_stratum_pow_verifier STATIC
    src/monero_stratum_pow_verifier.cpp)
add_library(mspv::verifier ALIAS monero_stratum_pow_verifier)
set_target_properties(monero_stratum_pow_verifier PROPERTIES
    EXPORT_NAME verifier
    OUTPUT_NAME monero_stratum_pow_verifier
    POSITION_INDEPENDENT_CODE ON)

target_compile_features(monero_stratum_pow_verifier PRIVATE cxx_std_17)
target_include_directories(monero_stratum_pow_verifier
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/RandomX/src)
target_link_libraries(monero_stratum_pow_verifier
    PRIVATE randomx Threads::Threads)

if(MSPV_ENABLE_TRACE_LOGGING)
    target_compile_definitions(monero_stratum_pow_verifier
        PRIVATE MSPV_ENABLE_TRACE_LOGGING=1)
endif()

if(UNIX AND NOT APPLE AND
   CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # RandomX v1.2.2's x86 assembly omits a GNU-stack note. The verifier never
    # needs an executable process stack, even when RandomX JIT is enabled.
    target_link_options(monero_stratum_pow_verifier
        INTERFACE "LINKER:-z,noexecstack")
endif()

function(mspv_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
            -Wshadow -Wformat=2)
    endif()
endfunction()

mspv_enable_warnings(monero_stratum_pow_verifier)

if(MSPV_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(monero_stratum_pow_verifier PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(monero_stratum_pow_verifier PUBLIC
        -fsanitize=address,undefined)
endif()

if(MSPV_BUILD_EXAMPLES)
    add_executable(mspv_verify examples/verify.c)
    target_compile_features(mspv_verify PRIVATE c_std_99)
    set_target_properties(mspv_verify PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(mspv_verify PRIVATE mspv::verifier)
endif()

if(MSPV_BUILD_BENCHMARK)
    add_executable(mspv_benchmark tools/benchmark.cpp)
    target_compile_features(mspv_benchmark PRIVATE cxx_std_17)
    target_link_libraries(mspv_benchmark PRIVATE mspv::verifier)
    mspv_enable_warnings(mspv_benchmark)
endif()

if(MSPV_BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests)
endif()

include(CMakePackageConfigHelpers)

install(TARGETS monero_stratum_pow_verifier randomx
        EXPORT mspv-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/monero_stratum_pow_verifier.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/monero-stratum-pow-verifier)
install(FILES third_party/RandomX/LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/monero-stratum-pow-verifier
        RENAME RANDOMX-LICENSE)

configure_package_config_file(
    cmake/monero-stratum-pow-verifier-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/monero-stratum-pow-verifier-config.cmake"
    INSTALL_DESTINATION
        "${CMAKE_INSTALL_LIBDIR}/cmake/monero-stratum-pow-verifier")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/monero-stratum-pow-verifier-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY ExactVersion)

install(EXPORT mspv-targets
        FILE monero-stratum-pow-verifier-targets.cmake
        NAMESPACE mspv::
        DESTINATION
            "${CMAKE_INSTALL_LIBDIR}/cmake/monero-stratum-pow-verifier")
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/monero-stratum-pow-verifier-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/monero-stratum-pow-verifier-config-version.cmake"
        DESTINATION
            "${CMAKE_INSTALL_LIBDIR}/cmake/monero-stratum-pow-verifier")
