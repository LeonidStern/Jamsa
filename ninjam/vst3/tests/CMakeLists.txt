# Tests for the plug-in (configure with -DNINJAM_VST3_TESTS=ON), run with ctest:
#
#   plugin_unit_test   saved state, port mapping messages, following the host's tempo and bars,
#                      the metronome's click
#   plugin_e2e         test_plugin_e2e.py: plugin_e2e_host loads NINJAM.vst3, which hosts a
#                      server; a second client joins; audio both ways in Opus and FLAC.
#                      Also two instances in one process, playing together, and a host whose
#                      tempo the room follows and whose bars the intervals start on

add_executable(plugin_unit_test
    plugin_unit_test.cpp
    ../source/click.cpp
    ../source/engine.cpp
    ../source/hostserver.cpp
    ../source/hostsync.cpp
)
target_compile_features(plugin_unit_test PRIVATE cxx_std_17)
target_link_libraries(plugin_unit_test PRIVATE ninjam_client ninjamsrv_embedded)
add_test(NAME plugin_unit_test COMMAND plugin_unit_test)

if(NOT WIN32)
    # POSIX only (it reads its stdin with select)
    if(SMTG_MAC)
        set(module_loader ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_mac.mm)
    else()
        set(module_loader ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_linux.cpp)
    endif()
    add_executable(plugin_e2e_host plugin_e2e_host.cpp ${module_loader})
    target_compile_features(plugin_e2e_host PRIVATE cxx_std_17)
    target_link_libraries(plugin_e2e_host PRIVATE sdk_hosting ninjam_client ${CMAKE_DL_LIBS})
    if(SMTG_MAC)
        set_source_files_properties(${module_loader} PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        find_library(COCOA_FRAMEWORK Cocoa)
        target_link_libraries(plugin_e2e_host PRIVATE ${COCOA_FRAMEWORK})
    endif()
    add_dependencies(plugin_e2e_host NINJAM)

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME plugin_e2e
            COMMAND ${Python3_EXECUTABLE} -m unittest -v test_plugin_e2e
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(plugin_e2e PROPERTIES
            TIMEOUT 600
            ENVIRONMENT "NINJAM_VST3_PLUGIN=$<TARGET_PROPERTY:NINJAM,SMTG_PLUGIN_PACKAGE_PATH>;NINJAM_VST3_E2E_HOST=$<TARGET_FILE:plugin_e2e_host>"
        )
    endif()
endif()
