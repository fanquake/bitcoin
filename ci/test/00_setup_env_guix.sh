#!/usr/bin/env bash
#
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit.

export LC_ALL=C.UTF-8

export BUILD_GUIX=true
export CONTAINER_NAME=ci_guix
# --cap-add CAP_SYS_ADMIN --cap-add=CAP_NET_ADMIN
export CI_CONTAINER_CAP="--security-opt seccomp=unconfined"
export CI_IMAGE_NAME_TAG="registry.gitlab.com/debdistutils/guix/debian-with-guix-container:stable"
export CI_BASE_PACKAGES="curl make git gpg gpg-agent procps rsync util-linux"
export NO_DEPENDS=1
