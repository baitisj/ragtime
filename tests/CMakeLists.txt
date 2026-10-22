# Unit tests for ragtime_core. Each test is a plain executable built on
# Check.h; it prints every failed check and exits non-zero if any failed.
#
#   ragtime_test(<name> <source>... [ARGS <arg>...])

function(ragtime_test name)
    cmake_parse_arguments(T "" "" "ARGS" ${ARGN})
    add_executable(${name} ${T_UNPARSED_ARGUMENTS} Check.h)
    target_link_libraries(${name} PRIVATE ragtime_core)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    ragtime_warnings(${name})
    add_test(NAME ${name} COMMAND ${name} ${T_ARGS})
endfunction()

ragtime_test(ragtime_version_test VersionTest.cpp)
