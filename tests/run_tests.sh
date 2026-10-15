#!/bin/bash
# Builds and runs the unit/integration tests against stand-ins for the VDR headers
# and a fake SAT>IP server. No VDR installation is needed.
cd "$(dirname "$0")" || exit 1
P=..
OUT=${TMPDIR:-/tmp}/vtsatip-tests
mkdir -p "$OUT"
CXX="g++ -std=c++17 -O1 -g -Wall -Istubs -I$P -include functional -x c++"
SRC="$P/tuner.c $P/rtsp.c $P/sectionfilter.c $P/pat.c $P/vtconfig.c $P/servers.c $P/discover.c"
fail=0
build() { out=$1; shift; $CXX "$@" -o "$OUT/$out" -lpthread || { echo "BUILD FAILED: $out"; fail=1; }; }
build test_cfg test_cfg.cpp $P/vtconfig.c
build test_sec test_sec.cpp $P/sectionfilter.c
build test_tuner test_tuner.cpp $SRC
build test_ci test_ci.cpp $SRC
build test_discover test_discover.cpp $SRC
build test_compat test_compat.cpp $SRC
echo "### config";   "$OUT/test_cfg" || fail=1
echo "### sections"; "$OUT/test_sec" || fail=1
for t in tuner ci; do
  PORT=$((20000 + RANDOM % 10000))
  rm -f "$OUT/server.log"
  python3 fake_satip.py $PORT "$OUT/server.log" 30 & SRV=$!
  sleep 0.5
  echo "### $t"
  (cd "$OUT" && timeout 90 ./test_$t $PORT 0) || fail=1
  kill $SRV 2>/dev/null
  wait $SRV 2>/dev/null
done
# server quirks, handled by generic fallbacks
for mode in zeros nodelta notune busy mcast nomcast refusemcast; do
  PORT=$((20000 + RANDOM % 10000)); PORT2=$((PORT + 1))
  rm -f "$OUT/server.log"
  FAKE_MCAST_PORT=$((PORT + 100)) python3 fake_satip.py $PORT "$OUT/server.log" 30 $mode & SRV=$!
  OK=""
  if [ $mode = busy ]; then python3 fake_satip.py $PORT2 "$OUT/server2.log" 30 & OK=$!; fi
  sleep 0.5
  echo "### compat: $mode"
  (cd "$OUT" && timeout 60 ./test_compat $mode $PORT $PORT2) || fail=1
  kill $SRV $OK 2>/dev/null; wait $SRV $OK 2>/dev/null
done
# UPnP: fake SAT>IP server (RTSP) + fake UPnP device (SSDP/HTTP)
PORT=$((20000 + RANDOM % 10000)); HTTP=$((PORT + 1)); SSDP=$((PORT + 2))
rm -f "$OUT/server.log"
python3 fake_satip.py $PORT "$OUT/server.log" 30 & SRV=$!
python3 fake_upnp.py $SSDP $HTTP $PORT "DVBS2-2,DVBT2-1" & UPNP=$!
sleep 0.7
echo "### discover"
(cd "$OUT" && timeout 90 ./test_discover $PORT $SSDP $HTTP) || fail=1
kill $SRV $UPNP 2>/dev/null
wait $SRV $UPNP 2>/dev/null
exit $fail
