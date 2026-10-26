# ragtime_audio: the Ear listening alongside data2g-host, through PipeWire
# (EarAudio.h). libpipewire is loaded at run time, never linked, so only its
# headers (libpipewire-0.3-dev) are needed to build, and Ragtime runs where
# there is no PipeWire. Elsewhere (Windows, -DRAGTIME_PIPEWIRE=OFF) a
# stand-in says the Ear needs it. The Ear's spectrum and graph search are
# core's, and tested there.

if(RAGTIME_PIPEWIRE)
    add_library(ragtime_audio STATIC src/PipeWireEar.cpp)
    # Headers only: SYSTEM, so a warning in PipeWire's own never fails -Werror.
    target_include_directories(ragtime_audio SYSTEM PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
    target_link_libraries(ragtime_audio PRIVATE ${CMAKE_DL_LIBS})
else()
    add_library(ragtime_audio STATIC src/NoEar.cpp)
endif()

find_package(Threads REQUIRED)
target_link_libraries(ragtime_audio PUBLIC ragtime_core Threads::Threads)
target_include_directories(ragtime_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
ragtime_warnings(ragtime_audio)
