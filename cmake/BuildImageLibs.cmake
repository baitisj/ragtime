# Fetches OpenJPEG, libwebp, libde265 and libheif with git and builds them
# as static libraries (-DRAGTIME_BUILD_IMAGE_LIBS=ON), for the Windows
# build, where there are none to find. Defines ragtime::openjpeg,
# ragtime::webp and (with RAGTIME_HEIF) ragtime::heif, which the image
# library links, and image_libs_build, which it waits for.
#
# Only the decoders and encoder Ragtime uses are built: OpenJPEG without
# its tools, libwebp's decoder, libde265's decoder library, and libheif
# with libde265 for HEVC and nothing else (no x265: it is GPL; no AV1).
# Licences: OpenJPEG BSD-2-Clause, libwebp BSD-3-Clause, libde265 and
# libheif LGPL-3.0. packaging/windows/make-zip.sh puts each one's licence
# in the zip; Ragtime's own source is public, so the exe can be rebuilt
# with changed copies of the LGPL libraries, as the LGPL asks.

set(RAGTIME_OPENJPEG_VERSION v2.5.3)
set(RAGTIME_WEBP_VERSION v1.4.0)
set(RAGTIME_DE265_VERSION v1.0.15)
set(RAGTIME_HEIF_VERSION v1.18.2)

include(ExternalProject)

