#include <vdr/tools.h>
#include <vdr/thread.h>
#include <vdr/ringbuffer.h>
#include <atomic>
#include <fstream>
#include <vector>
#include <string>
#include "common.h"
#include "tuner.h"
#include "pat.h"

int StubLogLevel = 0;
cVtSatipConfig VtSatipConfig;
static const char *LOGFILE = "server.log";
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++fails; } else printf("ok:   %s\n", msg); } while (0)

static void Mark(const char *s) { std::ofstream f(LOGFILE, std::ios::app); f << "0 MARK " << s << "\n"; }
static std::vector<std::string> Since(const char *mark) {
  std::ifstream f(LOGFILE); std::string l; std::vector<std::string> out; bool on = false;
  while (std::getline(f, l)) { if (l.find(std::string("MARK ") + mark) != std::string::npos) { on = true; out.clear(); continue; } if (on && l.find("REQ ") != std::string::npos) out.push_back(l.substr(l.find("REQ ") + 4)); }
  return out;
}
static bool Has(const std::vector<std::string> &v, const std::string &needle) { for (auto &s : v) if (s.find(needle) != std::string::npos) return true; return false; }

struct Sink : cVtTunerSink {
  std::atomic<unsigned long> packets{0};
  int SinkIndex() override { return 0; }
  void OnTsData(const uchar *, int len) override { packets += len / TS_SIZE; }
  void OnRetune() override { packets = 0; }
};
static bool WaitFor(std::function<bool()> f, int ms) { cTimeMs t(ms); while (!t.TimedOut()) { if (f()) return true; usleep(1000); } return false; }

static const char *TP1 = "src=1&freq=11493&pol=h&msys=dvbs2&mtype=8psk&sr=22000&fec=23";
static const char *TP2 = "src=1&freq=12188&pol=v&msys=dvbs&mtype=qpsk&sr=27500&fec=34";

static void PatTest() {
  // PAT: tsid 1, programs 0 -> NIT(16), 0x1234 -> pid 0x100, 0x1235 -> pid 0x200
  uchar s[] = { 0x00, 0xB0, 0x1D, 0x00, 0x01, 0xC1, 0x00, 0x00,
                0x00, 0x00, 0xE0, 0x10,  0x12, 0x34, 0xE1, 0x00,  0x12, 0x35, 0xE2, 0x00, 0, 0, 0, 0 };
  int total = 3 + 0x1D - 0; // header(3) + length
  (void)total;
  int len = 8 + 12 + 4;     // 24 bytes: section_length = 21 => 0x15
  s[2] = (uchar)(len - 3);
  uint32_t crc = cVtPatParser::Crc32(s, len - 4);
  s[len - 4] = crc >> 24; s[len - 3] = crc >> 16; s[len - 2] = crc >> 8; s[len - 1] = crc;
  std::map<int, int> p;
  CHECK(cVtPatParser::ParseSection(s, len, p) && p.size() == 2 && p[0x1234] == 0x100 && p[0x1235] == 0x200, "PAT parsed (SID -> PMT PID, NIT skipped)");
  s[10] ^= 0x01; // damage
  CHECK(!cVtPatParser::ParseSection(s, len, p), "PAT with wrong CRC is rejected");
  s[10] ^= 0x01;
  // through the TS path
  uchar ts[TS_SIZE]; memset(ts, 0xff, sizeof(ts));
  ts[0] = 0x47; ts[1] = 0x40; ts[2] = 0x00; ts[3] = 0x10; ts[4] = 0; memcpy(ts + 5, s, len);
  cVtPatParser parser; std::map<int, int> q;
  CHECK(parser.Process(ts, q) && q[0x1235] == 0x200, "PAT parsed from a TS packet");
  cVtPmtCache::Set(1, 11493, 0x1234, 0x100);
  CHECK(cVtPmtCache::Get(1, 11493, 0x1234) == 0x100 && cVtPmtCache::Get(1, 11493, 99) == 0, "PMT cache");
}

int main(int argc, char **argv) {
  PatTest();
  VtSatipConfig.host = "127.0.0.1"; VtSatipConfig.port = argc > 1 ? atoi(argv[1]) : 8554; VtSatipConfig.realtime = false;
  VtSatipConfig.ciExtension = true;

  { // --- restart mode (default) ---
    VtSatipConfig.ciRestart = true;
    Sink sink; cVtTuner t(sink);
    Mark("a encrypted channel, PMT unknown");
    t.SetChannel(TP1, 1, 0);
    CHECK(WaitFor([&]{ return t.HasLock(); }, 3000), "lock without PMT");
    auto r = Since("a encrypted channel, PMT unknown");
    CHECK(!Has(r, "x_pmt"), "no x_pmt while PMT is unknown");

    Mark("b PMT learned");
    t.SetPmt(256);
    CHECK(WaitFor([&]{ auto v = Since("b PMT learned"); return Has(v, "SETUP") && Has(v, "x_pmt=256&x_ci=1"); }, 4000), "session restarted with x_pmt/x_ci");
    CHECK(WaitFor([&]{ return t.HasLock(); }, 3000), "lock after restart");
    r = Since("b PMT learned");
    CHECK(Has(r, "TEARDOWN"), "old session torn down");
    CHECK(Has(r, "pids=0,16,17,18,19,20,256&x_pmt=256&x_ci=1"), "PMT PID is part of the PID list");

    Mark("c second service on same transponder");
    t.SetChannel(TP1, 1, 300);
    usleep(500000);
    r = Since("c second service on same transponder");
    CHECK(Has(r, "x_pmt=256,300&x_ci=1") && Has(r, "addpids=300"), "PMT list extended without restart");
    CHECK(!Has(r, "TEARDOWN") && !Has(r, "SETUP"), "no restart for another service of the same transponder");

    Mark("d free-to-air channel on other transponder");
    t.SetChannel(TP2, 0, 0);
    CHECK(WaitFor([&]{ auto v = Since("d free-to-air channel on other transponder"); return Has(v, "SETUP") && Has(v, "freq=12188"); }, 4000), "session restarted to release the CI");
    r = Since("d free-to-air channel on other transponder");
    CHECK(Has(r, "TEARDOWN") && !Has(r, "x_pmt"), "no x_pmt anymore");
    CHECK(WaitFor([&]{ return t.HasLock(); }, 3000), "lock on free-to-air channel");
  }
  { // --- no restart mode ---
    VtSatipConfig.ciRestart = false;
    Sink sink; cVtTuner t(sink);
    Mark("e no-restart mode");
    t.SetChannel(TP1, 1, 0);
    CHECK(WaitFor([&]{ return t.HasLock(); }, 3000), "lock");
    t.SetPmt(256);
    usleep(600000);
    auto r = Since("e no-restart mode");
    CHECK(Has(r, "addpids=256&x_pmt=256&x_ci=1"), "x_pmt sent in a plain PLAY");
    CHECK(!Has(r, "TEARDOWN"), "session kept");
  }
  printf("%s\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
  return fails ? 1 : 0;
}
