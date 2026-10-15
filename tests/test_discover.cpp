#include <vdr/tools.h>
#include <vdr/thread.h>
#include <vdr/ringbuffer.h>
#include <atomic>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "common.h"
#include "tuner.h"
#include "servers.h"
#include "discover.h"

int StubLogLevel = 0;
cVtSatipConfig VtSatipConfig;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++fails; } else printf("ok:   %s\n", msg); } while (0)
static bool WaitFor(std::function<bool()> f, int ms) { cTimeMs t(ms); while (!t.TimedOut()) { if (f()) return true; usleep(2000); } return false; }

struct Sink : cVtTunerSink {
  std::atomic<unsigned long> packets{0};
  int SinkIndex() override { return 0; }
  void OnTsData(const uchar *, int len) override { packets += len / TS_SIZE; }
  void OnRetune() override {}
};

static void Units() {
  int caps[3];
  CHECK(cVtServers::ParseCaps("DVBS2-4,DVBT2-2", caps) && caps[0] == 4 && caps[1] == 2 && caps[2] == 0, "X_SATIPCAP parsed");
  CHECK(cVtServers::ParseCaps("DVBS-2,DVBS2-4,DVBC-1", caps) && caps[0] == 4 && caps[2] == 1, "same medium: larger number wins");
  CHECK(cVtServers::ParseCaps("DVBS2", caps) && caps[0] == 1, "missing count means 1");
  CHECK(!cVtServers::ParseCaps("ATSC-2", caps), "unsupported systems only -> rejected");

  cVtSsdpMessage m;
  CHECK(cVtDiscover::ParseSsdp("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=900\r\nLOCATION: http://192.168.0.5:8888/octonet.xml\r\nST: urn:ses-com:device:SatIPServer:1\r\nUSN: uuid:abc-123::urn:ses-com:device:SatIPServer:1\r\n\r\n", m)
        && m.kind == cVtSsdpMessage::eResponse && m.uuid == "abc-123" && m.maxAge == 900 && m.location == "http://192.168.0.5:8888/octonet.xml", "M-SEARCH response parsed");
  CHECK(cVtDiscover::ParseSsdp("NOTIFY * HTTP/1.1\r\nNT: urn:ses-com:device:SatIPServer:1\r\nNTS: ssdp:alive\r\nLOCATION: http://1.2.3.4/x.xml\r\nUSN: uuid:zzz::urn:ses-com:device:SatIPServer:1\r\n\r\n", m)
        && m.kind == cVtSsdpMessage::eAlive && m.uuid == "zzz", "NOTIFY alive parsed");
  CHECK(cVtDiscover::ParseSsdp("NOTIFY * HTTP/1.1\r\nNT: urn:ses-com:device:SatIPServer:1\r\nNTS: ssdp:byebye\r\nUSN: uuid:zzz::urn:ses-com:device:SatIPServer:1\r\n\r\n", m)
        && m.kind == cVtSsdpMessage::eByebye && m.uuid == "zzz", "NOTIFY byebye parsed (no location needed)");
  CHECK(!cVtDiscover::ParseSsdp("NOTIFY * HTTP/1.1\r\nNT: urn:schemas-upnp-org:device:MediaServer:1\r\nNTS: ssdp:alive\r\nLOCATION: http://x/\r\n\r\n", m), "other UPnP devices are ignored");
  CHECK(!cVtDiscover::ParseSsdp("garbage", m), "garbage is ignored");

  std::string xml = "<?xml version=\"1.0\"?><root><device><friendlyName>Octo &amp; Net</friendlyName><satip:X_SATIPCAP xmlns:satip=\"u\">DVBS2-8,DVBT2-2</satip:X_SATIPCAP></device></root>";
  cVtDescription d;
  CHECK(cVtDiscover::ParseDescription(xml, d) && d.name == "Octo & Net" && d.caps == "DVBS2-8,DVBT2-2", "device description parsed (namespace prefix, entities)");
  std::string host, path; int port;
  CHECK(cVtDiscover::SplitUrl("http://10.0.0.1:8888/a/b.xml", host, port, path) && host == "10.0.0.1" && port == 8888 && path == "/a/b.xml", "URL split");
  CHECK(cVtDiscover::SplitUrl("http://box/desc.xml", host, port, path) && port == 80, "URL default port");

  // frontend accounting: A has 2 sat, B has 1 sat + 1 terr
  cVtServers s;
  int a[3] = {2, 0, 0}, b[3] = {1, 1, 0};
  s.AddManual("A", 554, "A", a); s.AddManual("B", 554, "B", b);
  std::string h; int p;
  CHECK(s.Assign(0, 0, h, p) && h == "A", "device 0 -> A");
  CHECK(s.Assign(1, 0, h, p) && h == "A", "device 1 -> A");
  CHECK(s.Assign(2, 0, h, p) && h == "B", "device 2 -> B (A is full)");
  CHECK(!s.Assign(3, 0, h, p), "no 4th satellite frontend");
  CHECK(s.Assign(3, 1, h, p) && h == "B", "terrestrial -> B");
  CHECK(s.Assign(0, 0, h, p) && h == "A", "device keeps its server");
  s.Unassign(0);
  CHECK(s.Assign(3, 0, h, p) && h == "A", "freed frontend is reused (device 3 switches to satellite)");
  CHECK(!s.Provides(2), "no cable");
  { // busy handling: A (2 sat) and B (1 sat, 1 terr)
    cVtServers q; int a2[3] = {2, 0, 0}, b2[3] = {1, 1, 0};
    q.AddManual("A", 554, "A", a2); q.AddManual("B", 554, "B", b2);
    std::string h2; int p2;
    CHECK(q.Assign(0, 0, h2, p2) && h2 == "A", "busy test: device 0 on A");
    CHECK(q.Reassign(0, 0, "A", 554, h2, p2) && h2 == "B", "A answered 503: device 0 moves to B");
    CHECK(!q.Assign(1, 0, h2, p2) || h2 == "B", "A is skipped while it is marked busy");
    CHECK(q.Assign(1, 1, h2, p2) && h2 == "B", "other media are not affected by the busy mark");
    q.Unassign(0); q.Unassign(1);
    CHECK(!q.Reassign(2, 0, "B", 554, h2, p2) && q.Assign(2, 0, h2, p2) == false, "no alternative: Reassign fails, both busy");
  }
  { // satellite position filters: A = 19.2E only, B = 13E only
    cVtServers q; int a3[3] = {2, 0, 0}, b3[3] = {2, 0, 0};
    q.AddManual("A", 554, "A", a3, cVtServers::ParseFilters("s19.2e"));
    q.AddManual("B", 554, "B", b3, cVtServers::ParseFilters("S13E, S28.2E"));
    std::string h3; int p3;
    CHECK(cVtServers::ParseFilters("S19.2E,S13E").size() == 2 && cVtServers::ParseFilters("").empty(), "filter list parsed");
    CHECK(q.Provides(0, "S13E") && q.Provides(0, "S28.2E") && !q.Provides(0, "S0.8W") && q.Provides(0, NULL), "Provides honours the satellite position");
    CHECK(q.Assign(0, 0, h3, p3, NULL, "S13E") && h3 == "B", "13E goes to B");
    CHECK(q.Assign(1, 0, h3, p3, NULL, "S19.2E") && h3 == "A", "19.2E goes to A");
    CHECK(q.Assign(0, 0, h3, p3, NULL, "S19.2E") && h3 == "A", "device moves to the server of the new position");
    CHECK(!q.Assign(2, 0, h3, p3, NULL, "S0.8W"), "no server for 0.8W");
  }
  CHECK(!s.Remove("A") , "manual servers cannot be removed by UPnP");
  int c[3] = {1, 0, 0};
  CHECK(!s.Update("uuid-x", "B", 554, "dup", "", c, 100) && s.Count() == 2, "UPnP duplicate of a manual server is ignored");
}

