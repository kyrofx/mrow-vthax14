#!/bin/bash
# Configures /src/build with the feature flags CI uses
# (.github/workflows/tests.yml). Extra arguments are passed to cmake.
set -euo pipefail

exec cmake -S /src -B /src/build \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTING=ON \
    -DQT6=ON \
    -DBATTERY=ON \
    -DBROADCAST=ON \
    -DBULK=ON \
    -DFFMPEG=ON \
    -DHID=ON \
    -DKEYFINDER=ON \
    -DLILV=ON \
    -DLOCALECOMPARE=ON \
    -DMAD=ON \
    -DMODPLUG=ON \
    -DOPUS=ON \
    -DQTKEYCHAIN=ON \
    -DVINYLCONTROL=ON \
    -DWAVPACK=ON \
    -DDOWNLOAD_MANUAL=OFF \
    -DINSTALL_USER_UDEV_RULES=OFF \
    -DDEBUG_ASSERTIONS_FATAL=OFF \
    -DWARNINGS_FATAL=OFF \
    "$@"
