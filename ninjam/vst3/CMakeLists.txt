# Jamsa: a NINJAM client as a VST3 plug-in, with a built-in server, in one file.
#
#   cmake -S ninjam/vst3 -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --target Jamsa validator
#   build/bin/Release/validator build/VST3/Release/Jamsa.vst3
#
# Everything it needs is downloaded at configure time and linked statically: the
# VST3 SDK with VSTGUI (MIT / BSD), Ogg, Opus and FLAC (BSD), and for the built-in
# server miniupnpc and libnatpmp (BSD). To use local copies, pass
# -DFETCHCONTENT_SOURCE_DIR_<NAME>=/path for VST3SDK (with its vstgui4 folder),
# OGG, OPUS, FLAC, MINIUPNP or LIBNATPMP.
#
# The plug-in sends and plays Opus and FLAC only (NINJAM_NO_VORBIS): it sends
# nothing in a room that uses Vorbis, and plays Vorbis intervals as silence.
#
# On Windows the plug-in is a single Jamsa.vst3 file; build with /MT
# (-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
# -DSMTG_USE_STATIC_CRT=ON) so it needs no Visual C++ runtime.
#
# NINJAM_VST3_TESTS=ON also builds the tests in tests/, and NINJAM_VST3_EDITORHOST=ON
# the SDK's editorhost, which shows the editor in a window (on Linux it needs gtkmm-3.0).

cmake_minimum_required(VERSION 3.25)

project(ninjam_vst3 VERSION 0.1.0 LANGUAGES C CXX)

# VSTGUI's macOS sources are Objective-C++, and smtg_enable_vst3_sdk() below adds some
# of them to targets in this directory, so the languages are enabled here (enabling them
# only inside the SDK's folder leaves this one without the rules to compile them).
if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

option(NINJAM_VST3_TESTS "Build the plug-in's tests" OFF)
option(NINJAM_VST3_EDITORHOST "Build the SDK's editorhost and other hosting examples" OFF)

set(NINJAM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(WDL_DIR ${NINJAM_DIR}/../WDL)

include(FetchContent)

# ---------------------------------------------------------------------------
# VST3 SDK with VSTGUI
# ---------------------------------------------------------------------------
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES ${NINJAM_VST3_EDITORHOST} CACHE BOOL "" FORCE) # the validator is built anyway
set(SMTG_ENABLE_VSTGUI_SUPPORT ON CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE) # don't symlink the build into the user's VST3 folder
set(SMTG_CREATE_BUNDLE_FOR_WINDOWS OFF CACHE BOOL "" FORCE) # on Windows Jamsa.vst3 is a single file
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE) # CI runs the validator as its own step

FetchContent_Declare(vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG v3.8.1_build_84
    GIT_SHALLOW TRUE
    GIT_SUBMODULES base cmake pluginterfaces public.sdk vstgui4
)
FetchContent_MakeAvailable(vst3sdk)
smtg_enable_vst3_sdk()

# ---------------------------------------------------------------------------
# Codecs: Ogg (for Opus), Opus, FLAC (static)
# ---------------------------------------------------------------------------
FetchContent_Declare(ogg
    GIT_REPOSITORY https://github.com/xiph/ogg.git
    GIT_TAG v1.3.5
    GIT_SHALLOW TRUE
)
FetchContent_Declare(opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
    GIT_SHALLOW TRUE
)
FetchContent_Declare(flac
    GIT_REPOSITORY https://github.com/xiph/flac.git
    GIT_TAG 1.4.3
    GIT_SHALLOW TRUE
)

# A function keeps these settings out of the rest of the build.
function(ninjam_fetch_codecs)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5) # ogg asks for CMake 2.8.12, which CMake 4 refuses
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW) # let option() see the normal variables below
    set(CMAKE_POLICY_DEFAULT_CMP0063 NEW) # hidden symbols in the static libraries too
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    set(BUILD_FRAMEWORK OFF)
    set(INSTALL_DOCS OFF)
    set(INSTALL_PKG_CONFIG_MODULE OFF)
    set(INSTALL_CMAKE_PACKAGE_MODULE OFF)

    FetchContent_MakeAvailable(ogg)

    set(OPUS_BUILD_SHARED_LIBRARY OFF)
    set(OPUS_BUILD_TESTING OFF)
    set(OPUS_BUILD_PROGRAMS OFF)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF)
    list(LENGTH CMAKE_OSX_ARCHITECTURES num_archs)
    if(num_archs GREATER 1)
        # a universal build compiles each file for both CPUs, so no CPU specific code
        set(OPUS_DISABLE_INTRINSICS ON)
    endif()
    FetchContent_MakeAvailable(opus)
    # opusencdec.h includes <opus/opus.h>, as with the system package
    file(COPY ${opus_SOURCE_DIR}/include/ DESTINATION ${CMAKE_BINARY_DIR}/opus-include/opus)

    set(BUILD_CXXLIBS OFF)
    set(BUILD_PROGRAMS OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_DOCS OFF)
    set(INSTALL_MANPAGES OFF)
    set(INSTALL_PKGCONFIG_MODULES OFF)
    set(INSTALL_CMAKE_CONFIG_MODULE OFF)
    set(WITH_OGG OFF) # NINJAM sends native FLAC
    set(WITH_STACK_PROTECTOR OFF)
    FetchContent_MakeAvailable(flac)
