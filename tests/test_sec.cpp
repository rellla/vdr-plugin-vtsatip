// Copyright (C) 2026 Andreas Baierl <ichgeh@imkreisrum.de>
// SPDX-License-Identifier: GPL-2.0-only
#include <vdr/tools.h>
#include <vdr/thread.h>
#include <vdr/ringbuffer.h>
#include <sys/socket.h>
#include <poll.h>
#include "common.h"
#include "sectionfilter.h"
int StubLogLevel = 0;
cVtSatipConfig VtSatipConfig;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++fails; } else printf("ok:   %s\n", msg); } while (0)

static void Ts(uchar *p, int pid, bool pusi, int cc, const uchar *payload, int len) {
  memset(p, 0xff, TS_SIZE);
  p[0] = 0x47; p[1] = (pusi ? 0x40 : 0) | (pid >> 8); p[2] = pid & 0xff; p[3] = 0x10 | cc;
  int off = 4; if (pusi) p[off++] = 0; // pointer field
  memcpy(p + off, payload, len);
}
static int Recv(int fd, uchar *b, int ms) { struct pollfd f = { fd, POLLIN, 0 }; if (poll(&f, 1, ms) <= 0) return -1; return recv(fd, b, 4096, 0); }

int main() {
  cVtSecHandler h(0, 1 << 20);
  int fd = h.Open(18, 0x4e, 0xff);
  int fd2 = h.Open(18, 0x50, 0xff);      // other table id on same pid must not match
  CHECK(fd >= 0 && fd2 >= 0 && h.Exists(18) && h.HasFilters(), "filters opened");
  CHECK(h.GetPid(fd) == 18, "GetPid");
  // section 1: short (tid 0x4e, length 12 bytes total: 3 header + 9)
  uchar s1[12] = { 0x4e, 0xF0, 9, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
  // section 2: 200 bytes spanning two TS packets
  uchar s2[200]; memset(s2, 0x55, sizeof(s2)); s2[0] = 0x4e; s2[1] = 0xF0; s2[2] = 197;
  uchar buf[TS_SIZE * 4];
  Ts(buf, 18, true, 0, s1, 12);
  Ts(buf + TS_SIZE, 18, true, 1, s2, 183);          // 183 bytes fit after pointer field
  Ts(buf + 2 * TS_SIZE, 18, false, 2, s2 + 183, 17);
  h.Write(buf, 3 * TS_SIZE);
  uchar rx[4096];
  int n = Recv(fd, rx, 1000);
  CHECK(n == 12 && !memcmp(rx, s1, 12), "short section delivered");
  n = Recv(fd, rx, 1000);
  CHECK(n == 200 && !memcmp(rx, s2, 200), "section spanning two TS packets delivered");
  CHECK(Recv(fd2, rx, 200) < 0, "filter with different table id gets nothing");
  // discontinuity: wrong cc must drop data until next PUSI
  uchar s3[200]; memset(s3, 0x33, sizeof(s3)); s3[0] = 0x4e; s3[1] = 0xF0; s3[2] = 197;
  Ts(buf, 18, true, 5, s3, 183);                    // cc jump (2 -> 5)
  Ts(buf + TS_SIZE, 18, false, 6, s3 + 183, 17);
  h.Write(buf, 2 * TS_SIZE);
  CHECK(Recv(fd, rx, 300) == 200, "section after cc jump still starts cleanly at PUSI");
  h.Close(fd);
  CHECK(h.Count() == 1, "filter closed");
  printf("%s\n", fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED");
  return fails;
}
