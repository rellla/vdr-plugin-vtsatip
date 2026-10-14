/*
 * discover.h: vtsatip - SAT>IP server discovery (UPnP/SSDP)
 *
 * Discovery runs in one thread: periodic M-SEARCH, listening for NOTIFY
 * (alive/byebye), fetching and evaluating the device description. No XML or
 * HTTP library is needed.
 */

#ifndef __VTSATIP_DISCOVER_H
#define __VTSATIP_DISCOVER_H

#include <atomic>
#include <map>
#include <string>
#include <vdr/thread.h>
#include "common.h"
#include "servers.h"

struct cVtSsdpMessage {
  enum eKind { eOther, eResponse, eAlive, eByebye };
  eKind kind;
  std::string location;
  std::string usn;
  std::string uuid;
  int maxAge;
  cVtSsdpMessage(void) : kind(eOther), maxAge(1800) {}
};

struct cVtDescription {
  std::string name;
  std::string model;
  std::string caps;      // X_SATIPCAP
  int rtspPort;
  cVtDescription(void) : rtspPort(VTSATIP_DEFAULT_RTSP_PORT) {}
};

struct cVtDiscoverConfig {
  std::string searchAddr;   // where M-SEARCH is sent to
  int searchPort;
  int listenPort;           // NOTIFY listener, 0 = none
  int probeIntervalS;
  cVtDiscoverConfig(void) : searchAddr("239.255.255.250"), searchPort(1900), listenPort(1900), probeIntervalS(60) {}
};

class cVtDiscover : public cThread {
private:
  cVtDiscoverConfig cfgM;
  cVtServers *serversM;
  int searchFdM;
  int listenFdM;
  std::atomic<bool> rescanM;
  std::map<std::string, uint64_t> failedM;   // location -> retry time

  bool OpenSockets(void);
  void CloseSockets(void);
  void SendSearch(void);
  void HandleDatagram(const char *dataP);
  void HandleLocation(const cVtSsdpMessage &msgP);
protected:
  virtual void Action(void);
public:
  explicit cVtDiscover(const cVtDiscoverConfig &cfgP = cVtDiscoverConfig(), cVtServers *serversP = NULL);
  virtual ~cVtDiscover();
  void Rescan(void) { rescanM = true; }
  void Stop(void);

  // building blocks, public for the tests
  static bool ParseSsdp(const char *dataP, cVtSsdpMessage &msgP);
  static bool ParseDescription(const std::string &xmlP, cVtDescription &descP);
  static std::string XmlText(const std::string &xmlP, const char *nameP);
  static bool SplitUrl(const std::string &urlP, std::string &hostP, int &portP, std::string &pathP);
  static bool HttpGet(const std::string &urlP, int timeoutMsP, std::string &headersP, std::string &bodyP);
};

#endif // __VTSATIP_DISCOVER_H
