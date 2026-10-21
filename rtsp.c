/*
 * rtsp.c: vtsatip - minimal blocking RTSP client for SAT>IP
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
#include <netdb.h>
#include <poll.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "rtsp.h"

// "host" or "host:port" - the default port is left out, like vtuner-ng does
static cString HostPart(const char *hostP, int portP)
{
  return portP == VTSATIP_DEFAULT_RTSP_PORT ? cString(hostP) : cString::sprintf("%s:%d", hostP, portP);
}

cVtRtsp::cVtRtsp(void)
: fdM(-1),
  abortM(false),
  hostM(""),
  portM(VTSATIP_DEFAULT_RTSP_PORT),
  cseqM(1),
  streamIdM(-1),
  timeoutS(30),
  lastStatusM(0),
  wantersM(0),
  tcpModeM(false),
  brokenM(false),
  ilenM(0),
  handlerM(NULL),
  handlerCtxM(NULL)
{
  sessionM[0] = 0;
  rxM[0] = 0;
}

cVtRtsp::~cVtRtsp()
{
  Close();
}

void cVtRtsp::LockIo(void)
{
  ++wantersM;
  ioM.lock();
  --wantersM;
}

void cVtRtsp::Close(void)
{
  LockIo();
  if (fdM >= 0) {
     close(fdM);
     fdM = -1;
     }
  cseqM = 1;
  sessionM[0] = 0;
  streamIdM = -1;
  timeoutS = 30;
  lastStatusM = 0;
  transportM = cVtTransport();
  tcpModeM = false;
  brokenM = false;
  ilenM = 0;
  ioM.unlock();
}

bool cVtRtsp::WaitFd(short eventsP, int timeoutMsP)
{
  struct pollfd pfd;
  pfd.fd = fdM;
  pfd.events = eventsP;
  pfd.revents = 0;
  // wait in slices so that SetAbort() takes effect quickly
  int left = timeoutMsP < 0 ? 0 : timeoutMsP;
  for (;;) {
      if (abortM.load())
         return false;
      int slice = left < 100 ? left : 100;
      int r = poll(&pfd, 1, slice);
      if (r < 0) {
         if (errno == EINTR)
            continue;
         return false;
         }
      if (r > 0)
         return (pfd.revents & eventsP) != 0;
      left -= slice;
      if (left <= 0)
         return false;
      }
}

bool cVtRtsp::Connect(const char *hostP, int portP, int timeoutMsP)
{
  Close();
  hostM = hostP;
  portM = portP;

  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET; // RTP sockets are IPv4 only
  hints.ai_socktype = SOCK_STREAM;
  char service[16];
  snprintf(service, sizeof(service), "%d", portP);
  int rc = getaddrinfo(hostP, service, &hints, &res);
  if (rc != 0) {
     vterror("RTSP: getaddrinfo(%s): %s", hostP, gai_strerror(rc));
     return false;
     }
  bool ok = false;
  for (struct addrinfo *rp = res; rp && !ok; rp = rp->ai_next) {
      int fd = socket(rp->ai_family, rp->ai_socktype | SOCK_CLOEXEC, rp->ai_protocol);
      if (fd < 0)
         continue;
      fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      if (VtSatipConfig.rcvBufSize > 0) { // large bursts of interleaved data
         int sz = VtSatipConfig.rcvBufSize;
         setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
         }
      fdM = fd;
      if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
         ok = true;
      else if (errno == EINPROGRESS && WaitFd(POLLOUT, timeoutMsP)) {
         int err = 0;
         socklen_t len = sizeof(err);
         if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
            ok = true;
         }
      if (!ok) {
         close(fd);
         fdM = -1;
         }
      }
  freeaddrinfo(res);
  if (!ok)
     vterror("RTSP: cannot connect to %s:%d", hostP, portP);
  return ok;
}

bool cVtRtsp::Send(const char *requestP)
{
  if (fdM < 0)
     return false;
  size_t len = strlen(requestP);
  size_t done = 0;
  while (done < len) {
        ssize_t n = send(fdM, requestP + done, len - done, MSG_NOSIGNAL);
        if (n < 0) {
           if (errno == EINTR)
              continue;
           if (errno == EAGAIN && WaitFd(POLLOUT, 1000))
              continue;
           return false;
           }
        done += n;
        }
  return true;
}

static const char *HeaderIn(const char *textP, const char *nameP)
{
  // case insensitive header search, returns pointer to the value
  size_t nlen = strlen(nameP);
  const char *p = strstr(textP, "\r\n");
  while (p) {
        p += 2;
        if (!strncasecmp(p, nameP, nlen) && p[nlen] == ':') {
           p += nlen + 1;
           while (*p == ' ' || *p == '\t')
                 ++p;
           return p;
           }
        p = strstr(p, "\r\n");
        }
  return NULL;
}

const char *cVtRtsp::Header(const char *nameP) const
{
  return HeaderIn(rxM, nameP);
}

bool cVtRtsp::Transact(const char *requestP, int timeoutMsP, int *statusP)
{
  vtdebug("RTSP >>> %s", requestP);
  lastStatusM = 0;
  LockIo();
  std::lock_guard<std::mutex> guard(ioM, std::adopt_lock);
  if (!Send(requestP))
     return false;

  int used = 0;
  int bodyStart = -1;
  int bodyLen = 0;
  cTimeMs timer(timeoutMsP);
  rxM[0] = 0;
  if (tcpModeM && !TransactTcp(timeoutMsP))
     return false;
  while (!tcpModeM) {
        int left = timeoutMsP - (int)timer.Elapsed();
        if (left <= 0 || !WaitFd(POLLIN, left)) {
           vterror("RTSP: response timeout");
           return false;
           }
        ssize_t n = recv(fdM, rxM + used, eMaxResponse - used, 0);
        if (n == 0) {
           vterror("RTSP: connection closed by server");
           return false;
           }
        if (n < 0) {
           if (errno == EAGAIN || errno == EINTR)
              continue;
           return false;
           }
        used += n;
        rxM[used] = 0;
        if (bodyStart < 0) {
           char *e = strstr(rxM, "\r\n\r\n");
           if (!e) {
              if (used >= eMaxResponse) {
                 vterror("RTSP: response too large");
                 return false;
                 }
              continue;
              }
           bodyStart = (int)(e - rxM) + 4;
           const char *cl = Header("Content-Length");
           bodyLen = cl ? atoi(cl) : 0;
           if (bodyLen < 0)
              bodyLen = 0;
           }
        if (used - bodyStart >= bodyLen)
           break;
        if (used >= eMaxResponse) // body is not needed, just stop reading
           break;
        }
  vtdebug("RTSP <<< %s", rxM);
  int status = 0;
  if (sscanf(rxM, "RTSP/%*s %d", &status) != 1)
     return false;
  lastStatusM = status;
  if (statusP)
     *statusP = status;
  if (status != 200) {
     char line[300];
     size_t n = strcspn(requestP, "\r\n");
     if (n >= sizeof(line)) n = sizeof(line) - 1;
     memcpy(line, requestP, n);
     line[n] = 0;
     vterror("RTSP: server answered with status %d to: %s", status, line);
     return false;
     }
  return true;
}

bool cVtRtsp::Options(void)
{
  char req[512];
  snprintf(req, sizeof(req), "OPTIONS rtsp://%s/ RTSP/1.0\r\nCSeq: %d\r\n%s%s%s\r\n",
           *HostPart(*hostM, portM), cseqM++,
           sessionM[0] ? "Session: " : "", sessionM, sessionM[0] ? "\r\n" : "");
  return Transact(req, 3000);
}

// Transport: RTP/AVP;multicast;destination=239.0.1.2;port=5004-5005;ttl=2[;source=10.0.0.1]
static void ParseTransport(const char *valueP, cVtTransport &tP)
{
  tP = cVtTransport();
  std::string line(valueP, strcspn(valueP, "\r\n"));
  size_t pos = 0;
  while (pos <= line.size()) {
        size_t semi = line.find(';', pos);
        std::string tok = line.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        if (!strncasecmp(tok.c_str(), "RTP/AVP/TCP", 11))
           tP.tcp = true;
        else if (!strncasecmp(tok.c_str(), "interleaved=", 12)) {
           int a = 0, b = 0;
           int k = sscanf(tok.c_str() + 12, "%d-%d", &a, &b);
           if (k >= 1) {
              tP.rtpChannel = a;
              tP.rtcpChannel = (k >= 2) ? b : a + 1;
              }
           }
        else if (tok == "multicast")
           tP.multicast = true;
        else if (!strncasecmp(tok.c_str(), "destination=", 12))
           tP.destination = tok.substr(12);
        else if (!strncasecmp(tok.c_str(), "source=", 7))
           tP.source = tok.substr(7);
        else if (!strncasecmp(tok.c_str(), "port=", 5)) {
           int a = 0, b = 0;
           if (sscanf(tok.c_str() + 5, "%d-%d", &a, &b) >= 1) {
              tP.rtpPort = a;
              tP.rtcpPort = b ? b : a + 1;
              }
           }
        if (semi == std::string::npos)
           break;
        pos = semi + 1;
        }
  if (tP.multicast && tP.destination.empty())
     tP.multicast = false; // not usable without a group
}

bool cVtRtsp::Setup(const char *queryP, int rtpPortP, int rtcpPortP, int modeP)
{
  char req[1536];
  char transport[96];
  tcpModeM = (modeP == VTSATIP_TRANSPORT_TCP);
  if (tcpModeM)
     snprintf(transport, sizeof(transport), "RTP/AVP/TCP;unicast;interleaved=0-1");
  else if (modeP == VTSATIP_TRANSPORT_MULTICAST)
     snprintf(transport, sizeof(transport), "RTP/AVP;multicast");
  else
     snprintf(transport, sizeof(transport), "RTP/AVP;unicast;client_port=%d-%d", rtpPortP, rtcpPortP);
  int n = snprintf(req, sizeof(req), "SETUP rtsp://%s/?%s RTSP/1.0\r\nCSeq: %d\r\n"
                   "Transport: %s\r\n\r\n",
                   *HostPart(*hostM, portM), queryP, cseqM++, transport);
  if (n <= 0 || n >= (int)sizeof(req))
     return false;
  if (!Transact(req, 5000))
     return false;
  const char *s = Header("com.ses.streamID");
  int id = -1;
  if (!s || sscanf(s, "%d", &id) != 1 || id < 0) {
     vterror("RTSP: SETUP response carries no stream id");
     return false;
     }
  const char *sess = Header("Session");
  if (!sess) {
     vterror("RTSP: SETUP response carries no session");
     return false;
     }
  size_t i = 0;
  while (sess[i] && sess[i] != ';' && sess[i] != '\r' && sess[i] != '\n' && i < sizeof(sessionM) - 1) {
        sessionM[i] = sess[i];
        ++i;
        }
  sessionM[i] = 0;
  const char *to = strstr(sess, "timeout=");
  const char *eol = strstr(sess, "\r\n");
  timeoutS = 30;
  if (to && (!eol || to < eol)) {
     int t = atoi(to + 8);
     if (t > 0)
        timeoutS = t;
     }
  streamIdM = id;
  const char *tr = Header("Transport");
  if (tr)
     ParseTransport(tr, transportM);
  else
     transportM = cVtTransport();
  if (tcpModeM && !transportM.tcp)
     tcpModeM = false; // the server chose UDP; the caller sees it in Transport()
  vtdebug("RTSP: stream=%d session=%s timeout=%d%s%s", streamIdM, sessionM, timeoutS, transportM.multicast ? " multicast" : "", transportM.tcp ? " tcp" : "");
  return true;
}

// Some servers (GSS.box, Telestar Digibit R1, Inverto IDL-400, Triax TSS 400, ...) hand out session ids
// with leading zeros but only accept them without. If a request is refused, try again without the zeros.
bool cVtRtsp::Play(const char *queryP)
{
  if (PlayOnce(queryP))
     return true;
  if (lastStatusM >= 400 && sessionM[0] == '0' && sessionM[1]) {
     char saved[sizeof(sessionM)];
     strcpy(saved, sessionM);
     const char *p = sessionM;
     while (*p == '0' && p[1])
           ++p;
     memmove(sessionM, p, strlen(p) + 1);
     vtinfo("RTSP: retrying with session id '%s' (server dislikes leading zeros)", sessionM);
     if (PlayOnce(queryP))
        return true;
     strcpy(sessionM, saved);
     }
  return false;
}

bool cVtRtsp::PlayOnce(const char *queryP)
{
  if (streamIdM < 0)
     return false;
  char req[1536];
  int n = snprintf(req, sizeof(req), "PLAY rtsp://%s/stream=%d%s%s RTSP/1.0\r\nCSeq: %d\r\nSession: %s\r\n\r\n",
                   *HostPart(*hostM, portM), streamIdM, (queryP && *queryP) ? "?" : "", queryP ? queryP : "",
                   cseqM++, sessionM);
  if (n <= 0 || n >= (int)sizeof(req))
     return false;
  return Transact(req, 5000);
}

bool cVtRtsp::Teardown(void)
{
  if (streamIdM < 0 || fdM < 0)
     return true;
  char req[512];
  snprintf(req, sizeof(req), "TEARDOWN rtsp://%s/stream=%d RTSP/1.0\r\nCSeq: %d\r\nSession: %s\r\n\r\n",
           *HostPart(*hostM, portM), streamIdM, cseqM++, sessionM);
  bool r = Transact(req, 2000);
  streamIdM = -1;
  sessionM[0] = 0;
  return r;
}

// --- RTP over TCP -------------------------------------------------------------------------------------------------
// Everything below runs with ioM held.

// Walks through the received bytes: '$'-frames go to the handler, RTSP messages are skipped, or - when wantResponseP -
// the first one is copied to rxM.
int cVtRtsp::ParseInput(bool wantResponseP)
{
  int result = 0;
  size_t pos = 0;
  while (pos < ilenM) {
        const uchar *p = &ibufM[pos];
        size_t avail = ilenM - pos;
        if (p[0] == '$') {
           if (avail < 4)
              break;
           size_t len = (size_t)((p[2] << 8) | p[3]);
           if (avail < 4 + len)
              break;
           if (handlerM)
              handlerM(handlerCtxM, p[1], p + 4, (int)len);
           pos += 4 + len;
           continue;
           }
        if (p[0] != 'R') { // neither a frame nor "RTSP/1.0": resynchronise
           ++pos;
           continue;
           }
        const uchar *e = (const uchar *)memmem(p, avail, "\r\n\r\n", 4);
        if (!e) {
           if (avail > (size_t)eMaxResponse) // nonsense
              pos = ilenM;
           break;
           }
        size_t hdr = (size_t)(e - p) + 4;
        char tmp[eMaxResponse + 1];
        size_t cp = hdr < (size_t)eMaxResponse ? hdr : (size_t)eMaxResponse;
        memcpy(tmp, p, cp);
        tmp[cp] = 0;
        const char *cl = HeaderIn(tmp, "Content-Length");
        size_t total = hdr + (cl && atoi(cl) > 0 ? (size_t)atoi(cl) : 0);
        if (avail < total)
           break;
        pos += total;
        if (wantResponseP) {
           memcpy(rxM, tmp, cp + 1);
           result = 1;
           break;
           }
        }
  if (pos > 0) {
     if (pos < ilenM)
        memmove(&ibufM[0], &ibufM[pos], ilenM - pos);
     ilenM -= pos;
     }
  return result;
}

bool cVtRtsp::TransactTcp(int timeoutMsP)
{
  if (ibufM.empty())
     ibufM.resize(128 * 1024);
  cTimeMs timer(timeoutMsP);
  rxM[0] = 0;
  while (true) {
        if (ParseInput(true) > 0)
           return true;
        if (ilenM >= ibufM.size()) { // a frame larger than the buffer, cannot be: start over
           ilenM = 0;
           continue;
           }
        int left = timeoutMsP - (int)timer.Elapsed();
        if (left <= 0 || !WaitFd(POLLIN, left)) {
           vterror("RTSP: response timeout");
           return false;
           }
        ssize_t n = recv(fdM, &ibufM[ilenM], ibufM.size() - ilenM, 0);
        if (n == 0) {
           vterror("RTSP: connection closed by server");
           return false;
           }
        if (n < 0) {
           if (errno == EAGAIN || errno == EINTR)
              continue;
           return false;
           }
        ilenM += (size_t)n;
        }
}

bool cVtRtsp::Pump(int timeoutMsP)
{
  LockIo();
  std::lock_guard<std::mutex> guard(ioM, std::adopt_lock);
  if (fdM < 0 || !tcpModeM || brokenM)
     return false;
  if (ibufM.empty())
     ibufM.resize(128 * 1024);
  ParseInput(false);
  if (ilenM >= ibufM.size())
     ilenM = 0;
  if (WaitFd(POLLIN, timeoutMsP)) {
     ssize_t n = recv(fdM, &ibufM[ilenM], ibufM.size() - ilenM, MSG_DONTWAIT);
     if (n == 0) {
        vterror("RTSP: connection closed by server");
        brokenM = true;
        return false;
        }
     if (n > 0) {
        ilenM += (size_t)n;
        ParseInput(false);
        }
     }
  return true;
}
