/*
 * discover.c: vtsatip - SAT>IP server discovery (UPnP/SSDP)
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
#include <arpa/inet.h>
#include "discover.h"

#define SATIP_ST "urn:ses-com:device:SatIPServer:1"

// --- parsing helpers --------------------------------------------------------

static std::string Trim(const std::string &sP)
{
  size_t b = sP.find_first_not_of(" \t\r\n");
  if (b == std::string::npos)
     return std::string();
  size_t e = sP.find_last_not_of(" \t\r\n");
  return sP.substr(b, e - b + 1);
}

static bool HeaderIs(const std::string &lineP, const char *nameP, std::string &valueP)
{
  size_t n = strlen(nameP);
  if (lineP.size() > n && lineP[n] == ':' && !strncasecmp(lineP.c_str(), nameP, n)) {
     valueP = Trim(lineP.substr(n + 1));
     return true;
     }
  return false;
}

static std::string XmlDecode(const std::string &sP)
{
  static const struct { const char *from; const char *to; } map[] = {
    { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&apos;", "'" }, { NULL, NULL } };
  std::string s = sP;
  for (int i = 0; map[i].from; ++i) {
      size_t pos = 0;
      while ((pos = s.find(map[i].from, pos)) != std::string::npos) {
            s.replace(pos, strlen(map[i].from), map[i].to);
            pos += strlen(map[i].to);
            }
      }
  return s;
}

bool cVtDiscover::ParseSsdp(const char *dataP, cVtSsdpMessage &msgP)
{
  msgP = cVtSsdpMessage();
  std::string data(dataP ? dataP : "");
  size_t pos = 0;
  bool first = true;
  bool st = false;
  std::string nts;
  while (pos < data.size()) {
        size_t eol = data.find('\n', pos);
        std::string line = Trim(data.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos));
        pos = (eol == std::string::npos) ? data.size() : eol + 1;
        if (line.empty())
           continue;
        if (first) {
           first = false;
           if (!strncasecmp(line.c_str(), "HTTP/1.1 200", 12) || !strncasecmp(line.c_str(), "HTTP/1.0 200", 12))
              msgP.kind = cVtSsdpMessage::eResponse;
           else if (!strncasecmp(line.c_str(), "NOTIFY", 6))
              msgP.kind = cVtSsdpMessage::eAlive; // refined by NTS below
           else
              return false;
           continue;
           }
        std::string v;
        if (HeaderIs(line, "LOCATION", v))
           msgP.location = v;
        else if (HeaderIs(line, "USN", v))
           msgP.usn = v;
        else if (HeaderIs(line, "ST", v) || HeaderIs(line, "NT", v)) {
           if (v.find(SATIP_ST) != std::string::npos)
              st = true;
           }
        else if (HeaderIs(line, "NTS", v))
           nts = v;
        else if (HeaderIs(line, "CACHE-CONTROL", v)) {
           const char *m = strcasestr(v.c_str(), "max-age");
           if (m && (m = strchr(m, '=')) && atoi(m + 1) > 0)
              msgP.maxAge = atoi(m + 1);
           }
        }
  if (!st)
     return false;
  if (msgP.kind == cVtSsdpMessage::eAlive && !strcasecmp(nts.c_str(), "ssdp:byebye"))
     msgP.kind = cVtSsdpMessage::eByebye;
  else if (msgP.kind == cVtSsdpMessage::eAlive && strcasecmp(nts.c_str(), "ssdp:alive"))
     return false;
  // USN: uuid:xxxx::urn:ses-com:device:SatIPServer:1
  std::string id = msgP.usn;
  size_t sep = id.find("::");
  if (sep != std::string::npos)
     id = id.substr(0, sep);
  if (!strncasecmp(id.c_str(), "uuid:", 5))
     id = id.substr(5);
  msgP.uuid = id.empty() ? msgP.location : id;
  if (msgP.kind != cVtSsdpMessage::eByebye && msgP.location.empty())
     return false;
  return true;
}

std::string cVtDiscover::XmlText(const std::string &xmlP, const char *nameP)
{
  // finds <name> or <prefix:name>, returns the text up to the next tag
  size_t pos = 0;
  std::string suffix = std::string(":") + nameP;
  while ((pos = xmlP.find('<', pos)) != std::string::npos) {
        size_t end = xmlP.find('>', pos);
        if (end == std::string::npos)
           break;
        std::string tag = xmlP.substr(pos + 1, end - pos - 1);
        size_t sp = tag.find_first_of(" \t\r\n/");
        bool selfClosing = !tag.empty() && tag[tag.size() - 1] == '/';
        if (sp != std::string::npos)
           tag = tag.substr(0, sp);
        if (!tag.empty() && tag[0] != '/' && tag[0] != '?' && tag[0] != '!' &&
            (tag == nameP || (tag.size() > suffix.size() && tag.compare(tag.size() - suffix.size(), suffix.size(), suffix) == 0))) {
           if (selfClosing)
              return std::string();
           size_t next = xmlP.find('<', end + 1);
           return XmlDecode(Trim(xmlP.substr(end + 1, next == std::string::npos ? std::string::npos : next - end - 1)));
           }
        pos = end + 1;
        }
  return std::string();
}

bool cVtDiscover::ParseDescription(const std::string &xmlP, cVtDescription &descP)
{
  descP.name = XmlText(xmlP, "friendlyName");
  descP.model = XmlText(xmlP, "modelName");
  descP.caps = XmlText(xmlP, "X_SATIPCAP");
  return !descP.caps.empty() || !descP.name.empty();
}

bool cVtDiscover::SplitUrl(const std::string &urlP, std::string &hostP, int &portP, std::string &pathP)
{
  if (strncasecmp(urlP.c_str(), "http://", 7))
     return false;
  std::string rest = urlP.substr(7);
  size_t slash = rest.find('/');
  std::string hostport = rest.substr(0, slash);
  pathP = (slash == std::string::npos) ? "/" : rest.substr(slash);
  portP = 80;
  size_t colon = hostport.rfind(':');
  if (colon != std::string::npos && hostport.find(']') == std::string::npos) {
     portP = atoi(hostport.c_str() + colon + 1);
     hostport = hostport.substr(0, colon);
     }
  hostP = hostport;
  return !hostP.empty() && portP > 0 && portP < 65536;
}

bool cVtDiscover::HttpGet(const std::string &urlP, int timeoutMsP, std::string &headersP, std::string &bodyP)
{
  std::string host, path;
  int port;
  if (!SplitUrl(urlP, host, port, path))
     return false;
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
     return false;
  int fd = socket(res->ai_family, res->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  bool ok = false;
  if (fd >= 0) {
     cTimeMs timer(timeoutMsP);
     bool connected = (connect(fd, res->ai_addr, res->ai_addrlen) == 0);
     if (!connected && errno == EINPROGRESS) {
        struct pollfd p = { fd, POLLOUT, 0 };
        int err = 0;
        socklen_t len = sizeof(err);
        connected = poll(&p, 1, timeoutMsP) > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
        }
     if (connected) {
        std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + ":" + std::to_string(port) +
                          "\r\nUser-Agent: vdr-vtsatip\r\nConnection: close\r\n\r\n";
        if (send(fd, req.data(), req.size(), MSG_NOSIGNAL) == (ssize_t)req.size()) {
           std::string data;
           char buf[4096];
           while (data.size() < 256 * 1024) {
                 int left = timeoutMsP - (int)timer.Elapsed();
                 struct pollfd p = { fd, POLLIN, 0 };
                 if (left <= 0 || poll(&p, 1, left) <= 0)
                    break;
                 ssize_t n = recv(fd, buf, sizeof(buf), 0);
                 if (n == 0) {
                    ok = true; // complete, the server closed the connection
                    break;
                    }
                 if (n < 0) {
                    if (errno == EAGAIN || errno == EINTR)
                       continue;
                    break;
                    }
                 data.append(buf, n);
                 }
           size_t split = data.find("\r\n\r\n");
           if (ok && split != std::string::npos && !strncasecmp(data.c_str(), "HTTP/1.", 7) && data.compare(8, 4, " 200") == 0) {
              headersP = data.substr(0, split);
              bodyP = data.substr(split + 4);
              }
           else
              ok = false;
           }
        }
     close(fd);
     }
  freeaddrinfo(res);
  return ok;
}

// --- cVtDiscover --------------------------------------------------------------

cVtDiscover::cVtDiscover(const cVtDiscoverConfig &cfgP, cVtServers *serversP)
: cThread("vtsatip discovery"),
  cfgM(cfgP),
  serversM(serversP ? serversP : cVtServers::Get()),
  searchFdM(-1),
  listenFdM(-1),
  rescanM(true)
{
}

cVtDiscover::~cVtDiscover()
{
  Stop();
}

void cVtDiscover::Stop(void)
{
  if (Running())
     Cancel(5);
  CloseSockets();
}

void cVtDiscover::CloseSockets(void)
{
  if (searchFdM >= 0)
     close(searchFdM);
  if (listenFdM >= 0)
     close(listenFdM);
  searchFdM = listenFdM = -1;
}

bool cVtDiscover::OpenSockets(void)
{
  searchFdM = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (searchFdM < 0)
     return false;
  unsigned char ttl = 2;
  setsockopt(searchFdM, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(searchFdM, (struct sockaddr *)&a, sizeof(a)) != 0)
     return false;
  if (cfgM.listenPort > 0) {
     listenFdM = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
     if (listenFdM >= 0) {
        int one = 1;
        setsockopt(listenFdM, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
        setsockopt(listenFdM, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
        a.sin_port = htons(cfgM.listenPort);
        if (bind(listenFdM, (struct sockaddr *)&a, sizeof(a)) == 0) {
           struct in_addr group;
           if (inet_pton(AF_INET, cfgM.searchAddr.c_str(), &group) == 1 && IN_MULTICAST(ntohl(group.s_addr))) {
              struct ip_mreq mreq;
              mreq.imr_multiaddr = group;
              mreq.imr_interface.s_addr = htonl(INADDR_ANY);
              if (setsockopt(listenFdM, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0)
                 vtdebug("cannot join the SSDP multicast group: %s (only M-SEARCH answers are used)", strerror(errno));
              }
           }
        else {
           vtdebug("cannot listen for SSDP notifications: %s", strerror(errno));
           close(listenFdM);
           listenFdM = -1;
           }
        }
     }
  return true;
}

void cVtDiscover::SendSearch(void)
{
  if (searchFdM < 0)
     return;
  std::string msg = "M-SEARCH * HTTP/1.1\r\nHOST: " + cfgM.searchAddr + ":" + std::to_string(cfgM.searchPort) +
                    "\r\nMAN: \"ssdp:discover\"\r\nST: " SATIP_ST "\r\nMX: 2\r\n\r\n";
  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port = htons(cfgM.searchPort);
  if (inet_pton(AF_INET, cfgM.searchAddr.c_str(), &to.sin_addr) != 1)
     return;
  if (sendto(searchFdM, msg.data(), msg.size(), 0, (struct sockaddr *)&to, sizeof(to)) < 0)
     vtdebug("M-SEARCH failed: %s", strerror(errno));
}

void cVtDiscover::HandleLocation(const cVtSsdpMessage &msgP)
{
  if (serversM->Known(msgP.uuid)) {
     serversM->Touch(msgP.uuid, msgP.maxAge);
     return;
     }
  uint64_t now = cVtServers::NowMs();
  std::map<std::string, uint64_t>::iterator f = failedM.find(msgP.location);
  if (f != failedM.end() && f->second > now)
     return;
  std::string headers, body, host, path;
  int port;
  if (!SplitUrl(msgP.location, host, port, path))
     return;
  if (!HttpGet(msgP.location, 3000, headers, body)) {
     vtdebug("cannot fetch the description %s", msgP.location.c_str());
     failedM[msgP.location] = now + 30000;
     return;
     }
  failedM.erase(msgP.location);
  cVtDescription desc;
  cVtDiscover::ParseDescription(body, desc);
  size_t h = 0;
  while ((h = headers.find('\n', h)) != std::string::npos) { // X-SATIP-RTSP-Port: 8554
        ++h;
        std::string v;
        std::string line = headers.substr(h, headers.find('\r', h) == std::string::npos ? std::string::npos : headers.find('\r', h) - h);
        if (HeaderIs(line, "X-SATIP-RTSP-Port", v) && atoi(v.c_str()) > 0)
           desc.rtspPort = atoi(v.c_str());
        }
  int caps[VTSATIP_MEDIUM_COUNT];
  if (!cVtServers::ParseCaps(desc.caps.empty() ? "DVBS2-1" : desc.caps.c_str(), caps)) {
     vtdebug("server %s offers no usable delivery system (%s)", host.c_str(), desc.caps.c_str());
     return;
     }
  if (serversM->Update(msgP.uuid, host, desc.rtspPort, desc.name.empty() ? "MyBrokenHardware" : desc.name, desc.model, caps, msgP.maxAge))
     vtinfo("found SAT>IP server '%s' at %s:%d (%s)", desc.name.c_str(), host.c_str(), desc.rtspPort, desc.caps.c_str());
}

void cVtDiscover::HandleDatagram(const char *dataP)
{
  cVtSsdpMessage msg;
  if (!ParseSsdp(dataP, msg))
     return;
  if (msg.kind == cVtSsdpMessage::eByebye) {
     if (serversM->Remove(msg.uuid))
        vtinfo("SAT>IP server %s said goodbye", msg.uuid.c_str());
     return;
     }
  HandleLocation(msg);
}

void cVtDiscover::Action(void)
{
  if (!OpenSockets()) {
     vterror("cannot open the UPnP discovery sockets: %s", strerror(errno));
     CloseSockets();
     return;
     }
  vtdebug("discovery started");
  cTimeMs nextProbe(0);
  int burst = 0;
  while (Running()) {
        if (rescanM.exchange(false)) {
           nextProbe.Set(0);
           burst = 2; // two queries one second apart, like the SAT>IP plugin
           }
        if (nextProbe.TimedOut()) {
           SendSearch();
           if (burst > 1) {
              --burst;
              nextProbe.Set(1000);
              }
           else {
              burst = 0;
              nextProbe.Set(cfgM.probeIntervalS * 1000);
              }
           }
        struct pollfd fds[2];
        int n = 0;
        fds[n].fd = searchFdM; fds[n].events = POLLIN; fds[n].revents = 0; ++n;
        if (listenFdM >= 0) {
           fds[n].fd = listenFdM; fds[n].events = POLLIN; fds[n].revents = 0; ++n;
           }
        if (poll(fds, n, 500) > 0) {
           for (int i = 0; i < n; ++i) {
               if (!(fds[i].revents & POLLIN))
                  continue;
               char buf[2048];
               for (int k = 0; k < 16; ++k) {
                   ssize_t len = recv(fds[i].fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
                   if (len <= 0)
                      break;
                   buf[len] = 0;
                   HandleDatagram(buf);
                   }
               }
           }
        serversM->Expire();
        }
  vtdebug("discovery stopped");
}
