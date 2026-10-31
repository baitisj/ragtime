# ragtime_audio: the Ear listening alongside data2g-host (EarAudio.h):
# through PipeWire on Linux, WASAPI on Windows. libpipewire is loaded at run
# time, never linked, so only its headers (libpipewire-0.3-dev) are needed
# to build, and Ragtime runs where there is no PipeWire. With
# -DRAGTIME_PIPEWIRE=OFF (and on the Mac) a stand-in says the Ear can't
# listen. The Ear's spectrum and graph search are core's, and tested there.

if(WIN32)
    add_library(ragtime_audio STATIC src/WasapiEar.cpp src/HostProcess.cpp)
    # COM and property variants (ole32), the TCP table (iphlpapi).
    target_link_libraries(ragtime_audio PRIVATE ole32 iphlpapi)
elseif(RAGTIME_PIPEWIRE)
    add_library(ragtime_audio STATIC src/PipeWireEar.cpp src/HostProcess.cpp)
    # Headers only: SYSTEM, so a warning in PipeWire's own never fails -Werror.
    target_include_directories(ragtime_audio SYSTEM PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
    target_link_libraries(ragtime_audio PRIVATE ${CMAKE_DL_LIBS})
else()
    add_library(ragtime_audio STATIC src/NoEar.cpp src/HostProcess.cpp)
endif()

find_package(Threads REQUIRED)
target_link_libraries(ragtime_audio PUBLIC ragtime_core Threads::Threads)
target_include_directories(ragtime_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
ragtime_warnings(ragtime_audio)

# libsotto, the voice codec, loaded at run time (ragtime/voice/Sotto.h):
# LGPL-2.1 through Codec 2, so never linked.
target_sources(ragtime_audio PRIVATE src/Sotto.cpp)
if(NOT WIN32)
    target_link_libraries(ragtime_audio PRIVATE ${CMAKE_DL_LIBS})
endif()
