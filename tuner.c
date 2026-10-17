/*
 * tuner.c: vtsatip - SAT>IP tuner (RTSP control + RTP/RTCP receive)
 *
 * Copyright (C) 2026 Andreas Baierl <ichgeh@imkreisrum.de>
 *
 * The control flow follows the satip daemon of vtuner-ng (https://github.com/joed74/vtuner-ng),
 * Copyright (C) 2014 mc.fishdish@gmail.com, maintained by Jochen Dolze;
 * vtuner-ng is based on vtuner by Honza Petrous.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation. See the file COPYING.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <algorithm>
#include <map>
#include <vector>
#include <utility>
#include "tuner.h"

// --- cVtReceiver ------------------------------------------------------------

cVtReceiver::cVtReceiver(cVtTuner &tunerP)
: cThread("vtsatip rtp"),
  tunerM(tunerP),
  changersM(0),
  rtpFdM(-1),
  rtcpFdM(-1),
  rtpPortM(-1),
  multicastM(false),
  tcpRtspM(NULL),
  tcpRtpChM(0),
  tcpRtcpChM(1),
  packetsM(0),
  lostM(0),
  reorderedM(0),
  resetSeqM(false),
  seqValidM(false),
  expectedSeqM(0),
  resetTimingM(false),
  lastPacketUsM(0),
  gapRecentUsM(0),
  gapMaxUsM(0),
  rateKbitM(0),
  gapValidM(false),
  prevPacketUsM(0),
  windowStartUsM(0),
  windowGapUsM(0),
  windowBytesM(0)
{
}

uint64_t cVtReceiver::NowUs(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// Called for every received packet: measures the data rate and the longest
// pause between two packets, the usual reason for buffer underruns.
void cVtReceiver::TrackTiming(int bytesP)
{
  uint64_t now = NowUs();
  if (resetTimingM.exchange(false)) {
     gapValidM = false; // the pause caused by a retune is not a gap
     windowStartUsM = now;
     windowGapUsM = 0;
     windowBytesM = 0;
     }
  if (gapValidM) {
     unsigned long gap = (unsigned long)(now - prevPacketUsM);
     if (gap > windowGapUsM)
        windowGapUsM = gap;
     if (gap > gapMaxUsM.load())
        gapMaxUsM = gap;
     }
  gapValidM = true;
  prevPacketUsM = now;
  lastPacketUsM = now ? now : 1;
  windowBytesM += bytesP;
  if (!windowStartUsM)
     windowStartUsM = now;
  else if (now - windowStartUsM >= 2000000) {
     rateKbitM = (unsigned long)(windowBytesM * 8000ULL / (now - windowStartUsM));
     gapRecentUsM = windowGapUsM;
     windowStartUsM = now;
     windowGapUsM = 0;
     windowBytesM = 0;
     }
}

// time since the last packet in microseconds, 0 if nothing arrived since the (re)tune
static unsigned long VtSilenceUs(uint64_t nowP, uint64_t lastP)
{
  return (lastP && nowP > lastP) ? (unsigned long)(nowP - lastP) : 0;
}

unsigned long cVtReceiver::RateKbit(void) const
{
  return VtSilenceUs(NowUs(), lastPacketUsM.load()) > 3000000 ? 0 : rateKbitM.load();
}

unsigned long cVtReceiver::GapMs(void) const
{
  unsigned long silence = VtSilenceUs(NowUs(), lastPacketUsM.load());
  return std::max(gapRecentUsM.load(), silence) / 1000;
}

unsigned long cVtReceiver::GapMaxMs(void) const
{
  unsigned long silence = VtSilenceUs(NowUs(), lastPacketUsM.load());
  return std::max(gapMaxUsM.load(), silence) / 1000;
}

cVtReceiver::~cVtReceiver()
{
  Stop();
}

void cVtReceiver::CloseSocketsLocked(void)
{
  if (rtpFdM >= 0)
     close(rtpFdM);
  if (rtcpFdM >= 0)
     close(rtcpFdM);
  rtpFdM = rtcpFdM = -1;
  multicastM = false;
}

bool cVtReceiver::Open(void)
{
  cChangeLock lock(*this);
  CloseSocketsLocked();
  int start, stop;
  if (VtSatipConfig.portStart > 0 && VtSatipConfig.portStop > VtSatipConfig.portStart) {
     start = VtSatipConfig.portStart;
     stop = VtSatipConfig.portStop;
     }
  else {
     start = 45000;
     stop = 47000;
     }
  start += start % 2; // RTP needs an even port
  int range = (stop - start) / 2;
  if (range < 1)
     range = 1;
  int first = (int)(random() % range);
  for (int i = 0; i < range; ++i) {
      int port = start + 2 * ((first + i) % range);
      int rtp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
      int rtcp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
      if (rtp < 0 || rtcp < 0) {
         if (rtp >= 0) close(rtp);
         if (rtcp >= 0) close(rtcp);
         vterror("cannot create RTP sockets: %s", strerror(errno));
         return false;
         }
      struct sockaddr_in a;
      memset(&a, 0, sizeof(a));
      a.sin_family = AF_INET;
      a.sin_addr.s_addr = htonl(INADDR_ANY);
      a.sin_port = htons(port);
      bool ok = (bind(rtp, (struct sockaddr *)&a, sizeof(a)) == 0);
      if (ok) {
         a.sin_port = htons(port + 1);
         ok = (bind(rtcp, (struct sockaddr *)&a, sizeof(a)) == 0);
         }
      if (ok) {
         int size = VtSatipConfig.rcvBufSize > 0 ? VtSatipConfig.rcvBufSize : 2 * 1024 * 1024;
         setsockopt(rtp, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
         rtpFdM = rtp;
         rtcpFdM = rtcp;
         rtpPortM = port;
         return true;
         }
      close(rtp);
      close(rtcp);
      }
  vterror("no free RTP/RTCP port pair found");
  return false;
}

bool cVtReceiver::OpenMulticast(const std::string &groupP, const std::string &sourceP, int rtpPortP, int rtcpPortP)
{
  cChangeLock lock(*this);
  CloseSocketsLocked();
  struct in_addr group, source, iface;
  bool ok = inet_pton(AF_INET, groupP.c_str(), &group) == 1 && IN_MULTICAST(ntohl(group.s_addr)) && rtpPortP > 0 && rtcpPortP > 0;
  bool ssm = !sourceP.empty() && inet_pton(AF_INET, sourceP.c_str(), &source) == 1;
  iface.s_addr = htonl(INADDR_ANY);
  if (!VtSatipConfig.mcastInterface.empty() && inet_pton(AF_INET, VtSatipConfig.mcastInterface.c_str(), &iface) != 1)
     iface.s_addr = htonl(INADDR_ANY);
  int fds[2] = { -1, -1 };
  int ports[2] = { rtpPortP, rtcpPortP };
  for (int i = 0; ok && i < 2; ++i) {
      fds[i] = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
      if (fds[i] < 0) {
         ok = false;
         break;
         }
      int one = 1;
      setsockopt(fds[i], SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
      setsockopt(fds[i], SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
      int zero = 0; // only the groups joined by this socket, not those of other sockets of the host
      setsockopt(fds[i], IPPROTO_IP, IP_MULTICAST_ALL, &zero, sizeof(zero));
      struct sockaddr_in a;
      memset(&a, 0, sizeof(a));
      a.sin_family = AF_INET;
      a.sin_addr = group; // binding to the group address filters other streams on the same port
      a.sin_port = htons(ports[i]);
      ok = bind(fds[i], (struct sockaddr *)&a, sizeof(a)) == 0;
      if (ok) {
         if (ssm) {
            struct ip_mreq_source m;
            m.imr_multiaddr = group;
            m.imr_interface = iface;
            m.imr_sourceaddr = source;
            ok = setsockopt(fds[i], IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, &m, sizeof(m)) == 0;
            }
         else {
            struct ip_mreq m;
            m.imr_multiaddr = group;
            m.imr_interface = iface;
            ok = setsockopt(fds[i], IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) == 0;
            }
         }
      }
  if (!ok) {
     vterror("cannot join the multicast group %s port %d: %s", groupP.c_str(), rtpPortP, strerror(errno));
     for (int i = 0; i < 2; ++i)
         if (fds[i] >= 0)
            close(fds[i]);
     return false;
     }
  int size = VtSatipConfig.rcvBufSize > 0 ? VtSatipConfig.rcvBufSize : 2 * 1024 * 1024;
  setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
  rtpFdM = fds[0];
  rtcpFdM = fds[1];
  rtpPortM = rtpPortP;
  multicastM = true;
  return true;
}

void cVtReceiver::OnInterleaved(void *ctxP, int channelP, const uchar *dataP, int lengthP)
{
  cVtReceiver *self = (cVtReceiver *)ctxP;
  if (channelP == self->tcpRtpChM.load())
     self->ProcessRtp(dataP, lengthP);
  else if (channelP == self->tcpRtcpChM.load())
     self->ProcessRtcp(dataP, lengthP);
}

void cVtReceiver::SetTcp(cVtRtsp *rtspP, int rtpChannelP, int rtcpChannelP)
{
  if (rtspP) {
     tcpRtpChM = rtpChannelP;
     tcpRtcpChM = rtcpChannelP;
     rtspP->SetInterleavedHandler(&cVtReceiver::OnInterleaved, this);
     }
  tcpRtspM = rtspP;
}

void cVtReceiver::Stop(void)
{
  if (Running())
     Cancel(3);
  cChangeLock lock(*this);
  CloseSocketsLocked();
}

void cVtReceiver::ProcessRtp(const uchar *bufP, int lenP)
{
  if (lenP < 4)
     return;
  int hdr = 0;
  if (bufP[0] != TS_SYNC_BYTE) {
     // RFC 3550 / 2250
     if (lenP < 12 || ((bufP[0] >> 6) & 0x03) != 2)
        return;
     hdr = 12 + 4 * (bufP[0] & 0x0F);
     if (bufP[0] & 0x10) { // extension
        if (lenP < hdr + 4)
           return;
        hdr += 4 + 4 * ((bufP[hdr + 2] << 8) | bufP[hdr + 3]);
        }
     }
  if (hdr >= lenP)
     return;
  TrackTiming(lenP - hdr);
  if (hdr >= 12) { // a real RTP header: count lost and reordered packets
     unsigned seq = (unsigned)((bufP[2] << 8) | bufP[3]);
     ++packetsM;
     if (resetSeqM.exchange(false))
        seqValidM = false;
     if (seqValidM) {
        unsigned diff = (seq - expectedSeqM) & 0xFFFF;
        if (diff == 0 || diff < 0x8000) {
           lostM += diff;
           expectedSeqM = (seq + 1) & 0xFFFF;
           }
        else
           ++reorderedM; // late or duplicate, the expectation stays
        }
     else {
        seqValidM = true;
        expectedSeqM = (seq + 1) & 0xFFFF;
        }
     }
  int len = lenP - hdr;
  len -= len % TS_SIZE;
  if (len <= 0 || bufP[hdr] != TS_SYNC_BYTE)
     return;
  tunerM.OnTsPayload(bufP + hdr, len);
}

void cVtReceiver::ProcessRtcp(const uchar *bufP, int lenP)
{
  // walk through the compound RTCP packet, look for APP "SES1"
  int offset = 0;
  while (lenP - offset >= 16) {
        const uchar *p = bufP + offset;
        int v = (p[0] >> 6) & 0x03;
        int pt = p[1];
        int length = (((p[2] << 8) | p[3]) + 1) * 4;
        if (length <= 0 || offset + length > lenP)
           break;
        if (v == 2 && pt == 204 && p[8] == 'S' && p[9] == 'E' && p[10] == 'S' && p[11] == '1') {
           int slen = (p[14] << 8) | p[15];
           if (slen > 0 && slen <= length - 16 && slen < 512) {
              char info[520];
              memcpy(info, p + 16, slen);
              info[slen] = 0;
              tunerM.OnRtcpInfo(info);
              }
           }
        offset += length;
        }
}

void cVtReceiver::Action(void)
{
  if (VtSatipConfig.realtime) {
     // like the vtuner-ng satip daemon: lowest realtime priority for the RTP thread
     struct sched_param sp;
     sp.sched_priority = sched_get_priority_min(SCHED_FIFO) + 1;
     if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0)
        vtinfo("RTP thread of device %d runs with realtime priority %d", tunerM.indexM, sp.sched_priority);
     else
        vtdebug("no realtime scheduling for RTP thread of device %d: %s", tunerM.indexM, strerror(errno));
     }
  uchar buf[4096];
  while (Running()) {
        cVtRtsp *tcp = tcpRtspM.load();
        if (tcp) { // RTP over TCP: the tuner thread owns the connection, we read the stream from it
           if (!tcp->Pump(50))
              cCondWait::SleepMs(20);
           // not fair either: let a pending RTSP request go first
           for (int i = 0; i < 200 && tcp->WaitingThreads() > 0; ++i)
               cCondWait::SleepMs(1);
           continue;
           }
        sockMutexM.Lock(); // sockets are replaced under this mutex (unicast <-> multicast)
        if (rtpFdM < 0 || rtcpFdM < 0) {
           sockMutexM.Unlock();
           cCondWait::SleepMs(20);
           continue;
           }
        struct pollfd fds[2];
        fds[0].fd = rtpFdM;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = rtcpFdM;
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        int r = poll(fds, 2, 50);
        if (r > 0) {
           if (fds[0].revents & POLLIN) {
              // drain the socket completely, this keeps the latency low
              for (int i = 0; i < 256; ++i) {
                  ssize_t n = recv(rtpFdM, buf, sizeof(buf), MSG_DONTWAIT);
                  if (n <= 0)
                     break;
                  ProcessRtp(buf, (int)n);
                  }
              }
           if (fds[1].revents & POLLIN) {
              ssize_t n = recv(rtcpFdM, buf, sizeof(buf), MSG_DONTWAIT);
              if (n > 0)
                 ProcessRtcp(buf, (int)n);
              }
           }
        sockMutexM.Unlock();
        // a mutex is not fair: give a thread which wants to replace the sockets time to get it
        for (int i = 0; i < 200 && changersM.load() > 0; ++i)
            cCondWait::SleepMs(1);
        }
}

// --- cVtTuner ---------------------------------------------------------------

cVtTuner::cVtTuner(cVtTunerSink &sinkP)
: cThread(*cString::sprintf("vtsatip#%d tuner", sinkP.SinkIndex())),
  sinkM(sinkP),
  indexM(sinkP.SinkIndex()),
  tunePendingM(false),
  wantSeqM(0),
  wantReleaseM(false),
  wantCiSlotM(0),
  wantPortM(0),
  activePortM(0),
  sessionHadPmtM(false),
  fullPidListM(false),
  multicastActiveM(false),
  tcpActiveM(false),
  failStatusM(0),
  keepAliveM(),
  receiverM(*this),
  acceptM(false),
  lockM(false),
  strengthM(-1),
  qualityM(-1),
  levelM(0),
  frontendM(-1),
  sessionUpM(false),
  transportModeM(VTSATIP_TRANSPORT_UNICAST),
  bytesM(0)
{
  if (receiverM.Open()) {
     receiverM.Start();
     Start();
     }
}

cVtTuner::~cVtTuner()
{
  mutexM.Lock();
  wantReleaseM = true;
  tunePendingM = false;
  condM.Broadcast();
  mutexM.Unlock();
  if (Running())
     Cancel(8);
  receiverM.Stop();
  rtspM.Close();
}

std::set<int> cVtTuner::WantedPids(void)
{
  if (VtSatipConfig.allPids) // the server sends everything, VDR picks what it needs
     return std::set<int>(&kAllPids, &kAllPids + 1);
  std::set<int> pids = wantPidsM;
  pids.insert(wantPmtM.begin(), wantPmtM.end()); // the CI needs the PMT in the stream
  for (int i = 0; i < VTSATIP_STDPID_COUNT; ++i)
      pids.insert(VtSatipStdPids[i]);
  return pids;
}

std::string cVtTuner::JoinPids(const std::set<int> &pidsP)
{
  if (pidsP.count(kAllPids))
     return "all";
  std::string s;
  for (std::set<int>::const_iterator it = pidsP.begin(); it != pidsP.end(); ++it) {
      if (!s.empty())
         s += ",";
      s += std::to_string(*it);
      }
  return s.empty() ? std::string("none") : s;
}

void cVtTuner::SetChannel(const char *paramsP, int ciSlotP, int pmtPidP, const char *hostP, int portP)
{
  mutexM.Lock();
  if (isempty(paramsP)) {
     wantReleaseM = true;
     tunePendingM = false;
     wantParamsM.clear();
     wantPmtM.clear();
     wantCiSlotM = 0;
     wantHostM.clear();
     }
  else {
     wantHostM = hostP ? hostP : "";
     wantPortM = portP;
     if (wantParamsM != paramsP)
        wantPmtM.clear(); // other transponder: the old services are gone
     if (ciSlotP > 0) {
        wantCiSlotM = ciSlotP;
        if (pmtPidP > 0 && pmtPidP < 8191)
           wantPmtM.insert(pmtPidP);
        }
     wantParamsM = paramsP;
     tunePendingM = true;
     wantReleaseM = false;
     ++wantSeqM;
     }
  condM.Broadcast();
  mutexM.Unlock();
}

void cVtTuner::SetPmt(int pmtPidP)
{
  if (pmtPidP <= 0 || pmtPidP >= 8191)
     return;
  mutexM.Lock();
  if (wantCiSlotM > 0 && wantPmtM.insert(pmtPidP).second)
     condM.Broadcast();
  mutexM.Unlock();
}

void cVtTuner::SetPid(int pidP, bool onP)
{
  if (pidP < 0 || pidP > 8191)
     return;
  mutexM.Lock();
  bool changed;
  if (onP)
     changed = wantPidsM.insert(pidP).second;
  else
     changed = wantPidsM.erase(pidP) > 0;
  if (changed)
     condM.Broadcast();
  mutexM.Unlock();
}

void cVtTuner::Release(void)
{
  mutexM.Lock();
  wantReleaseM = true;
  tunePendingM = false;
  wantParamsM.clear();
  wantPmtM.clear();
  wantCiSlotM = 0;
  wantHostM.clear();
  condM.Broadcast();
  mutexM.Unlock();
}

double cVtTuner::SignalStrengthDBm(void) const
{
  int level = levelM.load();
  return (level > 0) ? 40.0 * (level - 32) / 192.0 - 65.0 : 0.0;
}

void cVtTuner::DropSession(void)
{
  receiverM.ResetSequence();
  acceptM = false;
  lockM = false;
  sessionUpM = false;
  if (rtspM.IsOpen()) {
     if (rtspM.HasSession())
        rtspM.Teardown(); // in TCP mode the receiver may still pump; the RTSP object serialises that
     rtspM.Close();
     }
  activePidsM.clear();
  activePmtM.clear();
  sessionHadPmtM = false;
  fullPidListM = false;
  if (tcpActiveM) {
     receiverM.SetTcp(NULL);
     tcpActiveM = false;
     }
  if (multicastActiveM) {
     receiverM.ResetUnicast(); // leave the group
     multicastActiveM = false;
     }
  activeParamsM.clear();
  activeHostM.clear();
  strengthM = qualityM = -1;
  levelM = 0;
  frontendM = -1;
}

void cVtTuner::DoRelease(void)
{
  vtdebug("device %d: release", indexM);
  DropSession();
  sinkM.OnRetune();
}

// Bring the query into the form vtuner-ng sends (order of the keys, fec and
// plts always present for DVB-S/S2), because some servers are picky.
static std::string NormalizeParams(const std::string &inP)
{
  std::vector<std::pair<std::string, std::string> > kv;
  size_t pos = 0;
  while (pos <= inP.size()) {
        size_t amp = inP.find('&', pos);
        std::string item = inP.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (!item.empty()) {
           size_t eq = item.find('=');
           if (eq == std::string::npos)
              kv.push_back(std::make_pair(item, std::string()));
           else
              kv.push_back(std::make_pair(item.substr(0, eq), item.substr(eq + 1)));
           }
        if (amp == std::string::npos)
           break;
        pos = amp + 1;
        }
  std::map<std::string, std::string> m;
  for (size_t i = 0; i < kv.size(); ++i)
      m[kv[i].first] = kv[i].second;
  bool sat = m.count("msys") && (m["msys"] == "dvbs" || m["msys"] == "dvbs2");
  if (sat) {
     if (!m.count("fec"))
        m["fec"] = "auto";
     if (m["msys"] == "dvbs2" && !m.count("plts"))
        m["plts"] = "on";
     }
  static const char *order[] = { "src", "fe", "freq", "pol", "msys", "mtype", "sr", "fec", "ro", "plts", NULL };
  std::string out;
  for (int i = 0; order[i]; ++i) {
      if (m.count(order[i])) {
         out += (out.empty() ? "" : "&") + std::string(order[i]) + "=" + m[order[i]];
         m.erase(order[i]);
         }
      }
  for (size_t i = 0; i < kv.size(); ++i) { // everything else in its original order
      std::map<std::string, std::string>::iterator it = m.find(kv[i].first);
      if (it != m.end()) {
         out += (out.empty() ? "" : "&") + it->first + (it->second.empty() ? "" : "=" + it->second);
         m.erase(it);
         }
      }
  return out;
}

std::string cVtTuner::CiParams(const std::set<int> &pmtP, int ciSlotP)
{
  if (pmtP.empty() || ciSlotP <= 0)
     return std::string();
  return "&x_pmt=" + JoinPids(pmtP) + "&x_ci=" + std::to_string(ciSlotP);
}

// connects and sets the session up; multicast is requested if configured and falls back to unicast
// (per server) when the server does not offer it
bool cVtTuner::OpenSession(const std::string &hostP, int portP, const std::string &paramsP, const std::set<int> &pmtP, int ciSlotP)
{
  DropSession();
  std::string key = hostP + ":" + std::to_string(portP);
  std::string query = paramsP + "&pids=none" + CiParams(pmtP, ciSlotP);
  int modes[3];
  int n = 0;
  if (VtSatipConfig.transport == VTSATIP_TRANSPORT_TCP && !noTcpM.count(key))
     modes[n++] = VTSATIP_TRANSPORT_TCP;
  else if (VtSatipConfig.transport == VTSATIP_TRANSPORT_MULTICAST && !noMulticastM.count(key))
     modes[n++] = VTSATIP_TRANSPORT_MULTICAST;
  modes[n++] = VTSATIP_TRANSPORT_UNICAST;
  for (int i = 0; i < n; ++i) {
      if (!rtspM.IsOpen()) {
         if (!rtspM.Connect(hostP.c_str(), portP))
            return false;
         if (!rtspM.Options()) {
            failStatusM = rtspM.LastStatus();
            DropSession();
            return false;
            }
         }
      bool ok = rtspM.Setup(query.c_str(), receiverM.RtpPort(), receiverM.RtcpPort(), modes[i]);
      if (ok && modes[i] == VTSATIP_TRANSPORT_MULTICAST) {
         const cVtTransport &tr = rtspM.Transport();
         if (tr.multicast && receiverM.OpenMulticast(tr.destination, tr.source, tr.rtpPort, tr.rtcpPort))
            multicastActiveM = true;
         else {
            // the server gave no usable group: close this session and start again with unicast
            ok = false;
            if (rtspM.HasSession())
               rtspM.Teardown();
            rtspM.Close();
            noMulticastM.insert(key);
            vtinfo("device %d: server %s offers no multicast, using unicast", indexM, key.c_str());
            continue;
            }
         }
      if (ok && modes[i] == VTSATIP_TRANSPORT_TCP) {
         const cVtTransport &tr = rtspM.Transport();
         if (tr.tcp) {
            receiverM.SetTcp(&rtspM, tr.rtpChannel, tr.rtcpChannel);
            tcpActiveM = true;
            }
         else {
            // the server answered with something else than RTP/AVP/TCP
            ok = false;
            if (rtspM.HasSession())
               rtspM.Teardown();
            rtspM.Close();
            noTcpM.insert(key);
            vtinfo("device %d: server %s does not do RTP over TCP, using UDP", indexM, key.c_str());
            continue;
            }
         }
      if (ok) {
         transportModeM = tcpActiveM ? VTSATIP_TRANSPORT_TCP : (multicastActiveM ? VTSATIP_TRANSPORT_MULTICAST : VTSATIP_TRANSPORT_UNICAST);
         sessionUpM = true;
         sessionHadPmtM = !pmtP.empty();
         return true;
         }
      failStatusM = rtspM.LastStatus();
      if (modes[i] == VTSATIP_TRANSPORT_UNICAST || failStatusM == 503 || failStatusM == 0)
         break;
      if (modes[i] == VTSATIP_TRANSPORT_TCP)
         noTcpM.insert(key);
      else
         noMulticastM.insert(key); // e.g. 461 unsupported transport
      vtinfo("device %d: server %s refused %s (status %d), using unicast", indexM, key.c_str(),
             modes[i] == VTSATIP_TRANSPORT_TCP ? "RTP over TCP" : "multicast", failStatusM);
      }
  DropSession();
  return false;
}

bool cVtTuner::DoTune(const std::string &paramsP, const std::set<int> &pidsP, const std::set<int> &pmtP, int ciSlotP,
                      const std::string &hostP, int portP)
{
  std::string params = paramsP;
  int fe = VtSatipConfig.frontend[indexM];
  if (fe > 0 && params.find("fe=") == std::string::npos)
     params += "&fe=" + std::to_string(fe);
  params = NormalizeParams(params);

  std::string host = hostP.empty() ? std::string(*VtSatipConfig.host) : hostP;
  int port = (hostP.empty() || portP <= 0) ? VtSatipConfig.port : portP;
  if (rtspM.HasSession() && (host != activeHostM || port != activePortM)) {
     vtdebug("device %d: server changes to %s:%d", indexM, host.c_str(), port);
     DropSession();
     }

  // Some servers (Digital Devices Octopus) can only attach the CI when the stream
  // was started with x_pmt, and release it only when the session is torn down.
  if (VtSatipConfig.ciRestart && rtspM.HasSession() && sessionHadPmtM != !pmtP.empty() && params != activeParamsM) {
     vtdebug("device %d: CI state changes, restarting the session", indexM);
     DropSession();
     }

  // same transponder, session is running -> nothing to do, keep the lock
  if (rtspM.HasSession() && params == activeParamsM) {
     vtdebug("device %d: transponder unchanged", indexM);
     return true;
     }

  // from now on discard everything that still belongs to the old tuning
  acceptM = false;
  lockM = false;
  strengthM = qualityM = -1;
  levelM = 0;
  sinkM.OnRetune();
  receiverM.ResetSequence();

  // A server may refuse a retune inside a running session (e.g. FRITZ!WLAN Repeater DVB-C wants a
  // TEARDOWN first). One retry with a brand-new session handles that without any server list.
  failStatusM = 0;
  std::string query = params + "&pids=" + JoinPids(pidsP) + CiParams(pmtP, ciSlotP);
  for (int attempt = 0; ; ++attempt) {
      bool fresh = false;
      if (!rtspM.HasSession()) {
         if (!OpenSession(host, port, params, pmtP, ciSlotP))
            return false;
         fresh = true;
         }
      vtdebug("device %d: tune %s", indexM, query.c_str());
      if (rtspM.Play(query.c_str()))
         break;
      failStatusM = rtspM.LastStatus();
      DropSession();
      if (fresh || attempt > 0 || failStatusM < 400)
         return false;
      vtinfo("device %d: server refused the retune (status %d), starting a new session", indexM, failStatusM);
      }
  activeParamsM = params;
  activeHostM = host;
  activePortM = port;
  activePidsM = pidsP;
  activePmtM = pmtP;
  int ka = rtspM.SessionTimeout() - 5;
  if (ka < eMinKeepAliveSec)
     ka = eMinKeepAliveSec;
  keepAliveM.Set(ka * 1000);
  acceptM = true;
  return true;
}

bool cVtTuner::DoPids(const std::set<int> &wantP, const std::set<int> &pmtP, int ciSlotP)
{
  if (wantP.count(kAllPids) != activePidsM.count(kAllPids)) {
     // switching between "all" and a PID list (setup change): send the complete list
     std::string q = "pids=" + JoinPids(wantP) + CiParams(pmtP, ciSlotP);
     vtdebug("device %d: pids %s", indexM, q.c_str());
     if (!rtspM.Play(q.c_str()))
        return false;
     activePidsM = wantP;
     activePmtM = pmtP;
     return true;
     }
  std::string add, del;
  for (std::set<int>::const_iterator it = wantP.begin(); it != wantP.end(); ++it)
      if (!activePidsM.count(*it))
         add += (add.empty() ? "" : ",") + std::to_string(*it);
  for (std::set<int>::const_iterator it = activePidsM.begin(); it != activePidsM.end(); ++it)
      if (!wantP.count(*it))
         del += (del.empty() ? "" : ",") + std::to_string(*it);
  std::string ci = CiParams(pmtP, ciSlotP);
  if (add.empty() && del.empty() && (ci.empty() || pmtP == activePmtM))
     return true;
  std::string query;
  if (!add.empty())
     query = "addpids=" + add;
  if (!del.empty())
     query += std::string(query.empty() ? "" : "&") + "delpids=" + del;
  if (!ci.empty())
     query += std::string(query.empty() ? "" : "&") + ci.substr(1);
  if (fullPidListM)
     query = "pids=" + JoinPids(wantP) + ci;
  vtdebug("device %d: pids %s", indexM, query.c_str());
  if (!rtspM.Play(query.c_str())) {
     // some servers (FRITZ!WLAN Repeater DVB-C) do not know addpids/delpids: send the complete list instead
     if (fullPidListM || rtspM.LastStatus() < 400 || (add.empty() && del.empty()))
        return false;
     query = "pids=" + JoinPids(wantP) + ci;
     vtinfo("device %d: server refused addpids/delpids, using complete PID lists", indexM);
     if (!rtspM.Play(query.c_str()))
        return false;
     fullPidListM = true;
     }
  activePidsM = wantP;
  activePmtM = pmtP;
  return true;
}

void cVtTuner::Action(void)
{
  vtdebug("device %d: tuner thread started", indexM);
  mutexM.Lock();
  while (Running()) {
        if (wantReleaseM) {
           wantReleaseM = false;
           mutexM.Unlock();
           DoRelease();
           mutexM.Lock();
           continue;
           }
        if (tunePendingM) {
           std::string params = wantParamsM;
           unsigned seq = wantSeqM;
           std::set<int> pids = WantedPids();
           std::set<int> pmt = wantPmtM;
           int ci = wantCiSlotM;
           std::string host = wantHostM;
           int port = wantPortM;
           mutexM.Unlock();
           bool ok = DoTune(params, pids, pmt, ci, host, port);
           if (!ok && failStatusM == 503) {
              std::string h = host.empty() ? std::string(*VtSatipConfig.host) : host;
              sinkM.OnServerBusy(h, (host.empty() || port <= 0) ? VtSatipConfig.port : port);
              }
           mutexM.Lock();
           if (ok) {
              if (wantSeqM == seq)
                 tunePendingM = false;
              }
           else if (Running() && wantSeqM == seq && !wantReleaseM)
              condM.TimedWait(mutexM, eRetryDelayMs);
           continue;
           }
        if (rtspM.HasSession()) {
           std::set<int> want = WantedPids();
           if (VtSatipConfig.ciRestart && wantCiSlotM > 0 && !wantPmtM.empty() && !sessionHadPmtM) {
              // the CI can only be attached to a stream that was started with x_pmt
              vtdebug("device %d: PMT known now, restarting the session for the CI", indexM);
              mutexM.Unlock();
              DropSession();
              mutexM.Lock();
              tunePendingM = !wantParamsM.empty();
              continue;
              }
           if (want != activePidsM || (wantCiSlotM > 0 && wantPmtM != activePmtM)) {
              std::set<int> pmt = wantPmtM;
              int ci = wantCiSlotM;
              mutexM.Unlock();
              bool ok = DoPids(want, pmt, ci);
              mutexM.Lock();
              if (!ok) {
                 vterror("device %d: PID update failed, will retune", indexM);
                 mutexM.Unlock();
                 DropSession();
                 mutexM.Lock();
                 tunePendingM = !wantParamsM.empty();
                 }
              continue;
              }
           if (keepAliveM.TimedOut()) {
              mutexM.Unlock();
              bool ok = rtspM.Options();
              mutexM.Lock();
              if (ok) {
                 int ka = rtspM.SessionTimeout() - 5;
                 if (ka < eMinKeepAliveSec)
                    ka = eMinKeepAliveSec;
                 keepAliveM.Set(ka * 1000);
                 }
              else {
                 vterror("device %d: keep-alive failed, will retune", indexM);
                 mutexM.Unlock();
                 DropSession();
                 mutexM.Lock();
                 tunePendingM = !wantParamsM.empty();
                 }
              continue;
              }
           }
        condM.TimedWait(mutexM, eIdlePollMs);
        }
  mutexM.Unlock();
  DropSession();
  vtdebug("device %d: tuner thread stopped", indexM);
}

void cVtTuner::OnTsPayload(const uchar *dataP, int lenP)
{
  if (!acceptM.load())
     return; // stale data of a previous tuning
  if (!lockM.load()) {
     // vtuner-ng: the first real TS packet means "we have a lock"
     for (int i = 0; i + TS_SIZE <= lenP; i += TS_SIZE) {
         if (VtTsPid(dataP + i) != 0x1FFF) {
            lockM = true;
            break;
            }
         }
     }
  bytesM += lenP;
  sinkM.OnTsData(dataP, lenP);
}

void cVtTuner::OnRtcpInfo(const char *infoP)
{
  // ver=1.0;src=1;tuner=<fe>,<level>,<lock>,<quality>,...
  const char *c = strstr(infoP, ";tuner=");
  if (!c)
     return;
  c += 7;
  frontendM = atoi(c);
  c = strchr(c, ',');
  if (!c)
     return;
  int level = atoi(++c);
  if (level > 255)
     level = 255;
  c = strchr(c, ',');
  if (!c)
     return;
  ++c; // lock flag is deliberately ignored, see OnTsPayload()
  c = strchr(c, ',');
  if (!c)
     return;
  int quality = atoi(++c);
  if (quality > 15)
     quality = 15;
  levelM = level;
  strengthM = (level >= 0) ? (int)(0.5 + level * 100.0 / 255.0) : -1;
  qualityM = (quality >= 0) ? (int)(0.5 + quality * 100.0 / 15.0) : -1;
}

cString cVtTuner::GetInformation(void)
{
  mutexM.Lock();
  std::string params = wantParamsM;
  size_t pids = wantPidsM.size();
  std::string pmt = wantPmtM.empty() ? std::string("-") : JoinPids(wantPmtM);
  int ci = wantCiSlotM;
  mutexM.Unlock();
  return cString::sprintf("session=%s accept=%d lock=%d fe=%d strength=%d quality=%d bytes=%lu rtp=%lu lost=%lu reordered=%lu rate=%.2fMbit/s gap=%lums gapmax=%lums pids=%zu ci=%d pmt=%s params=%s",
                          sessionUpM.load() ? "up" : "down", (int)acceptM.load(), (int)lockM.load(),
                          frontendM.load(), strengthM.load(), qualityM.load(), bytesM.load(),
                          receiverM.Packets(), receiverM.Lost(), receiverM.Reordered(),
                          RateKbit() / 1000.0, GapMs(), GapMaxMs(), pids, ci, pmt.c_str(), params.c_str());
}
