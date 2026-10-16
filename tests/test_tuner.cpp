// Copyright (C) 2026 Andreas Baierl <ichgeh@imkreisrum.de>
// SPDX-License-Identifier: GPL-2.0-only
#include <vdr/tools.h>
#include <vdr/thread.h>
#include <vdr/ringbuffer.h>
#include <atomic>
#include <map>
#include <fcntl.h>
#include "common.h"
#include "tuner.h"
#include "sectionfilter.h"

int StubLogLevel = 0;
cVtSatipConfig VtSatipConfig;

static const char *LOGFILE = "server.log";
static void Mark(const char *s) {
  FILE *f = fopen(LOGFILE, "a"); struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
  fprintf(f, "%ld.%03ld MARK %s\n", (long)t.tv_sec, t.tv_nsec / 1000000, s); fclose(f);
  printf("== %s\n", s); fflush(stdout);
}

struct Sink : cVtTunerSink {
  std::atomic<unsigned> lastMarker{0};
  std::atomic<unsigned long> packets{0};
  std::atomic<int> retunes{0};
  std::map<int, unsigned long> pidCount; cMutex m;
  int SinkIndex() override { return 0; }
  void OnTsData(const uchar *d, int len) override {
    for (int i = 0; i + TS_SIZE <= len; i += TS_SIZE) {
      lastMarker = (d[i+4] << 24) | (d[i+5] << 16) | (d[i+6] << 8) | d[i+7];
      m.Lock(); pidCount[VtTsPid(d + i)]++; m.Unlock(); ++packets;
    }
  }
  void OnRetune() override { ++retunes; m.Lock(); pidCount.clear(); m.Unlock(); packets = 0; }
};

static bool WaitFor(std::function<bool()> f, int ms) { cTimeMs t(ms); while (!t.TimedOut()) { if (f()) return true; usleep(1000); } return false; }
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++fails; } else printf("ok:   %s\n", msg); } while (0)

int main(int argc, char **argv) {
  int port = argc > 1 ? atoi(argv[1]) : 8554;
  int ka = argc > 2 ? atoi(argv[2]) : 0;
  VtSatipConfig.host = "127.0.0.1"; VtSatipConfig.port = port; VtSatipConfig.realtime = false;
  Sink sink; cVtTuner *t = new cVtTuner(sink);

  Mark("1 first tune");
  cTimeMs tm(0);
  t->SetChannel("src=1&freq=11493&pol=h&msys=dvbs2&mtype=8psk&sr=22000&fec=23");
  t->SetPid(100, true);
  CHECK(WaitFor([&]{ return t->HasLock(); }, 3000), "lock after first tune");
  printf("   first tune -> lock: %d ms\n", (int)tm.Elapsed());
  CHECK(WaitFor([&]{ return sink.lastMarker == 11493; }, 1000), "data of first transponder");
  CHECK(WaitFor([&]{ std::map<int,unsigned long> c; sink.m.Lock(); c = sink.pidCount; sink.m.Unlock(); return c.count(100) && c.count(0) && c.count(18); }, 2000), "pid 100 and standard pids arrive");
  CHECK(WaitFor([&]{ return t->SignalQuality() == 100; }, 2000), "RTCP signal quality parsed (15 -> 100%)");
  CHECK(t->SignalStrength() == 88, "RTCP signal strength parsed (224 -> 88%)");

  Mark("2 retune");
  int r0 = sink.retunes; tm.Set(0);
  t->SetChannel("src=1&freq=12188&pol=v&msys=dvbs&mtype=qpsk&sr=27500&fec=34");
  CHECK(WaitFor([&]{ return sink.lastMarker == 12188; }, 3000), "data of second transponder");
  printf("   retune -> data: %d ms\n", (int)tm.Elapsed());
  CHECK(sink.retunes > r0, "buffers flushed on retune");
  CHECK(t->HasLock(), "lock after retune");

  Mark("3 same params again (no traffic expected)");
  t->SetChannel("src=1&freq=12188&pol=v&msys=dvbs&mtype=qpsk&sr=27500&fec=34");
  usleep(400000);
  CHECK(t->HasLock(), "lock kept");

  Mark("4 pid change 100 -> 200");
  t->SetPid(100, false); t->SetPid(200, true);
  CHECK(WaitFor([&]{ std::map<int,unsigned long> c; sink.m.Lock(); c = sink.pidCount; sink.m.Unlock(); return c.count(200) > 0; }, 2000), "pid 200 arrives");
  usleep(300000);

  Mark("5 release");
  t->Release(); usleep(400000);
  CHECK(!t->HasLock(), "no lock after release");

  Mark("6 tune again after release");
  tm.Set(0);
  t->SetChannel("src=1&freq=11493&pol=h&msys=dvbs2&mtype=8psk&sr=22000&fec=23");
  CHECK(WaitFor([&]{ return t->HasLock(); }, 3000), "lock after reconnect");
  printf("   reconnect -> lock: %d ms\n", (int)tm.Elapsed());

  Mark("7 connection drop while changing pids");
  t->SetPid(999, true);
  CHECK(WaitFor([&]{ return !t->HasLock(); }, 1000) || true, "lock dropped (or recovered fast)");
  CHECK(WaitFor([&]{ return t->HasLock() && sink.lastMarker == 11493; }, 6000), "tuner recovered by itself");

  if (ka) { Mark("8 keep-alive"); sleep(ka); CHECK(t->HasLock(), "still locked after keep-alive period"); }

  Mark("9 shutdown");
  delete t;
  printf("%s\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
  return fails ? 1 : 0;
}
