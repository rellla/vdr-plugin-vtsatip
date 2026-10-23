// SPDX-License-Identifier: GPL-2.0-only
//
// Soak test: runs the tuner, the receiver and the section filters for a while
// against tests/fake_satip.py and looks for anything that builds up over time.
// Every round it retunes and changes PIDs; it also opens and closes section
// filters, releases the stream, deletes and recreates the tuner, and kills and
// restarts the server (SIGKILL), so the reconnect paths are exercised too.
//
// Fails if open file descriptors are left over at the end, if the resident
// memory grows by more than SOAK_RSS_SLACK_KB (default 2048) after the
// warm-up, or if no data arrives at the end (the last reconnect did not work).
//
// Usage: soak <port> <seconds> <udp|tcp> [server log]   (see run_soak.sh)
#include <vdr/tools.h>
#include <vdr/thread.h>
#include <vdr/ringbuffer.h>
#include <atomic>
#include <map>
#include <vector>
#include <dirent.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "common.h"
#include "tuner.h"
#include "sectionfilter.h"
#include "pat.h"

int StubLogLevel = 0;
cVtSatipConfig VtSatipConfig;

struct Sink : cVtTunerSink {
  cVtSecHandler *sec = NULL;
  cVtPatParser pat;
  std::atomic<unsigned long> bytes{0};
  int SinkIndex() override { return 0; }
  void OnTsData(const uchar *d, int len) override {
    bytes += len;
    if (sec && sec->HasFilters())
       sec->Write(d, len);
    std::map<int, int> programs;
    for (int i = 0; i + TS_SIZE <= len; i += TS_SIZE)
        pat.Process(d + i, programs);
  }
  void OnRetune() override { pat.Reset(); if (sec) sec->Clear(); }
};

static const char *Transponders[] = {
  "src=1&freq=11493&pol=h&msys=dvbs2&mtype=8psk&sr=22000&fec=23",
  "src=1&freq=12188&pol=v&msys=dvbs&mtype=qpsk&sr=27500&fec=34" };

static long RssKb(void)
{
  long pages = 0, rss = 0;
  FILE *f = fopen("/proc/self/statm", "r");
  if (f) {
     if (fscanf(f, "%ld %ld", &pages, &rss) != 2)
        rss = 0;
     fclose(f);
     }
  return rss * (sysconf(_SC_PAGESIZE) / 1024);
}

static int OpenFds(void)
{
  int n = 0;
  DIR *d = opendir("/proc/self/fd");
  if (!d)
     return -1;
  while (readdir(d))
        ++n;
  closedir(d);
  return n - 3; // ".", ".." and the directory itself
}

static pid_t StartServer(int portP, const char *logP, const char *modeP)
{
  pid_t p = fork();
  if (p == 0) {
     execlp("python3", "python3", "fake_satip.py", *cString::sprintf("%d", portP), logP, "30", modeP, (char *)NULL);
     _exit(1);
     }
  usleep(400000);
  return p;
}

static void StopServer(pid_t pidP)
{
  if (pidP > 0) {
     kill(pidP, SIGKILL);
     waitpid(pidP, NULL, 0);
     }
}

