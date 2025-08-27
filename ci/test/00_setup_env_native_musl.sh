#!/usr/bin/env sh
#
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CI_RETRY_EXE=" "
export CONTAINER_NAME=ci_native_musl
export CI_IMAGE_NAME_TAG="mirror.gcr.io/alpine:latest"
export CI_BASE_PACKAGES="build-base cmake linux-headers pkgconf python3 py3-pip libevent-dev boost-dev sqlite-dev capnproto capnproto-dev zeromq-dev qt6-qtbase-dev  qt6-qttools-dev libqrencode-dev"
export PIP_PACKAGES="--break-system-packages pyzmq"
export GOAL="install"
export BITCOIN_CONFIG="\
 -DWITH_ZMQ=ON \
 -DBUILD_GUI=ON \
 -DREDUCE_EXPORTS=ON \
 -DBUILD_UTIL_CHAINSTATE=ON \
 "
