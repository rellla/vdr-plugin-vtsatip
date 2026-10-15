#include <vdr/tools.h>
#include <vdr/thread.h>
#include <vdr/ringbuffer.h>
#include <atomic>
#include <fstream>
#include <map>
#include "common.h"
#include "tuner.h"
#include "servers.h"

int StubLogLevel = 0;
cVtSatipConfig VtSatipConfig;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++fails; } else printf("ok:   %s\n", msg); } while (0)
static bool WaitFor(std::function<bool()> f, int ms) { cTimeMs t(ms); while (!t.TimedOut()) { if (f()) return true; usleep(1000); } return false; }
static std::string LogText(const char *f) { std::ifstream in(f); return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); }
static int Count(const std::string &s, const std::string &n) { int c = 0; for (size_t p = s.find(n); p != std::string::npos; p = s.find(n, p + 1)) ++c; return c; }

struct Sink : cVtTunerSink {
  std::atomic<unsigned> lastMarker{0};
  std::map<int, unsigned long> pids; cMutex m;
  std::atomic<int> busy{0}; std::string busyHost; int busyPort = 0;
  int SinkIndex() override { return 0; }
  void OnTsData(const uchar *d, int len) override { for (int i = 0; i + TS_SIZE <= len; i += TS_SIZE) { lastMarker = (d[i+4] << 24) | (d[i+5] << 16) | (d[i+6] << 8) | d[i+7]; m.Lock(); pids[VtTsPid(d + i)]++; m.Unlock(); } }
  void OnRetune() override { m.Lock(); pids.clear(); m.Unlock(); }
  void OnServerBusy(const std::string &h, int p) override { busyHost = h; busyPort = p; ++busy; }
  bool HasPid(int pid) { m.Lock(); bool r = pids.count(pid) > 0; m.Unlock(); return r; }
};
static const char *TP1 = "src=1&freq=11493&pol=h&msys=dvbs2&mtype=8psk&sr=22000&fec=23";
static const char *TP2 = "src=1&freq=12188&pol=v&msys=dvbs&mtype=qpsk&sr=27500&fec=34";

