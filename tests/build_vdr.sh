#!/bin/bash
# Copyright (C) 2026 Andreas Baierl <ichgeh@imkreisrum.de>
# SPDX-License-Identifier: GPL-2.0-only
# Builds the plugin against the real VDR headers of one VDR release.
# Usage: tests/build_vdr.sh <vdr-tag> [work-dir]     e.g. tests/build_vdr.sh 2.8.3
# Needs git, g++, make, pkg-config; with gettext installed the translation
# files (msgfmt -c) are built as well.
set -e
TAG=${1:?usage: $0 <vdr-tag> [work-dir]}
WORK=${2:-${TMPDIR:-/tmp}/vtsatip-vdr-$TAG}
SRC=$(cd "$(dirname "$0")/.." && pwd)
URL=${VDR_GIT_URL:-https://github.com/vdr-projects/vdr.git}
rm -rf "$WORK"
git clone -q --depth 1 --branch "$TAG" "$URL" "$WORK/vdr"
make -C "$WORK/vdr" include-dir >/dev/null
make -C "$WORK/vdr" vdr.pc >/dev/null
mkdir -p "$WORK/vdr/PLUGINS/src"
# build a copy so the source tree stays clean
rm -rf "$WORK/vdr/PLUGINS/src/vtsatip"
mkdir "$WORK/vdr/PLUGINS/src/vtsatip"
(cd "$SRC" && tar --exclude=.git --exclude='*.o' --exclude='*.so' -cf - .) | tar -xf - -C "$WORK/vdr/PLUGINS/src/vtsatip"
cd "$WORK/vdr/PLUGINS/src/vtsatip"
if command -v msgfmt >/dev/null && command -v xgettext >/dev/null; then
   TARGET=all
else
   echo "note: gettext not installed, building the library only"
   TARGET=libvdr-vtsatip.so
fi
PKG_CONFIG_PATH=../../.. make INCLUDES="-I../../../include" CXXFLAGS_EXTRA= $TARGET 2>&1 | tee "$WORK/build.log"
test -f libvdr-vtsatip.so
if grep -E "\.(c|h):[0-9]+:[0-9]+: warning:" "$WORK/build.log"; then
   echo "FAIL: compiler warnings"; exit 1
fi
echo "OK: vtsatip $TAG"
