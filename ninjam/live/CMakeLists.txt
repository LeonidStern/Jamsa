# Jamsa's Live mode: players hear each other in real time, through the NINJAM
# server they are connected to. Built on a trimmed copy of AOO (aoo/, see
# README.md) with Opus.
#
#   cmake -S ninjam/live -B build-live -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-live && ctest --test-dir build-live
#
# Targets:
#   ninjam_live      static library: AOO's source and sink with Opus, and oscpack
#   live_aoo_test    AOO's lock-free queue and RT memory pool under threads
#
# The plug-in's build (ninjam/vst3) adds this folder after it has downloaded
# Opus and registers the tests from vst3/tests. A build of this folder alone
# downloads Opus 1.5.2 itself (or pass -DFETCHCONTENT_SOURCE_DIR_OPUS=/path)
# and builds the tests unless NINJAM_LIVE_TESTS is OFF.

cmake_minimum_required(VERSION 3.24)
project(ninjam_live LANGUAGES C CXX)

set(NINJAM_LIVE_AOO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/aoo)

# ---------------------------------------------------------------------------
# Opus: the plug-in's build already has the 'opus' target
# ---------------------------------------------------------------------------
if(NOT TARGET opus)
    include(FetchContent)
    FetchContent_Declare(opus
        GIT_REPOSITORY https://github.com/xiph/opus.git
        GIT_TAG v1.5.2
        GIT_SHALLOW TRUE
    )
    # a function keeps these settings out of the rest of the build
    function(ninjam_live_fetch_opus)
        set(CMAKE_POLICY_DEFAULT_CMP0077 NEW) # let option() see the normal variables below
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)
        set(BUILD_SHARED_LIBS OFF)
        set(BUILD_TESTING OFF)
        set(OPUS_BUILD_SHARED_LIBRARY OFF)
        set(OPUS_BUILD_TESTING OFF)
        set(OPUS_BUILD_PROGRAMS OFF)
        set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF)
        set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF)
        list(LENGTH CMAKE_OSX_ARCHITECTURES num_archs)
        if(num_archs GREATER 1)
            set(OPUS_DISABLE_INTRINSICS ON)
        endif()
        FetchContent_MakeAvailable(opus)
    endfunction()
    ninjam_live_fetch_opus()
endif()

