# ragtime_image: the picture codecs for inline pictures (Codec.h). JPEG 2000
# both ways through OpenJPEG, and WebP (libwebp) and HEIF (libheif, which
# decodes HEVC through libde265) read from disk; JPEG and PNG come through
# wxImage in the app. Kept out of ragtime_core, which links no codecs; the
# downscaler and the airtime sums are core's, and tested there. With
# -DRAGTIME_PICTURES=OFF a stand-in says pictures can't be made or shown,
# and with -DRAGTIME_HEIF=OFF HEIF files can't be picked.

if(RAGTIME_PICTURES)
    add_library(ragtime_image STATIC src/Codec.cpp)
    target_link_libraries(ragtime_image PRIVATE ragtime::openjpeg ragtime::webp)
    if(RAGTIME_HEIF)
        target_link_libraries(ragtime_image PRIVATE ragtime::heif)
        target_compile_definitions(ragtime_image PRIVATE RAGTIME_HEIF=1)
    else()
        target_compile_definitions(ragtime_image PRIVATE RAGTIME_HEIF=0)
    endif()
    if(TARGET image_libs_build)
        add_dependencies(ragtime_image image_libs_build)
    endif()
else()
    add_library(ragtime_image STATIC src/NoCodec.cpp)
endif()

target_link_libraries(ragtime_image PUBLIC ragtime_core PRIVATE ${CMAKE_DL_LIBS})
target_include_directories(ragtime_image PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
ragtime_warnings(ragtime_image)
