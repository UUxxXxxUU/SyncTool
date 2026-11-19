cmake_minimum_required(VERSION 3.16)
project(SyncToolCrashReporter LANGUAGES C CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if (NOT EXISTS "${CRASHPAD_SOURCE_DIR}/client/crashpad_client.h")
    message(FATAL_ERROR "Set CRASHPAD_SOURCE_DIR to the pinned Crashpad checkout.")
endif()

add_subdirectory("${CRASHPAD_SOURCE_DIR}" crashpad EXCLUDE_FROM_ALL)
add_library(SyncToolCrashReporter SHARED reporter.cpp)
target_compile_features(SyncToolCrashReporter PRIVATE cxx_std_17)
target_compile_options(SyncToolCrashReporter PRIVATE /utf-8)
target_link_libraries(SyncToolCrashReporter PRIVATE crashpad::client crashpad::interface Advapi32)
add_dependencies(SyncToolCrashReporter crashpad_handler crashpad_wer)
add_custom_command(TARGET SyncToolCrashReporter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:crashpad_handler> $<TARGET_FILE:crashpad_wer>
        $<TARGET_FILE_DIR:SyncToolCrashReporter>)

option(SYNC_BUILD_CRASH_TEST "Build a process that deliberately crashes to verify dump capture" OFF)
if (SYNC_BUILD_CRASH_TEST)
    add_executable(SyncToolCrashProbe ../../tests/crash_probe.cpp)
    target_link_libraries(SyncToolCrashProbe PRIVATE SyncToolCrashReporter)
endif()
