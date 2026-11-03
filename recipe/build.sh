#!/bin/bash
set -e
set -x

mkdir -p build
cd build

BUILD_TYPE=RelWithDebInfo

# Determine OSX architecture from the target platform
if [[ "$target_platform" == "osx-arm64" ]]; then
    OSX_ARCH="arm64"
elif [[ "$target_platform" == "osx-64" ]]; then
    OSX_ARCH="x86_64"
else
    OSX_ARCH=$(uname -m)
fi

IGNORE_ARGS=()
if [[ "$target_platform" == osx-* ]]; then
    IGNORE_ARGS=(
        "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local"
        "-DCMAKE_SYSTEM_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local"
    )
fi

# ${CMAKE_ARGS} carries the conda toolchain settings (compilers, ar/ranlib, sysroot,
# find-root paths). Our own flags come after it so they override its defaults for
# install prefix and build type.
cmake .. ${CMAKE_ARGS} \
      "${IGNORE_ARGS[@]}" \
      -DCMAKE_PREFIX_PATH=$PREFIX \
      -DCMAKE_INSTALL_PREFIX=$PREFIX/lib/q/mod/kx \
      -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
      -DCMAKE_OSX_ARCHITECTURES=$OSX_ARCH

cmake --build . --config $BUILD_TYPE

cmake --build . --config $BUILD_TYPE --target install
