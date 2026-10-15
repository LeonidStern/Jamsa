#!/usr/bin/env bash
# Builds the static Qt the JamTaba plugin links in: qtbase and qtwebsockets, in the
# JamTabaQt namespace. Static, so JamTaba.vst3 is a single file with no Qt to install.
# Namespaced, so its Qt can't clash with another copy of Qt loaded into the same host
# (window class names on Windows, Objective-C class names on macOS).
#
#   jamtaba/ci/build-static-qt.sh <qt version> <install prefix> [extra CMake arguments]
#
# Extra arguments go to both modules, e.g. -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64".
# Needs git, CMake, Ninja and a C++ compiler (on Windows, run it from a Visual Studio
# developer shell). Sources are cloned into $QT_BUILD_DIR (default ./qt-build).

set -euo pipefail

version=$1
prefix=$2
shift 2

work=${QT_BUILD_DIR:-$PWD/qt-build}
mkdir -p "$work"
cd "$work"

for module in qtbase qtwebsockets; do
    if [ ! -d "$module" ]; then
        git clone --quiet --depth 1 --branch "v$version" "https://github.com/qt/$module.git" "$module"
    fi
done

cmake -S qtbase -B build-qtbase -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DBUILD_SHARED_LIBS=OFF \
    -DQT_NAMESPACE=JamTabaQt \
    -DQT_BUILD_EXAMPLES=OFF \
    -DQT_BUILD_TESTS=OFF \
    -DFEATURE_sql=OFF \
    -DFEATURE_testlib=OFF \
    -DFEATURE_printsupport=OFF \
    -DFEATURE_dbus=OFF \
    -DFEATURE_opengl=OFF \
    -DFEATURE_openssl=OFF \
    "$@"
cmake --build build-qtbase
cmake --install build-qtbase

cmake -S qtwebsockets -B build-qtwebsockets -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$prefix" \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DQT_BUILD_EXAMPLES=OFF \
    -DQT_BUILD_TESTS=OFF \
    "$@"
cmake --build build-qtwebsockets
cmake --install build-qtwebsockets