endfunction()

ninjam_fetch_codecs()

# ---------------------------------------------------------------------------
# The NINJAM server from this repository, run on a background thread when the
# user hosts a server (source/hostserver.cpp)
# ---------------------------------------------------------------------------
add_subdirectory(${NINJAM_DIR}/server ${CMAKE_CURRENT_BINARY_DIR}/ninjamsrv EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The NINJAM client (njclient) with Opus and FLAC, without Vorbis
# ---------------------------------------------------------------------------
add_library(ninjam_client STATIC
    ${NINJAM_DIR}/njclient.cpp
    ${NINJAM_DIR}/mpb.cpp
    ${NINJAM_DIR}/netmsg.cpp
    ${WDL_DIR}/jnetlib/asyncdns.cpp
    ${WDL_DIR}/jnetlib/connection.cpp
    ${WDL_DIR}/jnetlib/listen.cpp
    ${WDL_DIR}/jnetlib/util.cpp
    ${WDL_DIR}/rng.cpp
    ${WDL_DIR}/sha.cpp
    ${WDL_DIR}/resample.cpp
)
target_compile_features(ninjam_client PUBLIC cxx_std_17)
target_compile_definitions(ninjam_client PUBLIC NINJAM_OPUS NINJAM_FLAC NINJAM_NO_VORBIS NINJAM_VST3_VERSION="${PROJECT_VERSION}")
target_include_directories(ninjam_client PUBLIC ${NINJAM_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_BINARY_DIR}/opus-include)
target_link_libraries(ninjam_client PUBLIC opus ogg FLAC)
set_target_properties(ninjam_client PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    # njclient only opens files for features the plug-in doesn't use; plain fopen is enough
    target_compile_definitions(ninjam_client PUBLIC WDL_NO_SUPPORT_UTF8 _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS NOMINMAX)
    target_link_libraries(ninjam_client PUBLIC ws2_32)
elseif(APPLE)
    target_compile_definitions(ninjam_client PUBLIC _MAC)
endif()
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(ninjam_client PUBLIC Threads::Threads)
endif()
if(MSVC)
    target_compile_options(ninjam_client PRIVATE /wd4244 /wd4267 /wd4996)
endif()

# ---------------------------------------------------------------------------
# The plug-in
# ---------------------------------------------------------------------------
smtg_add_vst3plugin(Jamsa
    PACKAGE_NAME "Jamsa"
    SOURCES_LIST
        source/factory.cpp
        source/version.h
        source/pluginids.h
        source/plugin.cpp
        source/plugin.h
        source/engine.cpp
        source/engine.h
        source/click.cpp
        source/click.h
        source/hostserver.cpp
        source/hostserver.h
        source/hostsync.cpp
        source/hostsync.h
        source/editor.cpp
        source/editor.h
        source/views.cpp
        source/views.h
        source/uitext.cpp
        source/uitext.h
        source/clipboard.cpp
        source/clipboard.h
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/vstsinglecomponenteffect.h
)
target_compile_features(Jamsa PRIVATE cxx_std_17)
target_link_libraries(Jamsa PRIVATE sdk vstgui_support ninjam_client ninjamsrv_embedded)

if(SMTG_LINUX)
    # Export only the VST3 entry points, not the symbols of the libraries linked in,
    # so they can't clash with another plug-in's copies in the host.
    target_link_options(Jamsa PRIVATE "LINKER:--exclude-libs,ALL")
    # dlopen() keeps Pango loaded (see factory.cpp)
    target_link_libraries(Jamsa PRIVATE ${CMAKE_DL_LIBS})
    # the Copy buttons own X11's clipboard themselves (see clipboard.cpp); VSTGUI needs xcb too
    target_link_libraries(Jamsa PRIVATE xcb)
endif()

if(SMTG_MAC)
    smtg_target_set_bundle(Jamsa
        BUNDLE_IDENTIFIER io.github.leonidstern.jamsa.vst3
        COMPANY_NAME "LeonidStern/ninjam"
    )
    # Ninja and Makefile builds get CMake's default Info.plist, which calls the bundle an
    # application; this one says it is a plug-in bundle (BNDL), so Finder shows one item.
    set_target_properties(Jamsa PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/mac/Info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER io.github.leonidstern.jamsa.vst3
        MACOSX_BUNDLE_BUNDLE_NAME Jamsa
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_COPYRIGHT "NINJAM contributors, GPL v2 or later"
    )
endif()

if(NINJAM_VST3_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
