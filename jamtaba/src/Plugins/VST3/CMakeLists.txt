# JamTaba as a VST3 plugin (JamTaba.vst3), built on the jamtaba_core library.
#
# The VST3 SDK (MIT licensed since 3.8) is downloaded at configure time. To use a
# local copy instead, pass -DFETCHCONTENT_SOURCE_DIR_VST3SDK=/path/to/vst3sdk.

include(FetchContent)

set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES ON CACHE BOOL "" FORCE) # builds the validator
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE) # don't symlink the build into the user's VST3 folder

FetchContent_Declare(vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG v3.8.1_build_84
    GIT_SHALLOW TRUE
    GIT_SUBMODULES base cmake pluginterfaces public.sdk
)
FetchContent_MakeAvailable(vst3sdk)
smtg_enable_vst3_sdk()

set(JAMTABA_VST3_VERSION "2.2.0")

smtg_add_vst3plugin(JamTaba
    PACKAGE_NAME "JamTaba"
    SOURCES_LIST
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
        ${vst3sdk_SOURCE_DIR}/public.sdk/source/common/pluginview.cpp
        factory.cpp
        JamTabaVST3.cpp
        JamTabaVST3.h
        JamTabaVST3View.cpp
        JamTabaVST3View.h
        MainControllerVST3.cpp
        MainControllerVST3.h
        QtPluginApp.cpp
        QtPluginApp.h
)

set_target_properties(JamTaba PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_compile_features(JamTaba PRIVATE cxx_std_17)
target_compile_definitions(JamTaba PRIVATE JAMTABA_VST3_VERSION="${JAMTABA_VST3_VERSION}")
target_link_libraries(JamTaba PRIVATE sdk jamtaba_core)

if(SMTG_MAC)
    smtg_target_set_bundle(JamTaba
        BUNDLE_IDENTIFIER com.github.leonidstern.jamtaba.vst3
        COMPANY_NAME "JamTaba"
    )
endif()
