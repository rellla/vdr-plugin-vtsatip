/*
 * rtsp.c: vtsatip - minimal blocking RTSP client for SAT>IP
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

cVtRtsp::cVtRtsp(void)
: fdM(-1),
  hostM(""),
  portM(VTSATIP_DEFAULT_RTSP_PORT),
  cseqM(1),
  streamIdM(-1),
  timeoutS(30)
{
  sessionM[0] = 0;
  rxM[0] = 0;
}

cVtRtsp::~cVtRtsp()
{
  Close();
}

void cVtRtsp::Close(void)
{
  if (fdM >= 0) {
     close(fdM);
     fdM = -1;
     }
  cseqM = 1;
  sessionM[0] = 0;
  streamIdM = -1;
  timeoutS = 30;
}

bool cVtRtsp::WaitFd(short eventsP, int timeoutMsP)
{
  struct pollfd pfd;
  pfd.fd = fdM;
  pfd.events = eventsP;
  pfd.revents = 0;
  int r;
  do {
     r = poll(&pfd, 1, timeoutMsP);
     } while (r < 0 && errno == EINTR);
  if (r <= 0)
     return false;
  return (pfd.revents & eventsP) != 0;
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

const char *cVtRtsp::Header(const char *nameP) const
{
  // case insensitive header search, returns pointer to the value
  size_t nlen = strlen(nameP);
  const char *p = strstr(rxM, "\r\n");
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

bool cVtRtsp::Transact(const char *requestP, int timeoutMsP, int *statusP)
{
  vtdebug("RTSP >>> %s", requestP);
  if (!Send(requestP))
     return false;

  int used = 0;
  int bodyStart = -1;
  int bodyLen = 0;
  cTimeMs timer(timeoutMsP);
  rxM[0] = 0;
  while (true) {
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
  if (statusP)
     *statusP = status;
  if (status != 200) {
     vterror("RTSP: server answered with status %d", status);
     return false;
     }
  return true;
}

bool cVtRtsp::Options(void)
{
  char req[512];
  snprintf(req, sizeof(req), "OPTIONS rtsp://%s:%d/ RTSP/1.0\r\nCSeq: %d\r\n%s%s%s\r\n",
           *hostM, portM, cseqM++,
           sessionM[0] ? "Session: " : "", sessionM, sessionM[0] ? "\r\n" : "");
  return Transact(req, 3000);
}

bool cVtRtsp::Setup(const char *queryP, int rtpPortP, int rtcpPortP)
{
  char req[1536];
  int n = snprintf(req, sizeof(req), "SETUP rtsp://%s:%d/?%s RTSP/1.0\r\nCSeq: %d\r\n"
                   "Transport: RTP/AVP;unicast;client_port=%d-%d\r\n\r\n",
                   *hostM, portM, queryP, cseqM++, rtpPortP, rtcpPortP);
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
  vtdebug("RTSP: stream=%d session=%s timeout=%d", streamIdM, sessionM, timeoutS);
  return true;
}

bool cVtRtsp::Play(const char *queryP)
{
  if (streamIdM < 0)
     return false;
  char req[1536];
  int n = snprintf(req, sizeof(req), "PLAY rtsp://%s:%d/stream=%d%s%s RTSP/1.0\r\nCSeq: %d\r\nSession: %s\r\n\r\n",
                   *hostM, portM, streamIdM, (queryP && *queryP) ? "?" : "", queryP ? queryP : "",
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
  snprintf(req, sizeof(req), "TEARDOWN rtsp://%s:%d/stream=%d RTSP/1.0\r\nCSeq: %d\r\nSession: %s\r\n\r\n",
           *hostM, portM, streamIdM, cseqM++, sessionM);
  bool r = Transact(req, 2000);
  streamIdM = -1;
  sessionM[0] = 0;
  return r;
}
