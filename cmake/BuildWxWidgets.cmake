# Fetches wxWidgets with git and builds it statically as part of Ragtime
# (-DRAGTIME_BUILD_WX=ON), for the Windows build, where there is no system
# wxWidgets. Sets RAGTIME_WX_LIBS to the targets ragtime links.
#
# Point RAGTIME_WX_SOURCE at a wxWidgets checkout (with its submodules) to
# build that instead of cloning one.

set(RAGTIME_WX_VERSION 3.2.9)
set(RAGTIME_WX_SOURCE "" CACHE PATH "A wxWidgets source tree to build instead of cloning v${RAGTIME_WX_VERSION}.")

set(wxBUILD_SHARED OFF CACHE BOOL "" FORCE)
set(wxBUILD_PRECOMP OFF CACHE STRING "" FORCE)
set(wxBUILD_MONOLITHIC OFF CACHE BOOL "" FORCE)
set(wxBUILD_SAMPLES OFF CACHE STRING "" FORCE)
set(wxBUILD_TESTS OFF CACHE STRING "" FORCE)
set(wxBUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(wxBUILD_INSTALL OFF CACHE BOOL "" FORCE)

# Everything third-party wx needs comes from its own tree.
set(wxUSE_REGEX builtin CACHE STRING "" FORCE)
set(wxUSE_ZLIB builtin CACHE STRING "" FORCE)
set(wxUSE_EXPAT builtin CACHE STRING "" FORCE)
set(wxUSE_LIBPNG builtin CACHE STRING "" FORCE)
set(wxUSE_LIBJPEG builtin CACHE STRING "" FORCE)
set(wxUSE_LIBTIFF builtin CACHE STRING "" FORCE)
set(wxUSE_NANOSVG OFF CACHE STRING "" FORCE)
set(wxUSE_LIBLZMA OFF CACHE STRING "" FORCE)
set(wxUSE_LIBSDL OFF CACHE STRING "" FORCE)
set(wxUSE_LIBMSPACK OFF CACHE STRING "" FORCE)
set(wxUSE_LIBICONV OFF CACHE STRING "" FORCE)
set(wxUSE_WEBVIEW OFF CACHE BOOL "" FORCE)
set(wxUSE_MEDIACTRL OFF CACHE BOOL "" FORCE)
set(wxUSE_STC OFF CACHE BOOL "" FORCE)
set(wxUSE_OPENGL OFF CACHE BOOL "" FORCE)
set(wxUSE_SOCKETS ON CACHE BOOL "" FORCE)
set(wxUSE_IPV6 ON CACHE BOOL "" FORCE)

if(RAGTIME_WX_SOURCE)
    add_subdirectory(${RAGTIME_WX_SOURCE} ${CMAKE_BINARY_DIR}/_deps/wxwidgets-build EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(wxWidgets
        GIT_REPOSITORY https://github.com/wxWidgets/wxWidgets.git
        GIT_TAG v${RAGTIME_WX_VERSION}
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
        UPDATE_DISCONNECTED TRUE)
    FetchContent_GetProperties(wxWidgets)
    if(NOT wxwidgets_POPULATED)
        FetchContent_Populate(wxWidgets)
        add_subdirectory(${wxwidgets_SOURCE_DIR} ${wxwidgets_BINARY_DIR} EXCLUDE_FROM_ALL)
    endif()
endif()

set(RAGTIME_WX_LIBS wx::html wx::net wx::core wx::base)