set(prefix ${CMAKE_BINARY_DIR}/_deps/image-install)
set(common
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=${prefix}
    -DCMAKE_PREFIX_PATH=${prefix}
    -DBUILD_SHARED_LIBS=OFF
    -DBUILD_TESTING=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
if(CMAKE_TOOLCHAIN_FILE)
    list(APPEND common -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        "-DCMAKE_FIND_ROOT_PATH=${CMAKE_FIND_ROOT_PATH}|${prefix}")
endif()

ExternalProject_Add(openjpeg_build
    GIT_REPOSITORY https://github.com/uclouvain/openjpeg.git GIT_TAG ${RAGTIME_OPENJPEG_VERSION} GIT_SHALLOW TRUE
    PREFIX ${CMAKE_BINARY_DIR}/_deps/openjpeg
    UPDATE_DISCONNECTED TRUE
    LIST_SEPARATOR |
    CMAKE_ARGS ${common} -DBUILD_CODEC=OFF -DBUILD_DOC=OFF -DBUILD_JPIP=OFF -DBUILD_JPWL=OFF
    BUILD_BYPRODUCTS ${prefix}/lib/libopenjp2.a
    LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE LOG_OUTPUT_ON_FAILURE TRUE)

ExternalProject_Add(webp_build
    GIT_REPOSITORY https://github.com/webmproject/libwebp.git GIT_TAG ${RAGTIME_WEBP_VERSION} GIT_SHALLOW TRUE
    PREFIX ${CMAKE_BINARY_DIR}/_deps/webp
    UPDATE_DISCONNECTED TRUE
    LIST_SEPARATOR |
    CMAKE_ARGS ${common} -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF
        -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF
        -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DWEBP_BUILD_LIBWEBPMUX=OFF
    BUILD_BYPRODUCTS ${prefix}/lib/libwebp.a ${prefix}/lib/libsharpyuv.a
    LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE LOG_OUTPUT_ON_FAILURE TRUE)

set(builds openjpeg_build webp_build)

if(RAGTIME_HEIF)
    ExternalProject_Add(de265_build
        GIT_REPOSITORY https://github.com/strukturag/libde265.git GIT_TAG ${RAGTIME_DE265_VERSION} GIT_SHALLOW TRUE
        PREFIX ${CMAKE_BINARY_DIR}/_deps/de265
        UPDATE_DISCONNECTED TRUE
        LIST_SEPARATOR |
        CMAKE_ARGS ${common} -DENABLE_SDL=OFF -DENABLE_DECODER=OFF -DENABLE_ENCODER=OFF
            "-DCMAKE_CXX_FLAGS=-DLIBDE265_STATIC_BUILD" "-DCMAKE_C_FLAGS=-DLIBDE265_STATIC_BUILD"
        BUILD_BYPRODUCTS ${prefix}/lib/libde265.a
        LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE LOG_OUTPUT_ON_FAILURE TRUE)

    # Everything but the HEVC decoder off, so libheif needs nothing more.
    ExternalProject_Add(heif_build
        GIT_REPOSITORY https://github.com/strukturag/libheif.git GIT_TAG ${RAGTIME_HEIF_VERSION} GIT_SHALLOW TRUE
        PREFIX ${CMAKE_BINARY_DIR}/_deps/heif
        UPDATE_DISCONNECTED TRUE
        LIST_SEPARATOR |
        DEPENDS de265_build
        CMAKE_ARGS ${common} -DWITH_LIBDE265=ON -DWITH_LIBDE265_PLUGIN=OFF -DENABLE_PLUGIN_LOADING=OFF
            -DLIBDE265_INCLUDE_DIR=${prefix}/include -DLIBDE265_LIBRARY=${prefix}/lib/libde265.a
            -DWITH_X265=OFF -DWITH_AOM_DECODER=OFF -DWITH_AOM_ENCODER=OFF -DWITH_DAV1D=OFF -DWITH_RAV1E=OFF
            -DWITH_SvtEnc=OFF -DWITH_KVAZAAR=OFF -DWITH_FFMPEG_DECODER=OFF -DWITH_JPEG_DECODER=OFF
            -DWITH_JPEG_ENCODER=OFF -DWITH_OpenJPEG_DECODER=OFF -DWITH_OpenJPEG_ENCODER=OFF
            -DWITH_OPENJPH_ENCODER=OFF -DWITH_UNCOMPRESSED_CODEC=OFF -DWITH_LIBSHARPYUV=OFF
            -DWITH_HEADER_COMPRESSION=OFF -DWITH_EXAMPLES=OFF -DWITH_GDK_PIXBUF=OFF -DBUILD_DOCUMENTATION=OFF
            -DWITH_FUZZERS=OFF
            "-DCMAKE_CXX_FLAGS=-DLIBDE265_STATIC_BUILD" "-DCMAKE_C_FLAGS=-DLIBDE265_STATIC_BUILD"
        BUILD_BYPRODUCTS ${prefix}/lib/libheif.a
        LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE LOG_OUTPUT_ON_FAILURE TRUE)
    list(APPEND builds de265_build heif_build)
endif()

add_custom_target(image_libs_build DEPENDS ${builds})

# The include folders must exist when the targets are defined.
file(MAKE_DIRECTORY ${prefix}/include ${prefix}/include/openjpeg-2.5)

add_library(ragtime::openjpeg STATIC IMPORTED GLOBAL)
set_target_properties(ragtime::openjpeg PROPERTIES
    IMPORTED_LOCATION ${prefix}/lib/libopenjp2.a
    INTERFACE_INCLUDE_DIRECTORIES ${prefix}/include/openjpeg-2.5
    INTERFACE_COMPILE_DEFINITIONS OPJ_STATIC)

add_library(ragtime_sharpyuv STATIC IMPORTED GLOBAL)
set_target_properties(ragtime_sharpyuv PROPERTIES IMPORTED_LOCATION ${prefix}/lib/libsharpyuv.a)
add_library(ragtime::webp STATIC IMPORTED GLOBAL)
set_target_properties(ragtime::webp PROPERTIES
    IMPORTED_LOCATION ${prefix}/lib/libwebp.a
    INTERFACE_INCLUDE_DIRECTORIES ${prefix}/include
    INTERFACE_LINK_LIBRARIES ragtime_sharpyuv)

if(RAGTIME_HEIF)
    add_library(ragtime_de265 STATIC IMPORTED GLOBAL)
    set_target_properties(ragtime_de265 PROPERTIES IMPORTED_LOCATION ${prefix}/lib/libde265.a)
    add_library(ragtime::heif STATIC IMPORTED GLOBAL)
    set_target_properties(ragtime::heif PROPERTIES
        IMPORTED_LOCATION ${prefix}/lib/libheif.a
        INTERFACE_INCLUDE_DIRECTORIES ${prefix}/include
        INTERFACE_COMPILE_DEFINITIONS "LIBHEIF_STATIC_BUILD;LIBDE265_STATIC_BUILD"
        INTERFACE_LINK_LIBRARIES ragtime_de265)
endif()
