#!/usr/bin/env bash
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.
export LC_ALL=C
set -e -o pipefail

# shellcheck source=setup.sh
source "$(dirname "${BASH_SOURCE[0]}")/setup.sh"

# Set environment variables to point the NATIVE toolchain to the right
# includes/libs
NATIVE_GCC="$(store_path gcc-toolchain)"

unset LIBRARY_PATH
unset CPATH
unset C_INCLUDE_PATH
unset CPLUS_INCLUDE_PATH
unset OBJC_INCLUDE_PATH
unset OBJCPLUS_INCLUDE_PATH

# Set native toolchain
build_CC="${NATIVE_GCC}/bin/gcc -isystem ${NATIVE_GCC}/include"
build_CXX="${NATIVE_GCC}/bin/g++ -isystem ${NATIVE_GCC}/include/c++ -isystem ${NATIVE_GCC}/include"

NATIVE_GCC_STATIC="$(store_path gcc-toolchain static)"
export LIBRARY_PATH="${NATIVE_GCC}/lib:${NATIVE_GCC_STATIC}/lib"

# Set environment variables to point the CROSS toolchain to the right
# includes/libs for $HOST
CROSS_GLIBC="$(store_path "glibc-cross-${HOST}")"
CROSS_GLIBC_STATIC="$(store_path "glibc-cross-${HOST}" static)"
CROSS_KERNEL="$(store_path "linux-libre-headers-cross-${HOST}")"
CROSS_GCC="$(store_path "gcc-cross-${HOST}")"
CROSS_GCC_LIB_STORE="$(store_path "gcc-cross-${HOST}" lib)"
CROSS_GCC_LIBS=( "${CROSS_GCC_LIB_STORE}/lib/gcc/${HOST}"/* ) # This expands to an array of directories...
CROSS_GCC_LIB="${CROSS_GCC_LIBS[0]}" # ...we just want the first one (there should only be one)

export CROSS_C_INCLUDE_PATH="${CROSS_GCC_LIB}/include:${CROSS_GCC_LIB}/include-fixed:${CROSS_GLIBC}/include:${CROSS_KERNEL}/include"
export CROSS_CPLUS_INCLUDE_PATH="${CROSS_GCC}/include/c++:${CROSS_GCC}/include/c++/${HOST}:${CROSS_GCC}/include/c++/backward:${CROSS_C_INCLUDE_PATH}"
export CROSS_LIBRARY_PATH="${CROSS_GCC_LIB_STORE}/lib:${CROSS_GCC_LIB}:${CROSS_GLIBC}/lib:${CROSS_GLIBC_STATIC}/lib"

# Sanity check CROSS_*_PATH directories
IFS=':' read -ra PATHS <<< "${CROSS_C_INCLUDE_PATH}:${CROSS_CPLUS_INCLUDE_PATH}:${CROSS_LIBRARY_PATH}"
for p in "${PATHS[@]}"; do
    if [ -n "$p" ] && [ ! -d "$p" ]; then
        echo "'$p' doesn't exist or isn't a directory... Aborting..."
        exit 1
    fi
done

# Determine the correct value for -Wl,--dynamic-linker for the current $HOST
glibc_dynamic_linker=$(
    case "$HOST" in
        aarch64-linux-gnu|arm-linux-gnueabihf|riscv64-linux-gnu|x86_64-linux-gnu)      ;;
        powerpc64-linux-gnu)   echo /lib64/ld64.so.1;;
        powerpc64le-linux-gnu) echo /lib64/ld64.so.2;;
        *)                     exit 1 ;;
    esac
)

####################
# Depends Building #
####################

# Build the depends tree, overriding variables that assume multilib gcc
make -C depends --jobs="$JOBS" HOST="$HOST" \
                                   ${V:+V=1} \
                                   ${SOURCES_PATH+SOURCES_PATH="$SOURCES_PATH"} \
                                   ${BASE_CACHE+BASE_CACHE="$BASE_CACHE"} \
                                   ${SDK_PATH+SDK_PATH="$SDK_PATH"} \
                                   ${build_CC+build_CC="$build_CC"} \
                                   ${build_CXX+build_CXX="$build_CXX"} \
                                   x86_64_linux_CC=x86_64-linux-gnu-gcc \
                                   x86_64_linux_CXX=x86_64-linux-gnu-g++ \
                                   x86_64_linux_AR=x86_64-linux-gnu-gcc-ar \
                                   x86_64_linux_RANLIB=x86_64-linux-gnu-gcc-ranlib \
                                   x86_64_linux_NM=x86_64-linux-gnu-gcc-nm \
                                   x86_64_linux_STRIP=x86_64-linux-gnu-strip \
                                   NO_QT=1 NO_ZMQ=1 NO_WALLET=1 NO_IPC=1 NO_USDT=1 \
                                   CFLAGS="-O2 -flto" \
                                   CXXFLAGS="-O2 -flto" \
                                   LDFLAGS="-O2 -flto" \
                                   LTO=1

###########################
# Binary Tarball Building #
###########################

# CONFIGFLAGS
CONFIGFLAGS="-DREDUCE_EXPORTS=ON -DBUILD_BENCH=OFF -DBUILD_FUZZ_BINARY=OFF -DCMAKE_SKIP_RPATH=TRUE -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_EXTERNAL_SIGNER=OFF"

# CFLAGS
HOST_CFLAGS="-O2 -g -flto"
HOST_CFLAGS+=$(find /gnu/store -maxdepth 1 -mindepth 1 -type d -exec echo -n " -ffile-prefix-map={}=/usr" \;)
HOST_CFLAGS+=" -fdebug-prefix-map=${DISTSRC}/src=."

case "$HOST" in
    aarch64-linux-gnu) HOST_CFLAGS+="${HOST_CFLAGS} -mcpu=native" ;;
    x86_64-linux-gnu)  HOST_CFLAGS+="${HOST_CFLAGS} -march=native" ;;
esac

# CXXFLAGS
HOST_CXXFLAGS="$HOST_CFLAGS"

case "$HOST" in
    arm-linux-gnueabihf) HOST_CXXFLAGS="${HOST_CXXFLAGS} -Wno-psabi" ;;
esac

# LDFLAGS
HOST_LDFLAGS="-Wl,--as-needed -Wl,--dynamic-linker=$glibc_dynamic_linker -Wl,-O2 -flto"

case "$HOST" in
    aarch64-linux-gnu) HOST_LDFLAGS+="${HOST_LDFLAGS} -mcpu=native" ;;
    x86_64-linux-gnu)  HOST_LDFLAGS+="${HOST_LDFLAGS} -march=native" ;;
esac

# EXE FLAGS
case "$HOST" in
    aarch64-linux-gnu|arm-linux-gnueabihf|riscv64-linux-gnu|x86_64-linux-gnu) CMAKE_EXE_LINKER_FLAGS="-DCMAKE_EXE_LINKER_FLAGS=-static-pie -static-libgcc -Wl,-O2" ;;
    *linux*)  CMAKE_EXE_LINKER_FLAGS="-DCMAKE_EXE_LINKER_FLAGS=${HOST_LDFLAGS} -static-libstdc++ -static-libgcc" ;;
esac

mkdir -p "$DISTSRC"
(
    cd "$DISTSRC"

    # Extract the source tarball
    tar --strip-components=1 -xf "${GIT_ARCHIVE}"

    # Configure this DISTSRC for $HOST
    # shellcheck disable=SC2086
    env CFLAGS="${HOST_CFLAGS}" CXXFLAGS="${HOST_CXXFLAGS}" LDFLAGS="${HOST_LDFLAGS}" \
    cmake -S . -B build \
          --toolchain "${BASEPREFIX}/${HOST}/toolchain.cmake" \
          -DWITH_CCACHE=OFF \
          -Werror=dev \
          ${CONFIGFLAGS} \
          ${CMAKE_EXE_LINKER_FLAGS+"$CMAKE_EXE_LINKER_FLAGS"}

    # Build Bitcoin Core
    cmake --build build -j "$JOBS" --target bitcoind
)

exit 0
