# The Ragtime desktop app: wxWidgets on top of ragtime_core. Keep logic in
# core where it can be tested without a display; the app draws and wires.

find_package(wxWidgets 3.2 REQUIRED COMPONENTS html net core base)

add_executable(ragtime WIN32
    src/RagtimeApp.cpp
    src/MainFrame.cpp
    src/BroadcastPage.cpp
    src/ConnectionBar.cpp
    src/GearshiftDialog.cpp
    src/HostConnection.cpp
    src/Html.cpp
    src/Prefs.cpp
    src/SessionPage.cpp
    src/SettingsDialog.cpp
    src/StationList.cpp
    src/Thresholds.cpp
    src/TxLamp.cpp
    src/globus/GlobusPanel.cpp
    src/globus/GlobusToggle.cpp
    src/globus/GlobusView.cpp
)

# wx headers go in as system headers, so a warning in wxWidgets itself never
# fails a -Werror build of our code.
target_include_directories(ragtime SYSTEM PRIVATE ${wxWidgets_INCLUDE_DIRS})
target_compile_definitions(ragtime PRIVATE ${wxWidgets_DEFINITIONS})
if(wxWidgets_CXX_FLAGS)
    separate_arguments(RAGTIME_WX_CXX_FLAGS UNIX_COMMAND "${wxWidgets_CXX_FLAGS}")
    target_compile_options(ragtime PRIVATE ${RAGTIME_WX_CXX_FLAGS})
endif()
target_link_libraries(ragtime PRIVATE ragtime_core ${wxWidgets_LIBRARIES})
ragtime_warnings(ragtime)

if(RAGTIME_TESTS AND NOT WIN32)
    # Runs without a display: --version answers before the toolkit starts.
    add_test(NAME ragtime_app_version COMMAND ragtime --version)
    set_tests_properties(ragtime_app_version PROPERTIES
        PASS_REGULAR_EXPRESSION "^Ragtime ${PROJECT_VERSION}")
endif()
