#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Long-running soak test (not part of run_tests.sh / CI): runs tests/soak.cpp
# against tests/fake_satip.py, once with UDP and once with RTP over TCP, and
# fails on left-over file descriptors, growing memory or a stream that does not
# come back after server restarts. No VDR installation is needed.
#   SOAK_SECONDS=600 tests/run_soak.sh     longer run (default 180 s per transport)
#   SANITIZE=1 tests/run_soak.sh           build with ASan/LSan/UBSan
set -o pipefail
cd "$(dirname "$0")" || exit 1
P=..
OUT=${TMPDIR:-/tmp}/vtsatip-soak
mkdir -p "$OUT"
DURATION=${SOAK_SECONDS:-180}
FLAGS="-O2"
if [ -n "$SANITIZE" ]; then
  FLAGS="-O1 -fno-omit-frame-pointer -fsanitize=address,undefined"
  export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1}
  export SOAK_RSS_SLACK_KB=${SOAK_RSS_SLACK_KB:-65536} # ASan's quarantine grows on its own
fi
SRC="$P/tuner.c $P/rtsp.c $P/sectionfilter.c $P/pat.c $P/vtconfig.c $P/servers.c $P/discover.c"
g++ -std=c++17 $FLAGS -g -Wall -Istubs -I$P -include functional -x c++ soak.cpp $SRC -o "$OUT/soak" -lpthread || { echo "BUILD FAILED: soak"; exit 1; }
fail=0
for transport in udp tcp; do
  PORT=$((20000 + RANDOM % 10000))
  rm -f "$OUT/server.log"
  echo "### soak: $transport"
  "$OUT/soak" $PORT "$DURATION" $transport "$OUT/server.log" | grep -v '^vtsatip:' || fail=1
done
exit $fail
