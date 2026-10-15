cmake_minimum_required(VERSION 3.24)

project(jamtaba VERSION 2.1.16 LANGUAGES C CXX) # version as in src/Common/Configurator.h

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # jamtaba_core is linked into a shared plugin

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Concurrent WebSockets)

# ---------------------------------------------------------------------------
# Third party: Ogg, Vorbis, Opus and FLAC, fetched and built as static libraries
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(ogg
    GIT_REPOSITORY https://github.com/xiph/ogg.git
    GIT_TAG v1.3.5
    GIT_SHALLOW TRUE
)

FetchContent_Declare(vorbis
    GIT_REPOSITORY https://github.com/xiph/vorbis.git
    GIT_TAG v1.3.7
    GIT_SHALLOW TRUE
)

FetchContent_Declare(opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)

FetchContent_Declare(flac
    GIT_REPOSITORY https://github.com/xiph/flac.git
    GIT_TAG 1.5.0
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)

# A function keeps these settings out of the rest of the build (and out of src/Plugins/VST3).
function(jamtaba_fetch_xiph_libs)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5) # ogg/vorbis ask for CMake 2.8.12, which CMake 4 refuses
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW) # let option() see the normal variables below
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)
    set(BUILD_FRAMEWORK OFF)
    set(INSTALL_DOCS OFF)
    set(INSTALL_PKG_CONFIG_MODULE OFF)
    set(INSTALL_CMAKE_PACKAGE_MODULE OFF)

    FetchContent_MakeAvailable(ogg)

    # vorbis calls find_package(Ogg) with its own FindOgg.cmake; point it at the ogg target built above.
    set(OGG_INCLUDE_DIR "${ogg_SOURCE_DIR}/include" CACHE PATH "Ogg include dir (fetched)" FORCE)
    set(OGG_LIBRARY ogg CACHE STRING "Ogg library (fetched target)" FORCE)

    FetchContent_MakeAvailable(vorbis)

    set(OPUS_BUILD_PROGRAMS OFF)
    set(OPUS_BUILD_TESTING OFF)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF)
    # A universal macOS build compiles every file for arm64 and x86_64, so the SSE and NEON files can't be used.
    list(LENGTH CMAKE_OSX_ARCHITECTURES osx_architectures)
    if(osx_architectures GREATER 1)
        set(OPUS_DISABLE_INTRINSICS ON)
    endif()

    FetchContent_MakeAvailable(opus)

    # ninjam/opusencdec.h includes <opus/opus.h>, as with the system package
    file(COPY ${opus_SOURCE_DIR}/include/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/opus-include/opus)

    # only libFLAC; intervals are native FLAC, so no Ogg FLAC either
    set(BUILD_CXXLIBS OFF)
    set(BUILD_PROGRAMS OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_DOCS OFF)
    set(INSTALL_MANPAGES OFF)
    set(INSTALL_PKGCONFIG_MODULES OFF)
    set(INSTALL_CMAKE_CONFIG_MODULE OFF)
    set(WITH_OGG OFF)
    set(ENABLE_MULTITHREADING OFF) # JamTaba already encodes in its own thread

    FetchContent_MakeAvailable(flac)
endfunction()

jamtaba_fetch_xiph_libs()

# ---------------------------------------------------------------------------
# jamtaba_core
# ---------------------------------------------------------------------------
set(JT_COMMON ${CMAKE_CURRENT_SOURCE_DIR}/src/Common)
set(JT_PLUGINS ${CMAKE_CURRENT_SOURCE_DIR}/src/Plugins)
set(JT_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/resources)

