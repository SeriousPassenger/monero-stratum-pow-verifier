if(NOT DEFINED MSPV_BUILD_DIR OR NOT DEFINED MSPV_SOURCE_DIR OR
   NOT DEFINED MSPV_WORK_DIR)
    message(FATAL_ERROR "install-consumer test is missing required paths")
endif()

set(prefix "${MSPV_WORK_DIR}/prefix")
set(build "${MSPV_WORK_DIR}/build")
file(REMOVE_RECURSE "${MSPV_WORK_DIR}")
file(MAKE_DIRECTORY "${MSPV_WORK_DIR}")

set(config_args)
if(DEFINED MSPV_CONFIG AND NOT MSPV_CONFIG STREQUAL "")
    list(APPEND config_args --config "${MSPV_CONFIG}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${MSPV_BUILD_DIR}"
            --prefix "${prefix}" ${config_args}
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "staged MSPV installation failed: ${result}")
endif()

if(EXISTS "${prefix}/include/randomx.h")
    message(FATAL_ERROR "private RandomX header leaked into MSPV installation")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${MSPV_SOURCE_DIR}" -B "${build}"
            -DCMAKE_PREFIX_PATH=${prefix}
            -DCMAKE_BUILD_TYPE=${MSPV_CONFIG}
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "installed-consumer configure failed: ${result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${build}" ${config_args}
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "installed-consumer build failed: ${result}")
endif()

set(executable_suffix "")
if(WIN32)
    set(executable_suffix ".exe")
endif()
foreach(name consumer_c consumer_cpp)
    set(path "${build}/${name}${executable_suffix}")
    if(NOT EXISTS "${path}" AND DEFINED MSPV_CONFIG)
        set(path "${build}/${MSPV_CONFIG}/${name}${executable_suffix}")
    endif()
    execute_process(COMMAND "${path}" RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "installed ${name} failed: ${result}")
    endif()
endforeach()