int main(int argc, char **argv) {
  Units();
  int rtspPort = argc > 1 ? atoi(argv[1]) : 8554;
  int ssdp = argc > 2 ? atoi(argv[2]) : 19000;
  int notify = ssdp + 1;
  VtSatipConfig.realtime = false;

  cVtServers *servers = cVtServers::Get();
  cVtDiscoverConfig cfg; cfg.searchAddr = "127.0.0.1"; cfg.searchPort = ssdp; cfg.listenPort = notify; cfg.probeIntervalS = 60;
  cVtDiscover disc(cfg, servers);
  disc.Start();
  CHECK(WaitFor([&]{ return servers->Count() == 1; }, 6000), "server found by M-SEARCH");
  CHECK(servers->Provides(VTSATIP_MEDIUM_SAT) && servers->Provides(VTSATIP_MEDIUM_TERR) && !servers->Provides(VTSATIP_MEDIUM_CABLE), "capabilities from X_SATIPCAP");
  printf("%s", *servers->Info());

  std::string host; int port = 0, port2 = 0; std::string name;
  CHECK(servers->Assign(0, VTSATIP_MEDIUM_SAT, host, port, &name) && host == "127.0.0.1" && port == rtspPort, "RTSP port taken from X-SATIP-RTSP-Port");
  CHECK(name == "Fake & Test SAT>IP", "friendly name decoded");
  std::string h2;
  CHECK(servers->Assign(1, VTSATIP_MEDIUM_SAT, h2, port2) && !servers->Assign(2, VTSATIP_MEDIUM_SAT, h2, port2), "two satellite frontends announced, third is refused");

  { // stream from the discovered server
    Sink sink; cVtTuner t(sink);
    t.SetChannel("src=1&freq=11493&pol=h&msys=dvbs2&mtype=8psk&sr=22000&fec=23", 0, 0, host.c_str(), port);
    CHECK(WaitFor([&]{ return t.HasLock(); }, 4000) && WaitFor([&]{ return sink.packets > 10; }, 2000), "streaming from the UPnP-discovered server");
  }

  // byebye / alive through the NOTIFY listener
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  struct sockaddr_in to; memset(&to, 0, sizeof(to)); to.sin_family = AF_INET; to.sin_port = htons(notify); inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
  const char *bye = "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nNT: urn:ses-com:device:SatIPServer:1\r\nNTS: ssdp:byebye\r\nUSN: uuid:11111111-2222-3333-4444-555555555555::urn:ses-com:device:SatIPServer:1\r\n\r\n";
  sendto(fd, bye, strlen(bye), 0, (struct sockaddr *)&to, sizeof(to));
  CHECK(WaitFor([&]{ return servers->Count() == 0; }, 3000), "byebye removes the server");
  const char *alive = "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nCACHE-CONTROL: max-age=1800\r\nLOCATION: http://127.0.0.1:%d/desc.xml\r\nNT: urn:ses-com:device:SatIPServer:1\r\nNTS: ssdp:alive\r\nUSN: uuid:11111111-2222-3333-4444-555555555555::urn:ses-com:device:SatIPServer:1\r\n\r\n";
  char msg[600]; snprintf(msg, sizeof(msg), alive, (argc > 3 ? atoi(argv[3]) : 18089));
  sendto(fd, msg, strlen(msg), 0, (struct sockaddr *)&to, sizeof(to));
  CHECK(WaitFor([&]{ return servers->Count() == 1; }, 3000), "alive notification adds the server again");
  close(fd);

  disc.Rescan();
  usleep(300000);
  CHECK(servers->Count() == 1, "rescan does not create duplicates");
  disc.Stop();
  printf("%s\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
  return fails ? 1 : 0;
}