int main(int argc, char **argv)
{
  if (argc < 4) {
     fprintf(stderr, "usage: %s <port> <seconds> <udp|tcp> [server log]\n", argv[0]);
     return 2;
     }
  int port = atoi(argv[1]);
  int seconds = atoi(argv[2]);
  bool tcp = !strcmp(argv[3], "tcp");
  std::string log = argc > 4 ? argv[4] : std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/vtsatip-soak-server.log";
  long rssSlack = getenv("SOAK_RSS_SLACK_KB") ? atol(getenv("SOAK_RSS_SLACK_KB")) : 2048;
  if (seconds < 60)
     seconds = 60; // the memory baseline is taken after 30 s

  VtSatipConfig.host = "127.0.0.1";
  VtSatipConfig.port = port;
  VtSatipConfig.realtime = false;
  VtSatipConfig.stallTimeoutMs = 700;
  if (tcp)
     VtSatipConfig.transport = VTSATIP_TRANSPORT_TCP;
  const char *mode = tcp ? "tcp" : "lose";

  int fdsStart = OpenFds();
  unsigned long round = 0, restarts = 0, recreated = 0, lastBytes = 0;
  long rssBase = 0, rssMax = 0;
  bool dataAtEnd = false;
  { // everything that owns sockets lives in this block (the PAT parser of the sink has a socket pair, too)
  pid_t server = StartServer(port, log.c_str(), mode);
  Sink sink;
  sink.sec = new cVtSecHandler(0, 1024 * 1024);
  cVtTuner *tuner = new cVtTuner(sink);

  cTimeMs total(seconds * 1000), sample(10000), restart(15000);
  std::vector<int> filters;
  printf("soak: %s, %d s, server log %s\n", tcp ? "RTP over TCP" : "UDP", seconds, log.c_str());
  while (!total.TimedOut()) {
        ++round;
        tuner->SetChannel(Transponders[round % 2]);
        for (int k = 0; k < 6; ++k)
            tuner->SetPid(100 + (int)((round * 7 + k) % 40), (k + round) % 3 != 0);
        if (filters.size() < 8) {
           int h = sink.sec->Open(0, 0x00, 0xFF);
           if (h >= 0)
              filters.push_back(h);
           }
        else {
           sink.sec->Close(filters.front());
           filters.erase(filters.begin());
           }
        char drain[4096];
        for (size_t i = 0; i < filters.size(); ++i)
            while (recv(filters[i], drain, sizeof(drain), MSG_DONTWAIT) > 0)
                  ;
        if (round % 25 == 0)
           tuner->Release();
        if (round % 200 == 0) { // like a device that is deleted and created again
           delete tuner;
           tuner = new cVtTuner(sink);
           ++recreated;
           }
        // no restart in the last 15 s: the stream must be running again at the end
        if (restart.TimedOut() && total.Elapsed() < (uint64_t)(seconds - 15) * 1000) {
           StopServer(server);
           usleep(1500000);
           server = StartServer(port, log.c_str(), mode);
           ++restarts;
           restart.Set(15000);
           }
        usleep(150000);
        if (sample.TimedOut()) {
           long rss = RssKb();
           unsigned long bytes = sink.bytes.load();
           if (!rssBase && total.Elapsed() >= 30000)
              rssBase = rss;
           if (rssBase && rss > rssMax)
              rssMax = rss;
           dataAtEnd = bytes > lastBytes;
           lastBytes = bytes;
           printf("t=%3lus round=%5lu rss=%6ld kB fds=%3d bytes=%lu restarts=%lu recreated=%lu\n",
                  (unsigned long)(total.Elapsed() / 1000), round, rss, OpenFds(), bytes, restarts, recreated);
           fflush(stdout);
           sample.Set(10000);
           }
        }
  delete tuner;
  delete sink.sec;
  StopServer(server);
  }

  int fdsEnd = OpenFds();
  int fails = 0;
  if (fdsEnd != fdsStart) {
     printf("FAIL: %d file descriptors open at the end, %d at the start\n", fdsEnd, fdsStart);
     ++fails;
     }
  else
     printf("ok:   no file descriptors left over (%d)\n", fdsEnd);
  if (rssMax - rssBase > rssSlack) {
     printf("FAIL: resident memory grew by %ld kB after the warm-up (%ld -> %ld kB, allowed %ld)\n", rssMax - rssBase, rssBase, rssMax, rssSlack);
     ++fails;
     }
  else
     printf("ok:   resident memory stable after the warm-up (%ld -> %ld kB)\n", rssBase, rssMax);
  if (!dataAtEnd) {
     printf("FAIL: no data in the last 10 s, the stream did not come back\n");
     ++fails;
     }
  else
     printf("ok:   data flows at the end (%lu rounds, %lu server restarts, %lu tuners recreated)\n", round, restarts, recreated);
  printf(fails ? "SOME TESTS FAILED\n" : "ALL TESTS PASSED\n");
  return fails ? 1 : 0;
}
