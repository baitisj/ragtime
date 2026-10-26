# Fetches Hamlib with git and builds it as a static library
# (-DRAGTIME_BUILD_HAMLIB=ON), for the Windows build, where there is no
# system Hamlib. Defines ragtime::hamlib, which the rig library links, and
# hamlib_build, which it waits for.
#
# Hamlib builds with autotools, so this needs autoconf, automake and
# libtool besides the cross compiler. Point RAGTIME_HAMLIB_SOURCE at a
# Hamlib checkout to build that instead of cloning one; the build happens
# in a copy, so the checkout stays clean.

set(RAGTIME_HAMLIB_VERSION 4.5.5)
set(RAGTIME_HAMLIB_SOURCE "" CACHE PATH "A Hamlib source tree to build instead of cloning ${RAGTIME_HAMLIB_VERSION}.")

include(ExternalProject)

if(CMAKE_CROSSCOMPILING)
    # x86_64-w64-mingw32-gcc-posix: the triple is the compiler's name up to "-gcc".
    get_filename_component(compiler ${CMAKE_C_COMPILER} NAME)
    string(REGEX REPLACE "-gcc.*$" "" triple "${compiler}")
    set(host --host=${triple})
endif()

set(prefix ${CMAKE_BINARY_DIR}/_deps/hamlib-install)
# Only the library: no C++ binding, USB, readline, XML or sound file
# support, which the utilities and some backends would pull in.
set(configure <SOURCE_DIR>/configure ${host} --prefix=${prefix} --disable-shared --enable-static
    --without-cxx-binding --without-libusb --without-readline --without-xml-support --without-indi
    --without-libsndfile --disable-html-matrix --disable-winradio
    CC=${CMAKE_C_COMPILER} "CFLAGS=-O2")

if(RAGTIME_HAMLIB_SOURCE)
    set(source URL ${RAGTIME_HAMLIB_SOURCE})
else()
    set(source GIT_REPOSITORY https://github.com/Hamlib/Hamlib.git GIT_TAG ${RAGTIME_HAMLIB_VERSION} GIT_SHALLOW TRUE)
endif()

ExternalProject_Add(hamlib_build
    ${source}
    PREFIX ${CMAKE_BINARY_DIR}/_deps/hamlib
    UPDATE_DISCONNECTED TRUE
    BUILD_IN_SOURCE TRUE
    CONFIGURE_COMMAND ./bootstrap COMMAND ${configure}
    BUILD_COMMAND make -j4
    INSTALL_COMMAND make install
    BUILD_BYPRODUCTS ${prefix}/lib/libhamlib.a
    LOG_CONFIGURE TRUE
    LOG_BUILD TRUE
    LOG_INSTALL TRUE
    LOG_OUTPUT_ON_FAILURE TRUE)

# The include folder must exist when the target is defined.
file(MAKE_DIRECTORY ${prefix}/include)
add_library(ragtime::hamlib STATIC IMPORTED GLOBAL)
set_target_properties(ragtime::hamlib PROPERTIES
    IMPORTED_LOCATION ${prefix}/lib/libhamlib.a
    INTERFACE_INCLUDE_DIRECTORIES ${prefix}/include)
if(WIN32)
    set_property(TARGET ragtime::hamlib PROPERTY INTERFACE_LINK_LIBRARIES ws2_32)
endif()