set(JT_COMMON_SOURCES
    MainController.cpp MainController.h
    NinjamController.cpp NinjamController.h
    MetronomeUtils.cpp MetronomeUtils.h
    Configurator.cpp Configurator.h
    UploadIntervalData.cpp UploadIntervalData.h
    Utils.h

    midi/MidiDriver.cpp midi/MidiDriver.h
    midi/MidiMessage.cpp midi/MidiMessage.h

    looper/Looper.cpp looper/Looper.h
    looper/LooperLayer.cpp looper/LooperLayer.h
    looper/LooperStates.cpp looper/LooperStates.h
    looper/LooperPersistence.cpp looper/LooperPersistence.h

    audio/core/AudioDriver.cpp audio/core/AudioDriver.h
    audio/core/AudioNode.cpp audio/core/AudioNode.h
    audio/core/LocalInputNode.cpp audio/core/LocalInputNode.h
    audio/core/LocalInputGroup.cpp audio/core/LocalInputGroup.h
    audio/core/AudioNodeProcessor.cpp audio/core/AudioNodeProcessor.h
    audio/core/AudioMixer.cpp audio/core/AudioMixer.h
    audio/core/SamplesBuffer.cpp audio/core/SamplesBuffer.h
    audio/core/AudioPeak.cpp audio/core/AudioPeak.h
    audio/core/Plugins.cpp audio/core/Plugins.h
    audio/core/Filters.cpp audio/core/Filters.h
    audio/core/PluginDescriptor.cpp audio/core/PluginDescriptor.h
    audio/Encoder.h
    audio/Decoder.h
    audio/vorbis/Vorbis.h
    audio/vorbis/VorbisDecoder.cpp audio/vorbis/VorbisDecoder.h
    audio/vorbis/VorbisEncoder.cpp audio/vorbis/VorbisEncoder.h
    audio/opus/NJOpus.h
    audio/opus/OpusDecoder.cpp audio/opus/OpusDecoder.h
    audio/opus/OpusEncoder.cpp audio/opus/OpusEncoder.h
    audio/flac/FlacDecoder.cpp audio/flac/FlacDecoder.h
    audio/flac/FlacEncoder.cpp audio/flac/FlacEncoder.h
    audio/RoomStreamerNode.cpp audio/RoomStreamerNode.h
    audio/NinjamTrackNode.cpp audio/NinjamTrackNode.h
    audio/MetronomeTrackNode.cpp audio/MetronomeTrackNode.h
    audio/MidiSyncTrackNode.cpp audio/MidiSyncTrackNode.h
    audio/SamplesBufferResampler.cpp audio/SamplesBufferResampler.h
    audio/Mp3Decoder.cpp audio/Mp3Decoder.h
    audio/Resampler.cpp audio/Resampler.h

    file/FileReader.h
    file/FileReaderFactory.cpp file/FileReaderFactory.h
    file/WaveFileReader.cpp file/WaveFileReader.h
    file/WaveFileWriter.cpp file/WaveFileWriter.h
    file/OggFileReader.cpp file/OggFileReader.h
    file/Mp3FileReader.cpp file/Mp3FileReader.h
    file/FileUtils.cpp file/FileUtils.h

    recorder/JamRecorder.cpp recorder/JamRecorder.h
    recorder/ReaperProjectGenerator.cpp recorder/ReaperProjectGenerator.h
    recorder/ClipSortLogGenerator.cpp recorder/ClipSortLogGenerator.h

    loginserver/LoginService.cpp loginserver/LoginService.h
    loginserver/Version.cpp loginserver/Version.h
    loginserver/MainChat.cpp loginserver/MainChat.h

    ninjam/Ninjam.cpp ninjam/Ninjam.h
    ninjam/PrivateServer.cpp ninjam/PrivateServer.h # hosts the built-in ninjamsrv
    ninjam/client/Types.h
    ninjam/client/ServerInfo.cpp ninjam/client/ServerInfo.h
    ninjam/client/Service.cpp ninjam/client/Service.h
    ninjam/client/User.cpp ninjam/client/User.h
    ninjam/client/UserChannel.cpp ninjam/client/UserChannel.h
    ninjam/client/ServerMessages.cpp ninjam/client/ServerMessages.h
    ninjam/client/ClientMessages.cpp ninjam/client/ClientMessages.h
    ninjam/client/ServerMessagesHandler.cpp ninjam/client/ServerMessagesHandler.h

    gui/plugins/Guis.cpp gui/plugins/Guis.h
    gui/PreferencesDialog.cpp gui/PreferencesDialog.h gui/PreferencesDialog.ui
    gui/LooperWindow.cpp gui/LooperWindow.h gui/LooperWindow.ui
    gui/NinjamRoomWindow.cpp gui/NinjamRoomWindow.h gui/NinjamRoomWindow.ui
    gui/BaseTrackView.cpp gui/BaseTrackView.h
    gui/NinjamTrackView.cpp gui/NinjamTrackView.h
    gui/NinjamTrackGroupView.cpp gui/NinjamTrackGroupView.h
    gui/NinjamPanel.cpp gui/NinjamPanel.h gui/NinjamPanel.ui
    gui/MetronomePanel.cpp gui/MetronomePanel.h gui/MetronomePanel.ui
    gui/BusyDialog.cpp gui/BusyDialog.h gui/BusyDialog.ui
    gui/IconFactory.cpp gui/IconFactory.h
    gui/InactivityDetector.cpp gui/InactivityDetector.h
    gui/Highligther.cpp gui/Highligther.h
    gui/TrackGroupView.cpp gui/TrackGroupView.h
    gui/LocalTrackGroupView.cpp gui/LocalTrackGroupView.h
    gui/LocalTrackView.cpp gui/LocalTrackView.h
    gui/JamRoomViewPanel.cpp gui/JamRoomViewPanel.h gui/JamRoomViewPanel.ui
    gui/PrivateServerDialog.cpp gui/PrivateServerDialog.h gui/PrivateServerDialog.ui
    gui/PrivateServerWindow.cpp gui/PrivateServerWindow.h gui/PrivateServerWindow.ui
    gui/UserNameDialog.cpp gui/UserNameDialog.h gui/UserNameDialog.ui
    gui/MainWindow.cpp gui/MainWindow.h gui/MainWindow.ui
    gui/BpiUtils.cpp gui/BpiUtils.h
    gui/UsersColorsPool.cpp gui/UsersColorsPool.h
    gui/GuiUtils.cpp gui/GuiUtils.h
    gui/ThemeLoader.cpp gui/ThemeLoader.h
    gui/TextEditorModifier.h
    gui/chat/ChatPanel.cpp gui/chat/ChatPanel.h gui/chat/ChatPanel.ui
    gui/chat/ChatMessagePanel.cpp gui/chat/ChatMessagePanel.h gui/chat/ChatMessagePanel.ui
    gui/chat/ChatTextEditor.cpp gui/chat/ChatTextEditor.h
    gui/chat/EmojiWidget.cpp gui/chat/EmojiWidget.h
    gui/chat/EmojiManager.cpp gui/chat/EmojiManager.h
    gui/chat/NinjamChatMessageParser.cpp gui/chat/NinjamChatMessageParser.h
    gui/chords/Chord.cpp gui/chords/Chord.h
    gui/chords/ChordLabel.cpp gui/chords/ChordLabel.h
    gui/chords/ChordsPanel.cpp gui/chords/ChordsPanel.h gui/chords/ChordsPanel.ui
    gui/chords/ChordProgression.cpp gui/chords/ChordProgression.h
    gui/chords/ChordProgressionMeasure.cpp gui/chords/ChordProgressionMeasure.h
    gui/chords/ChordsProgressionParser.h
    gui/chords/ChordProgressionCreationDialog.cpp gui/chords/ChordProgressionCreationDialog.h gui/chords/ChordProgressionCreationDialog.ui
    gui/chords/ChatChordsProgressionParser.cpp gui/chords/ChatChordsProgressionParser.h
    gui/intervalProgress/IntervalProgressDisplay.cpp gui/intervalProgress/IntervalProgressDisplay.h
    gui/intervalProgress/IntervalProgressWindow.cpp gui/intervalProgress/IntervalProgressWindow.h
    gui/intervalProgress/LinearPaintStrategy.cpp
    gui/intervalProgress/EllipticalPaintStrategy.cpp
    gui/intervalProgress/CircularPaintStrategy.cpp
    gui/intervalProgress/PiePaintStrategy.cpp
    gui/screensaver/ScreensaverBlocker.h
    gui/widgets/IntervalChunksDisplay.cpp gui/widgets/IntervalChunksDisplay.h
    gui/widgets/MarqueeLabel.cpp gui/widgets/MarqueeLabel.h
    gui/widgets/PeakMeter.cpp gui/widgets/PeakMeter.h
    gui/widgets/WavePeakPanel.cpp gui/widgets/WavePeakPanel.h
    gui/widgets/LooperWavePanel.cpp gui/widgets/LooperWavePanel.h
    gui/widgets/UserNameLineEdit.cpp gui/widgets/UserNameLineEdit.h
    gui/widgets/MapWidget.cpp gui/widgets/MapWidget.h
    gui/widgets/MapMarker.cpp gui/widgets/MapMarker.h
    gui/widgets/MultiStateButton.cpp gui/widgets/MultiStateButton.h
    gui/widgets/BlinkableButton.cpp gui/widgets/BlinkableButton.h
    gui/widgets/Slider.cpp gui/widgets/Slider.h
    gui/widgets/BoostSpinBox.cpp gui/widgets/BoostSpinBox.h
    gui/widgets/ChatTabWidget.cpp gui/widgets/ChatTabWidget.h
    gui/widgets/InstrumentsMenu.cpp gui/widgets/InstrumentsMenu.h

    log/Logging.h
    log/logging.cpp

    persistence/Settings.cpp persistence/Settings.h
    persistence/UsersDataCache.cpp persistence/UsersDataCache.h
    persistence/CacheHeader.cpp persistence/CacheHeader.h

    performance/PerformanceMonitor.h

    gui/mainframe.qrc
)

