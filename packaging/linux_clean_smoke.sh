#!/bin/bash
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
# Keep the app dependencies synchronized with linux-runtime.txt. Xvfb/xauth
# provide a test display only and are not application requirements.
apt-get install -y --no-install-recommends xvfb xauth ca-certificates \
  libglib2.0-0t64 libgl1 libegl1 libopengl0 libfontconfig1 libfreetype6 libdbus-1-3 \
  libx11-6 libx11-xcb1 libxext6 libxrender1 libxcb1 libxcb-cursor0 \
  libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 \
  libxcb-render-util0 libxcb-render0 libxcb-shape0 libxcb-shm0 \
  libxcb-sync1 libxcb-xfixes0 libxcb-xkb1 libxkbcommon0 \
  libxkbcommon-x11-0 libsm6 libice6
mkdir -p '/tmp/installed package 한글'
tar -xzf /packages/ConflictBench-*-linux-x86_64.tar.gz -C '/tmp/installed package 한글'
app=('/tmp/installed package 한글'/ConflictBench-*/ConflictBench)
test "${#app[@]}" -eq 1
xvfb-run -a "${app[0]}" --smoke-test
