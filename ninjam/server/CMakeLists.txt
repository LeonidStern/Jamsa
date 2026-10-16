# CMake build for ninjamsrv, used for Windows and for the server that is built
# into the JamTaba plugin. The Makefile in this folder still works on Linux and
# macOS.
#
#   cmake -S ninjam/server -B build-server && cmake --build build-server
#
# Targets:
#   ninjamsrv                   the command line server
#   ninjamsrv_embedded          static library: the server on a background thread
#                               inside another program (ninjamsrv_embedded.h)
#   ninjamsrv_embedded_runner   test program that runs the embedded server like
#                               the command line one (NINJAM_SERVER_TESTS=ON)
#
# NINJAM_PORTMAP (on by default) builds in router port mapping. miniupnpc and
# libnatpmp are then downloaded and linked statically, so the binary has no
# extra runtime dependencies.

cmake_minimum_required(VERSION 3.24)
project(ninjamsrv LANGUAGES C CXX)

option(NINJAM_PORTMAP "Map the server port on the router with UPnP-IGD or NAT-PMP" ON)
option(NINJAM_SERVER_TESTS "Build the test runner for the embedded server" OFF)

set(WDL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../WDL)

set(NINJAMSRV_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/ninjamsrv.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/usercon.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/portmap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/liverelay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../mpb.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../netmsg.cpp
    ${WDL_DIR}/jnetlib/asyncdns.cpp
    ${WDL_DIR}/jnetlib/connection.cpp
    ${WDL_DIR}/jnetlib/listen.cpp
    ${WDL_DIR}/jnetlib/util.cpp
    ${WDL_DIR}/jnetlib/httpget.cpp
    ${WDL_DIR}/rng.cpp
    ${WDL_DIR}/sha.cpp
)

if(NINJAM_PORTMAP)
    include(FetchContent)

    set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
    set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(miniupnp
        GIT_REPOSITORY https://github.com/miniupnp/miniupnp.git
        GIT_TAG miniupnpc_2_2_6
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR miniupnpc
        EXCLUDE_FROM_ALL
    )
    FetchContent_Declare(libnatpmp
        GIT_REPOSITORY https://github.com/miniupnp/libnatpmp.git
        GIT_TAG 134fc89e2781e154e40042641f4d8bcbe42579f1
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(miniupnp libnatpmp)
    set_target_properties(libminiupnpc-static natpmp PROPERTIES POSITION_INDEPENDENT_CODE ON)

    # the server includes <miniupnpc/miniupnpc.h>, as with the system package
    file(COPY ${miniupnp_SOURCE_DIR}/miniupnpc/include/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/portmap-include/miniupnpc)

    # libnatpmp's folder holds a file named "version", which the C++ library's own
    # #include <version> would pick up (it does on macOS), so that folder stays off
    # the include path and its header is copied instead
    set_target_properties(natpmp PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "")
    file(COPY ${libnatpmp_SOURCE_DIR}/natpmp.h ${libnatpmp_SOURCE_DIR}/natpmp_declspec.h DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/portmap-include)
endif()

function(ninjamsrv_configure target)
    target_compile_features(${target} PRIVATE cxx_std_11)
    if(APPLE)
        target_compile_definitions(${target} PRIVATE _MAC)
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS NOMINMAX)
        target_link_libraries(${target} PUBLIC ws2_32 advapi32) # advapi32: the random source for Live keys
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(${target} PUBLIC Threads::Threads)
    endif()
    if(NINJAM_PORTMAP)
        target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/portmap-include)
        target_compile_definitions(${target} PRIVATE NINJAM_PORTMAP)
        target_link_libraries(${target} PUBLIC libminiupnpc-static natpmp)
    endif()
endfunction()

add_executable(ninjamsrv ${NINJAMSRV_SOURCES})
ninjamsrv_configure(ninjamsrv)
if(MINGW)
    target_link_options(ninjamsrv PRIVATE -static)
endif()

add_library(ninjamsrv_embedded STATIC EXCLUDE_FROM_ALL ${NINJAMSRV_SOURCES})
ninjamsrv_configure(ninjamsrv_embedded)
target_compile_definitions(ninjamsrv_embedded PRIVATE NINJAM_EMBEDDED)
target_include_directories(ninjamsrv_embedded INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(ninjamsrv_embedded PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NINJAM_SERVER_TESTS)
    add_executable(ninjamsrv_embedded_runner tests/embedded_runner.cpp)
    target_link_libraries(ninjamsrv_embedded_runner PRIVATE ninjamsrv_embedded)
    target_compile_features(ninjamsrv_embedded_runner PRIVATE cxx_std_11)
endif()
