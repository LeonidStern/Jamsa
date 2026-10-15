# JamTaba plugin

A NINJAM client that runs as a VST3 plugin inside a DAW, built from
[JamTaba](https://github.com/elieserdejesus/JamTaba).

## Building

You need CMake 3.24 or newer, a C++17 compiler and Qt 6 (Core, Gui,
Widgets, Network, Concurrent, WebSockets). Ogg, Vorbis and the VST3 SDK
are downloaded when CMake configures the build.

    cmake -S jamtaba -B build-jamtaba -DCMAKE_BUILD_TYPE=Release
    cmake --build build-jamtaba --target JamTaba

The plugin ends up in `build-jamtaba/VST3/Release/JamTaba.vst3`. To check
it with Steinberg's validator:

    cmake --build build-jamtaba --target validator
    build-jamtaba/bin/Release/validator build-jamtaba/VST3/Release/JamTaba.vst3

On Linux the validator needs the GTK 3 and gtkmm 3 development packages.

## Where the code came from

The sources under `src/` and `translations/` were imported from
elieserdejesus/JamTaba at commit 4bdbe69 (2021-10-30): `src/Common`,
`src/Plugins` (without the AU plugin) and `src/resources`. The standalone
app, the plugin hosts, the plugin scanners and the theme editor were left
out. Since then the code was ported to Qt 6 and CMake, video and
JamTaba's own server were removed, and the VST2 plugin was replaced by
the VST3 one in `src/Plugins/VST3`.

The JamTaba repository has no license file.
