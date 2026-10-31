# ragtime_denoise: noise reduction for the voice chain, RNNoise as a
# voice::Denoiser (RnNoise.h). Kept out of ragtime_core, which links no
# libraries. With -DRAGTIME_RNNOISE=OFF a stand-in makes none.
#
# RNNoise (Xiph.Org, BSD-3-Clause, its COPYING) is fetched with git at
# 0.1.1, the last release with its trained model in the repository: 0.2
# downloads its model from media.xiph.org at build time. Its seven C files
# are built here as a static library, without Ragtime's warning flags (it
# is not our code). For an offline build, point
# -DFETCHCONTENT_SOURCE_DIR_RNNOISE=<path> at a checkout of that tag.
if(RAGTIME_RNNOISE)
    include(FetchContent)
    FetchContent_Declare(rnnoise
        GIT_REPOSITORY https://github.com/xiph/rnnoise.git
        GIT_TAG 6cbfd53eb348a8d394e0757b4025c6ded34eb2b6  # v0.1.1
    )
    FetchContent_GetProperties(rnnoise)
    if(NOT rnnoise_POPULATED)
        # It has no CMakeLists.txt, so MakeAvailable only fetches it.
        FetchContent_MakeAvailable(rnnoise)
    endif()
    set(RAGTIME_RNNOISE_DIR ${rnnoise_SOURCE_DIR} PARENT_SCOPE)

    add_library(ragtime_rnnoise STATIC
        ${rnnoise_SOURCE_DIR}/src/denoise.c
        ${rnnoise_SOURCE_DIR}/src/rnn.c
        ${rnnoise_SOURCE_DIR}/src/rnn_data.c
        ${rnnoise_SOURCE_DIR}/src/rnn_reader.c
        ${rnnoise_SOURCE_DIR}/src/pitch.c
        ${rnnoise_SOURCE_DIR}/src/kiss_fft.c
        ${rnnoise_SOURCE_DIR}/src/celt_lpc.c
    )
    target_include_directories(ragtime_rnnoise PUBLIC ${rnnoise_SOURCE_DIR}/include)
    set_target_properties(ragtime_rnnoise PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
    if(NOT MSVC)
        target_compile_options(ragtime_rnnoise PRIVATE -w)
        find_library(RAGTIME_LIBM m)
        if(RAGTIME_LIBM)
            target_link_libraries(ragtime_rnnoise PRIVATE ${RAGTIME_LIBM})
        endif()
    endif()

    add_library(ragtime_denoise STATIC src/RnNoise.cpp)
    target_link_libraries(ragtime_denoise PRIVATE ragtime_rnnoise)
else()
    add_library(ragtime_denoise STATIC src/NoRnNoise.cpp)
endif()

target_link_libraries(ragtime_denoise PUBLIC ragtime_core)
target_include_directories(ragtime_denoise PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
ragtime_warnings(ragtime_denoise)