int main(int argc, char **argv) {
  // argv: <mode> <port> [port2]   modes: zeros nodelta notune busy
  std::string mode = argv[1]; int port = atoi(argv[2]);
  VtSatipConfig.host = "127.0.0.1"; VtSatipConfig.port = port; VtSatipConfig.realtime = false;
  if (mode == "mcast" || mode == "nomcast" || mode == "refusemcast") {
    VtSatipConfig.transport = VTSATIP_TRANSPORT_MULTICAST;
    VtSatipConfig.mcastInterface = "127.0.0.1";
  }
  if (mode == "tcp" || mode == "notcp" || mode == "refusetcp")
    VtSatipConfig.transport = VTSATIP_TRANSPORT_TCP;
  if (mode == "allpids")
    VtSatipConfig.allPids = true;
  Sink sink; cVtTuner t(sink);
  if (mode == "allpids") {
    t.SetChannel(TP1);
    CHECK(WaitFor([&]{ return t.HasLock() && sink.HasPid(200) && sink.HasPid(100); }, 4000), "all PIDs arrive without asking for them");
    int plays = Count(LogText("server.log"), "REQ PLAY");
    t.SetPid(300, true); t.SetPid(301, true);
    usleep(300000);
    CHECK(Count(LogText("server.log"), "REQ PLAY") == plays, "PID changes send nothing");
    CHECK(LogText("server.log").find("pids=all") != std::string::npos, "PLAY carries pids=all");
    t.SetChannel(TP2);
    CHECK(WaitFor([&]{ return t.HasLock() && sink.lastMarker == 12188; }, 4000), "retune works");
  }
  else if (mode == "tcp") {
    t.SetChannel(TP1);
    CHECK(WaitFor([&]{ return t.HasLock() && sink.lastMarker == 11493; }, 4000), "stream received interleaved in the RTSP connection");
    CHECK(Count(LogText("server.log"), "REQ SETUP") == 1, "one SETUP (no fallback)");
    CHECK(WaitFor([&]{ return t.SignalQuality() == 100; }, 2000), "RTCP arrives on the interleaved RTCP channel");
    t.SetPid(100, true);
    CHECK(WaitFor([&]{ return sink.HasPid(100); }, 3000), "PID changes work while data is flowing on the control connection");
    for (int i = 0; i < 20; ++i) { // many requests between data frames
      t.SetPid(200 + i, true);
      usleep(20000);
    }
    CHECK(WaitFor([&]{ return sink.HasPid(219); }, 3000), "20 quick PID changes");
    t.SetChannel(TP2);
    CHECK(WaitFor([&]{ return t.HasLock() && sink.lastMarker == 12188; }, 4000), "retune inside the TCP session");
    t.Release();
    usleep(300000);
    t.SetChannel(TP1);
    CHECK(WaitFor([&]{ return t.HasLock() && sink.lastMarker == 11493; }, 4000), "new TCP session after release");
  }
  else if (mode == "notcp" || mode == "refusetcp") {
    t.SetChannel(TP1);
    CHECK(WaitFor([&]{ return t.HasLock() && sink.lastMarker == 11493; }, 5000), "falls back to UDP");
    CHECK(Count(LogText("server.log"), "REQ SETUP") == 2, "second SETUP is the UDP one");
    t.Release();
    usleep(300000);
    t.SetChannel(TP2);
    CHECK(WaitFor([&]{ return t.HasLock() && sink.lastMarker == 12188; }, 4000), "works afterwards");
    CHECK(Count(LogText("server.log"), "REQ SETUP") == 3, "server is remembered as TCP-less");
  }
  else if (mode == "mcast") {
    t.SetChannel(TP1);
    CHECK(WaitFor([&]{ return t.HasLock() && sink.lastMarker == 11493; }, 4000), "stream received through the multicast group");
    CHECK(Count(LogText("server.log"), "REQ SETUP") == 1, "one SETUP (no fallback)");
    t.SetPid(100, true);
    CHECK(WaitFor([&]{ return sink.HasPid(100); }, 3000), "PID changes work in a multicast session");
    CHECK(WaitFor([&]{ return t.SignalQuality() == 100; }, 2000), "RTCP arrives on the multicast RTCP port");
    t.SetChannel(TP2);
    CHECK(WaitFor([&]{ return t.HasLock() && sink.lastMarker == 12188; }, 4000), "retune inside the multicast session");
    t.Release();
    usleep(300000);
    t.SetChannel(TP1);
    CHECK(WaitFor([&]{ return t.HasLock() && sink.lastMarker == 11493; }, 4000), "new multicast session after release");
  }
  else if (mode == "nomcast" || mode == "refusemcast") {
    t.SetChannel(TP1);
    CHECK(WaitFor([&]{ return t.HasLock() && sink.lastMarker == 11493; }, 5000), "falls back to unicast");
    CHECK(Count(LogText("server.log"), "REQ SETUP") == 2, "second SETUP is the unicast one");
    t.SetChannel(TP2);
    CHECK(WaitFor([&]{ return t.HasLock() && sink.lastMarker == 12188; }, 4000), "retune works");
    t.Release();
    usleep(300000);
    t.SetChannel(TP1);
    CHECK(WaitFor([&]{ return t.HasLock(); }, 4000), "server is remembered as unicast-only");
    CHECK(Count(LogText("server.log"), "REQ SETUP") == 3, "no further multicast attempt after release");
  }
  else if (mode == "zeros") {
    t.SetChannel(TP1);
    CHECK(WaitFor([&]{ return t.HasLock(); }, 4000), "lock although the server dislikes session ids with leading zeros");
    CHECK(LogText("server.log").find("REFUSED PLAY 454") != std::string::npos, "the first PLAY was refused (454)");
    t.SetPid(100, true);
    CHECK(WaitFor([&]{ return sink.HasPid(100); }, 3000), "later requests work with the stripped session id");
  }
  else if (mode == "nodelta") {
    t.SetChannel(TP1);
    CHECK(WaitFor([&]{ return t.HasLock(); }, 3000), "lock");
    t.SetPid(100, true);
    CHECK(WaitFor([&]{ return sink.HasPid(100); }, 3000), "PID arrives although addpids is refused");
    t.SetPid(200, true); t.SetPid(100, false);
    CHECK(WaitFor([&]{ return sink.HasPid(200); }, 3000), "second change uses the complete list directly");
    std::string log = LogText("server.log");
    CHECK(Count(log, "REFUSED PLAY 400") == 1, "addpids was only tried once");
    CHECK(log.find("PLAY pids=0,16,17,18,19,20,100") != std::string::npos, "complete PID list was sent");
  }
  else if (mode == "notune") {
    t.SetChannel(TP1);
    CHECK(WaitFor([&]{ return t.HasLock(); }, 3000), "lock");
    cTimeMs tm(0);
    t.SetChannel(TP2);
    CHECK(WaitFor([&]{ return t.HasLock() && sink.lastMarker == 12188; }, 4000), "retune works although the server refuses a PLAY with tuning in a running session");
    printf("   retune with new session: %d ms\n", (int)tm.Elapsed());
    CHECK(tm.Elapsed() < 2000, "no long waiting time");
    std::string log = LogText("server.log");
    CHECK(Count(log, "REQ TEARDOWN") >= 1 && Count(log, "REQ SETUP") == 2, "old session torn down, new one set up");
  }
  else if (mode == "busy") {
    t.SetChannel(TP1);
    CHECK(WaitFor([&]{ return sink.busy > 0; }, 4000), "sink is told that the server is busy (503)");
    CHECK(sink.busyHost == "127.0.0.1" && sink.busyPort == port, "with host and port of the server");
    if (argc > 3) {
      int port2 = atoi(argv[3]);
      t.SetChannel(TP1, 0, 0, "127.0.0.1", port2);
      CHECK(WaitFor([&]{ return t.HasLock(); }, 4000), "stream from the second server");
    }
  }
  printf("%s\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
  return fails ? 1 : 0;
}
