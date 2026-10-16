# Tests for the plug-in (configure with -DNINJAM_VST3_TESTS=ON), run with ctest:
#
#   plugin_unit_test   saved state, port mapping messages, following the host's tempo and bars,
#                      the metronome's click, the editor's texts and BPM dragging; Ableton Link:
#                      the beat, phase and tempo logic, one instance leading Link in real time
#   plugin_clipboard   (Linux, with xvfb-run) the Copy buttons' text, read back the way a
#                      program that pastes reads it
#   plugin_e2e         test_plugin_e2e.py: plugin_e2e_host loads Jamsa.vst3, which hosts a
#                      server; a second client joins; audio both ways in Opus and FLAC.
#                      Also two instances in one process, playing together (one leads Link), a
#                      host whose tempo the room follows and whose bars the intervals start on,
#                      and a host that is a Link peer (as Live with its Link on), which the room's
#                      tempo and intervals lead
#   live_aoo_test      (ninjam/live) AOO's lock-free queue and RT memory pool under threads,
#                      and the AOO bugs fixed in our copy
#   live_fuzz_test     (ninjam/live) hostile and mangled packets into a running AOO source
#                      and sinks
#   live_rt_test       (ninjam/live) no heap allocation in AOO's process(), clean network
#                      or not

add_executable(plugin_unit_test
    plugin_unit_test.cpp
    ../source/click.cpp
    ../source/engine.cpp
    ../source/hostserver.cpp
    ../source/hostsync.cpp
    ../source/linksync.cpp
    ../source/textkeys.cpp
    ../source/uitext.cpp
)
target_compile_features(plugin_unit_test PRIVATE cxx_std_17)
target_link_libraries(plugin_unit_test PRIVATE ninjam_client ninjamsrv_embedded Ableton::Link)
add_test(NAME plugin_unit_test COMMAND plugin_unit_test)
# Link apps on one machine see each other: the tests that use Link run one at a time
set_tests_properties(plugin_unit_test PROPERTIES RESOURCE_LOCK ableton_link)
if(SMTG_LINUX)
    target_sources(plugin_unit_test PRIVATE ../source/clipboard.cpp)
    target_link_libraries(plugin_unit_test PRIVATE xcb)
    # an X server of its own, so it runs on a machine without a display too
    find_program(XVFB_RUN xvfb-run)
    if(XVFB_RUN)
        add_test(NAME plugin_clipboard COMMAND ${XVFB_RUN} -a $<TARGET_FILE:plugin_unit_test> clipboard)
    endif()
endif()

# Live mode's tests, defined in ninjam/live/CMakeLists.txt
ninjam_live_add_tests()

if(NOT WIN32)
    # POSIX only (it reads its stdin with select)
    if(SMTG_MAC)
        set(module_loader ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_mac.mm)
    else()
        set(module_loader ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_linux.cpp)
    endif()
    add_executable(plugin_e2e_host plugin_e2e_host.cpp ${module_loader})
    target_compile_features(plugin_e2e_host PRIVATE cxx_std_17)
    target_link_libraries(plugin_e2e_host PRIVATE sdk_hosting ninjam_client Ableton::Link ${CMAKE_DL_LIBS})
    if(SMTG_MAC)
        set_source_files_properties(${module_loader} PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        find_library(COCOA_FRAMEWORK Cocoa)
        target_link_libraries(plugin_e2e_host PRIVATE ${COCOA_FRAMEWORK})
    endif()
    add_dependencies(plugin_e2e_host Jamsa)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME plugin_e2e
            COMMAND ${Python3_EXECUTABLE} -m unittest -v test_plugin_e2e
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(plugin_e2e PROPERTIES
            TIMEOUT 900
            RESOURCE_LOCK ableton_link
            ENVIRONMENT "NINJAM_VST3_PLUGIN=$<TARGET_PROPERTY:Jamsa,SMTG_PLUGIN_PACKAGE_PATH>;NINJAM_VST3_E2E_HOST=$<TARGET_FILE:plugin_e2e_host>"
        )
    endif()
endif()
