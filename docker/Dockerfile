# Copyright The Mumble Developers. All rights reserved.
# Use of this source code is governed by a BSD-style license
# that can be found in the LICENSE file at the root of the
# Mumble source tree or at <https://www.mumble.info/LICENSE>.

# A reproducible Linux build environment for Mumble.
#
# See README.md in this directory for usage.
#
# Note: docs/dev/build-instructions/build_linux.md lists dependencies for Ubuntu and Debian only.
# The packages below are the Fedora equivalents. They additionally include the database client
# libraries (mariadb-connector-c-devel, libpq-devel, sqlite-devel) that the bundled SOCI requires
# but which that document does not mention: SOCI builds one backend per database it detects, and
# CMake configuration fails outright if the MySQL client library is missing.

ARG BASE_IMAGE=fedora:42
FROM ${BASE_IMAGE}

RUN dnf install -y --setopt=install_weak_deps=False \
        gcc-c++ make cmake pkgconf git \
        qt6-qtbase-devel qt6-qttools-devel qt6-qtsvg-devel qt6-qtmultimedia-devel \
        mesa-libGL-devel boost-devel openssl-devel \
        protobuf-devel protobuf-compiler \
        libcap-devel libXi-devel nlohmann-json-devel poco-devel \
        alsa-lib-devel libogg-devel libsndfile-devel opus-devel speexdsp-devel \
        speech-dispatcher-devel avahi-compat-libdns_sd-devel \
        mariadb-connector-c-devel libpq-devel sqlite-devel \
        libvpx-devel \
    && dnf clean all

WORKDIR /build