if(WIN32)
    list(APPEND JT_COMMON_SOURCES
        gui/screensaver/WindowsScreensaverBlocker.cpp
        performance/WindowsPerformanceMonitor.cpp
    )
elseif(APPLE)
    list(APPEND JT_COMMON_SOURCES
        gui/screensaver/MacScreensaverBlocker.mm
        performance/MacPerformanceMonitor.cpp
    )
else()
    list(APPEND JT_COMMON_SOURCES
        gui/screensaver/LinuxScreensaverBlocker.cpp
        performance/LinuxPerformanceMonitor.cpp
    )
endif()

list(TRANSFORM JT_COMMON_SOURCES PREPEND ${JT_COMMON}/)

# format-neutral plugin base (the VST3 layer lives in src/Plugins/VST3)
set(JT_PLUGIN_SOURCES
    JamTabaPlugin.cpp JamTabaPlugin.h
    MainControllerPlugin.cpp MainControllerPlugin.h
    MainWindowPlugin.cpp MainWindowPlugin.h
    NinjamControllerPlugin.cpp NinjamControllerPlugin.h
    NinjamRoomWindowPlugin.cpp NinjamRoomWindowPlugin.h
    PreferencesDialogPlugin.cpp PreferencesDialogPlugin.h
    TopLevelTextEditorModifier.cpp TopLevelTextEditorModifier.h
    ConfiguratorPlugin.cpp
)
list(TRANSFORM JT_PLUGIN_SOURCES PREPEND ${JT_PLUGINS}/)

