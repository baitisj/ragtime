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
ragtime_test(ragtime_sha256_test Sha256Test.cpp)
ragtime_test(ragtime_ham_text_test HamTextTest.cpp)
target_compile_definitions(ragtime_ham_text_test PRIVATE RAGTIME_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
ragtime_test(ragtime_data2g_link_test Data2GLinkTest.cpp)
ragtime_test(ragtime_protocol_test ProtocolTest.cpp)
ragtime_test(ragtime_host_link_test HostLinkTest.cpp)
ragtime_test(ragtime_session_files_test SessionFilesTest.cpp)

# Two stations on loopback sockets to fake data2g-hosts (POSIX sockets).
if(UNIX)
    ragtime_test(ragtime_fake_host_test FakeHostTest.cpp FakeHost.cpp)
endif()

ragtime_test(ragtime_gearbox_test GearboxTest.cpp)

ragtime_test(ragtime_globus_test GlobusTest.cpp)
# The globus test also checks the generated map tables themselves.
target_include_directories(ragtime_globus_test PRIVATE ${PROJECT_SOURCE_DIR}/core/src/globus)
