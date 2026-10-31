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
ragtime_test(ragtime_parity_test ParityTest.cpp)
ragtime_test(ragtime_ham_text_test HamTextTest.cpp)
target_compile_definitions(ragtime_ham_text_test PRIVATE RAGTIME_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
ragtime_test(ragtime_data2g_link_test Data2GLinkTest.cpp)
ragtime_test(ragtime_protocol_test ProtocolTest.cpp)
ragtime_test(ragtime_host_link_test HostLinkTest.cpp)
ragtime_test(ragtime_session_files_test SessionFilesTest.cpp)

# Two stations on loopback sockets to fake data2g-hosts (POSIX sockets).
if(UNIX)
    ragtime_test(ragtime_fake_host_test FakeHostTest.cpp FakeHost.cpp)
    ragtime_test(ragtime_session_host_test SessionHostTest.cpp FakeHost.cpp)
    # A flight recording played back to a HostLink on loopback.
    ragtime_test(ragtime_replay_test ReplayTest.cpp ReplayHost.cpp FakeHost.cpp)

    # The bench: fake hosts on 8100/8300 and 8110/8310 and a few bot
    # stations, for trying the app without Data2G. Not a test.
    add_executable(ragtime_fake_air FakeAirMain.cpp FakeHost.cpp ReplayHost.cpp)
    target_link_libraries(ragtime_fake_air PRIVATE ragtime_core)
    ragtime_warnings(ragtime_fake_air)
endif()

ragtime_test(ragtime_gearbox_test GearboxTest.cpp)

ragtime_test(ragtime_broadcast_test BroadcastTest.cpp)
ragtime_test(ragtime_broadcast_log_test BroadcastLogTest.cpp)
ragtime_test(ragtime_watch_list_test WatchListTest.cpp)

ragtime_test(ragtime_session_test SessionTest.cpp)
ragtime_test(ragtime_stream_log_test StreamLogTest.cpp)
ragtime_test(ragtime_session_log_test SessionLogTest.cpp)

ragtime_test(ragtime_globus_test GlobusTest.cpp)
# The globus test also checks the generated map tables themselves.
target_include_directories(ragtime_globus_test PRIVATE ${PROJECT_SOURCE_DIR}/core/src/globus)

ragtime_test(ragtime_broadcast_files_test BroadcastFilesTest.cpp)
ragtime_test(ragtime_file_traffic_test FileTrafficTest.cpp)

# Inline pictures: the downscaler and airtime sums, and the codecs.
ragtime_test(ragtime_picture_test PictureTest.cpp)
target_link_libraries(ragtime_picture_test PRIVATE ragtime_image)
# Voice messages: the .sot file, the pitch strip and airtime, on
# tests/data/voice/.
ragtime_test(ragtime_voice_test VoiceTest.cpp)
target_compile_definitions(ragtime_voice_test PRIVATE RAGTIME_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
# Cool-chic pictures: the decoder against tests/data/coolchic/, and a fuzz run.
ragtime_test(ragtime_coolchic_test CoolChicTest.cpp)
target_link_libraries(ragtime_coolchic_test PRIVATE ragtime_image)
target_compile_definitions(ragtime_coolchic_test PRIVATE RAGTIME_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(TARGET ragtime::webp)
    # libwebp's encoder makes the WebP the test reads back.
    target_link_libraries(ragtime_picture_test PRIVATE ragtime::webp)
    target_compile_definitions(ragtime_picture_test PRIVATE RAGTIME_TEST_WEBP=1)
endif()

ragtime_test(ragtime_radio_test RadioTest.cpp)
ragtime_test(ragtime_recent_work_test RecentWorkTest.cpp)
ragtime_test(ragtime_rig_test RigTest.cpp)
# Hamlib's dummy rig (model 1), opened on the rig library's thread.
if(RAGTIME_HAMLIB)
    ragtime_test(ragtime_hamlib_test HamlibTest.cpp)
    target_link_libraries(ragtime_hamlib_test PRIVATE ragtime_rig)
endif()
ragtime_test(ragtime_flight_recorder_test FlightRecorderTest.cpp)
ragtime_test(ragtime_ear_test EarTest.cpp)
# The Ear without its window, for the bench and checking a station by hand
# (it only records). Not a test: it needs a running PipeWire.
add_executable(ragtime_ear_probe EarProbe.cpp)
target_link_libraries(ragtime_ear_probe PRIVATE ragtime_audio)
ragtime_warnings(ragtime_ear_probe)

# The Cool-chic encoder's backward passes against PyTorch and finite differences.
ragtime_test(ragtime_coolchic_fit_test CoolChicFitTest.cpp)
target_link_libraries(ragtime_coolchic_fit_test PRIVATE ragtime_image)
target_include_directories(ragtime_coolchic_fit_test PRIVATE ${PROJECT_SOURCE_DIR}/image/src/coolchic)
target_compile_definitions(ragtime_coolchic_fit_test PRIVATE RAGTIME_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
# The whole encoder on a small picture, through the decoder.
ragtime_test(ragtime_coolchic_encode_test CoolChicEncodeTest.cpp)
target_link_libraries(ragtime_coolchic_encode_test PRIVATE ragtime_image)
# A developer's tool beside it (tools/codecs/coolchic_fit_check.py).
add_executable(ragtime_coolchic_fit CoolChicFitTool.cpp)
target_link_libraries(ragtime_coolchic_fit PRIVATE ragtime_image)
target_include_directories(ragtime_coolchic_fit PRIVATE ${PROJECT_SOURCE_DIR}/image/src/coolchic)
ragtime_warnings(ragtime_coolchic_fit)