# minimp3 (in tree, LGPL), used to decode the public rooms mp3 streams
set(JT_MINIMP3_SOURCES
    ${JT_COMMON}/minimp3/minimp3.c
    ${JT_COMMON}/minimp3/minimp3.h
    ${JT_COMMON}/minimp3/libc.h
)

# Opus intervals are encoded and decoded by njclient's own code (ninjam/opusencdec.h, header only),
# which uses the WDL resampler to bring other sample rates to the 48 kHz of Opus.
set(JT_NINJAM_CLIENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ninjam)
set(JT_WDL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../WDL/resample.cpp)

# The NINJAM server from this repository, built in so the plugin can host a private
# server (ninjam/PrivateServer.cpp). It runs on a background thread in the plugin.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../ninjam/server ${CMAKE_CURRENT_BINARY_DIR}/ninjamsrv EXCLUDE_FROM_ALL)

add_library(jamtaba_core STATIC
    ${JT_COMMON_SOURCES}
    ${JT_PLUGIN_SOURCES}
    ${JT_MINIMP3_SOURCES}
    ${JT_WDL_SOURCES}
    ${JT_RESOURCES}/jamtaba.qrc # also holds the translations (translations/*.qm)
)

set_target_properties(jamtaba_core PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
    AUTOUIC_SEARCH_PATHS "${JT_COMMON}/gui;${JT_COMMON}/gui/chat;${JT_COMMON}/gui/chords"
)

# The qmake build used PreCompiledHeaders.h for every C++ file, and the sources rely on it.
target_precompile_headers(jamtaba_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${JT_COMMON}/PreCompiledHeaders.h>)
set_source_files_properties(${JT_COMMON}/minimp3/minimp3.c ${JT_WDL_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
if(APPLE)
    set_source_files_properties(${JT_COMMON}/gui/screensaver/MacScreensaverBlocker.mm PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

# Public headers include the generated ui_*.h files (e.g. MainWindow.h includes ui_MainWindow.h),
# so the AUTOUIC output directory is part of the public include path.
get_property(JT_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(JT_MULTI_CONFIG)
    set(JT_AUTOGEN_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/jamtaba_core_autogen/include_$<CONFIG>)
else()
    set(JT_AUTOGEN_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/jamtaba_core_autogen/include)
endif()

target_include_directories(jamtaba_core
    PUBLIC
        ${JT_COMMON}
        ${JT_PLUGINS}
        ${JT_AUTOGEN_INCLUDE_DIR}
    PRIVATE
        ${JT_COMMON}/minimp3
        ${JT_NINJAM_CLIENT_DIR} # opusencdec.h
        ${CMAKE_CURRENT_BINARY_DIR}/opus-include # <opus/opus.h>
)

target_compile_definitions(jamtaba_core
    PUBLIC
        OV_EXCLUDE_STATIC_CALLBACKS # avoid the unused static callbacks in vorbisfile.h
    PRIVATE
        $<$<BOOL:${MSVC}>:_CRT_SECURE_NO_WARNINGS>
)

if(MSVC)
    target_compile_options(jamtaba_core PRIVATE /utf-8) # sources are UTF-8 (e.g. the "º7" chord symbol)
endif()

target_link_libraries(jamtaba_core
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Network
        Qt6::Concurrent
        Qt6::WebSockets # public chat (loginserver/MainChat)
        vorbisenc
        vorbisfile
        vorbis
        opus
        FLAC
        ogg
    PRIVATE
        ninjamsrv_embedded
)

if(APPLE)
    target_link_libraries(jamtaba_core PUBLIC "-framework IOKit" "-framework CoreFoundation") # MacScreensaverBlocker
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/Plugins/VST3/CMakeLists.txt)
    add_subdirectory(src/Plugins/VST3)
endif()