# ---------------------------------------------------------------------------
# What the compiler offers. Without lock-free 64-bit and double atomics AOO
# falls back to one global spinlock for them (common/sync.hpp), which the
# audio thread would then share with every other thread.
# ---------------------------------------------------------------------------
include(CheckCXXSourceCompiles)
function(ninjam_live_check_atomics)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_REQUIRED_QUIET ON)
    check_cxx_source_compiles("
        #include <atomic>
        static_assert(std::atomic<double>::is_always_lock_free, \"\");
        int main() { std::atomic<double> d{0}; d.store(1); return d.load() > 2; }"
        NINJAM_LIVE_ATOMIC_DOUBLE)
    check_cxx_source_compiles("
        #include <atomic>
        #include <cstdint>
        static_assert(std::atomic<std::int64_t>::is_always_lock_free, \"\");
        int main() { std::atomic<std::int64_t> i{0}; return (int)i.fetch_add(1); }"
        NINJAM_LIVE_ATOMIC_INT64)
    # the tagged words of the lock-free queues and the RT memory pool; some
    # compilers turn their compare-exchange into calls into libatomic
    set(source "
        #include <atomic>
        #include <cstdint>
        struct alignas(8) tagged { std::uint32_t bits; std::uint32_t tag; };
        std::atomic<tagged> t;
        std::atomic<std::uint64_t> u;
        int main() {
            tagged e = t.load();
            std::uint64_t v = u.load();
            return t.compare_exchange_weak(e, tagged{1, 1}) + u.compare_exchange_weak(v, 2);
        }")
    check_cxx_source_compiles("${source}" NINJAM_LIVE_ATOMIC_LINKS)
    if(NOT NINJAM_LIVE_ATOMIC_LINKS)
        set(CMAKE_REQUIRED_LIBRARIES atomic)
        check_cxx_source_compiles("${source}" NINJAM_LIVE_ATOMIC_NEEDS_LIBATOMIC)
    endif()
endfunction()
ninjam_live_check_atomics()

# ---------------------------------------------------------------------------
# ninjam_live
# ---------------------------------------------------------------------------
set(NINJAM_LIVE_AOO_SOURCES
    ${NINJAM_LIVE_AOO_DIR}/src/aoo.cpp
    ${NINJAM_LIVE_AOO_DIR}/src/source.cpp
    ${NINJAM_LIVE_AOO_DIR}/src/sink.cpp
    ${NINJAM_LIVE_AOO_DIR}/src/packet_buffer.cpp
    ${NINJAM_LIVE_AOO_DIR}/src/data_frame.cpp
    ${NINJAM_LIVE_AOO_DIR}/src/resampler.cpp
    ${NINJAM_LIVE_AOO_DIR}/src/codec/opus.cpp
    ${NINJAM_LIVE_AOO_DIR}/common/sync.cpp
    ${NINJAM_LIVE_AOO_DIR}/common/time.cpp
    ${NINJAM_LIVE_AOO_DIR}/common/net_utils.cpp
    ${NINJAM_LIVE_AOO_DIR}/deps/oscpack/osc/OscOutboundPacketStream.cpp
    ${NINJAM_LIVE_AOO_DIR}/deps/oscpack/osc/OscReceivedElements.cpp
    ${NINJAM_LIVE_AOO_DIR}/deps/oscpack/osc/OscTypes.cpp
)

# What AOO's own sources (and tests that use AOO's internals) need on top of
# the public headers: its private headers and how they are configured.
add_library(ninjam_live_aoo_internal INTERFACE)
target_include_directories(ninjam_live_aoo_internal INTERFACE
    ${NINJAM_LIVE_AOO_DIR}
    ${NINJAM_LIVE_AOO_DIR}/src
    ${NINJAM_LIVE_AOO_DIR}/deps/oscpack
)
target_compile_definitions(ninjam_live_aoo_internal INTERFACE
    AOO_LOG_LEVEL=kAooLogLevelError
    $<$<BOOL:${NINJAM_LIVE_ATOMIC_DOUBLE}>:AOO_HAVE_ATOMIC_DOUBLE>
    $<$<BOOL:${NINJAM_LIVE_ATOMIC_INT64}>:AOO_HAVE_ATOMIC_INT64>
    $<$<NOT:$<PLATFORM_ID:Windows>>:AOO_HAVE_PTHREAD_RWLOCK>
)

add_library(ninjam_live STATIC ${NINJAM_LIVE_AOO_SOURCES})
target_compile_features(ninjam_live PUBLIC cxx_std_17)
target_include_directories(ninjam_live PUBLIC ${NINJAM_LIVE_AOO_DIR}/include)
# the public headers' layout depends on these, so whoever includes them gets them too
target_compile_definitions(ninjam_live PUBLIC AOO_NET=0 AOO_USE_OPUS=1 AOO_SAMPLE_SIZE=32)
target_link_libraries(ninjam_live PRIVATE ninjam_live_aoo_internal)
target_link_libraries(ninjam_live PUBLIC opus)
set_target_properties(ninjam_live PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    target_compile_definitions(ninjam_live PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS NOMINMAX)
    target_link_libraries(ninjam_live PUBLIC ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(ninjam_live PUBLIC Threads::Threads)
endif()
if(NOT NINJAM_LIVE_ATOMIC_LINKS AND NINJAM_LIVE_ATOMIC_NEEDS_LIBATOMIC)
    target_link_libraries(ninjam_live PUBLIC atomic)
endif()

if(MSVC)
    # AOO checks __cplusplus, which MSVC only sets right with /Zc:__cplusplus
    target_compile_options(ninjam_live PRIVATE /W3 /Zc:__cplusplus)
    # AOO's known noise: conversions, signed/unsigned, unused locals, sscanf
    set_source_files_properties(${NINJAM_LIVE_AOO_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/wd4244;/wd4267;/wd4018;/wd4101;/wd4996;/wd4305")
else()
    target_compile_options(ninjam_live PRIVATE -Wall -Wextra)
    # AOO's known noise (its own build turns these off too), and oscpack's
    # missing 'override', which the VST3 SDK's -Wsuggest-override points out
    set(aoo_quiet -Wno-sign-compare -Wno-unused-variable -Wno-unused-parameter)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-Wsuggest-override NINJAM_LIVE_HAVE_WSUGGEST_OVERRIDE)
    if(NINJAM_LIVE_HAVE_WSUGGEST_OVERRIDE)
        list(APPEND aoo_quiet -Wno-suggest-override)
    endif()
    set_source_files_properties(${NINJAM_LIVE_AOO_SOURCES} PROPERTIES COMPILE_OPTIONS "${aoo_quiet}")
endif()

# ---------------------------------------------------------------------------
# Tests. ninjam/vst3/tests calls this after enable_testing(), because tests
# added in a folder added before enable_testing() are silently left out.
# ---------------------------------------------------------------------------
function(ninjam_live_add_tests)
    set(dir ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tests)
    foreach(name live_aoo_test)
        add_executable(${name} ${dir}/${name}.cpp)
        target_compile_features(${name} PRIVATE cxx_std_17)
        target_link_libraries(${name} PRIVATE ninjam_live ninjam_live_aoo_internal)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES TIMEOUT 300)
    endforeach()
endfunction()

if(PROJECT_IS_TOP_LEVEL)
    option(NINJAM_LIVE_TESTS "Build the tests" ON)
    if(NINJAM_LIVE_TESTS)
        enable_testing()
        ninjam_live_add_tests()
    endif()
endif()
