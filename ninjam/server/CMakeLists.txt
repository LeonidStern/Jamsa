# CMake build for ninjamsrv, used for Windows and for the copy of the server that
# ships inside the JamTaba plugin. The Makefile in this folder still works on
# Linux and macOS.
#
#   cmake -S ninjam/server -B build-server && cmake --build build-server
#
# NINJAM_PORTMAP (on by default) builds in router port mapping. miniupnpc and
# libnatpmp are then downloaded and linked statically, so the binary has no
# extra runtime dependencies.

cmake_minimum_required(VERSION 3.24)
project(ninjamsrv LANGUAGES C CXX)

option(NINJAM_PORTMAP "Map the server port on the router with UPnP-IGD or NAT-PMP" ON)

set(WDL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../WDL)

add_executable(ninjamsrv
    ninjamsrv.cpp
    usercon.cpp
    portmap.cpp
    ../mpb.cpp
    ../netmsg.cpp
    ${WDL_DIR}/jnetlib/asyncdns.cpp
    ${WDL_DIR}/jnetlib/connection.cpp
    ${WDL_DIR}/jnetlib/listen.cpp
    ${WDL_DIR}/jnetlib/util.cpp
    ${WDL_DIR}/jnetlib/httpget.cpp
    ${WDL_DIR}/rng.cpp
    ${WDL_DIR}/sha.cpp
)
target_compile_features(ninjamsrv PRIVATE cxx_std_11)

if(APPLE)
    target_compile_definitions(ninjamsrv PRIVATE _MAC)
endif()

if(WIN32)
    target_compile_definitions(ninjamsrv PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS NOMINMAX)
    target_link_libraries(ninjamsrv PRIVATE ws2_32)
    if(MINGW)
        target_link_options(ninjamsrv PRIVATE -static)
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(ninjamsrv PRIVATE Threads::Threads)
endif()

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

    # the server includes <miniupnpc/miniupnpc.h>, as with the system package
    file(COPY ${miniupnp_SOURCE_DIR}/miniupnpc/include/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/portmap-include/miniupnpc)
    target_include_directories(ninjamsrv PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/portmap-include)

    target_compile_definitions(ninjamsrv PRIVATE NINJAM_PORTMAP)
    target_link_libraries(ninjamsrv PRIVATE libminiupnpc-static natpmp)
endif()
