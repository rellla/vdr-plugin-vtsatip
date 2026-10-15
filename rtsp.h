/*
 * rtsp.h: vtsatip - minimal blocking RTSP client for SAT>IP
 *
 * Based on the RTSP handling of the vtuner-ng satip daemon.
 */

#ifndef __VTSATIP_RTSP_H
#define __VTSATIP_RTSP_H

#include <string>
#include "common.h"

// what the server answered to SETUP
struct cVtTransport {
  bool multicast;
  std::string destination;   // multicast group
  std::string source;        // optional multicast source (SSM)
  int rtpPort;
  int rtcpPort;
  cVtTransport(void) : multicast(false), rtpPort(0), rtcpPort(0) {}
};

class cVtRtsp {
private:
  enum { eMaxResponse = 4096 };
  int fdM;
  cString hostM;
  int portM;
  int cseqM;
  char sessionM[96];
  int streamIdM;
  int timeoutS;
  int lastStatusM;
  cVtTransport transportM;
  char rxM[eMaxResponse + 1];

  bool Send(const char *requestP);
  bool Transact(const char *requestP, int timeoutMsP, int *statusP = NULL);
  bool WaitFd(short eventsP, int timeoutMsP);
  const char *Header(const char *nameP) const;

public:
  cVtRtsp(void);
  ~cVtRtsp();
  bool Connect(const char *hostP, int portP, int timeoutMsP = 3000);
  void Close(void);
  bool IsOpen(void) const { return fdM >= 0; }
  bool HasSession(void) const { return streamIdM >= 0; }
  int StreamId(void) const { return streamIdM; }
  int SessionTimeout(void) const { return timeoutS; }
  // RTSP status of the last request: 0 = no answer (transport problem), otherwise the status code
  int LastStatus(void) const { return lastStatusM; }
  bool Options(void);
  // query: the part after '?' (without '?')
  // modeP: VTSATIP_TRANSPORT_UNICAST or _MULTICAST; what the server really chose is in Transport()
  bool Setup(const char *queryP, int rtpPortP, int rtcpPortP, int modeP = VTSATIP_TRANSPORT_UNICAST);
  const cVtTransport &Transport(void) const { return transportM; }
  bool PlayOnce(const char *queryP);
  bool Play(const char *queryP);
  bool Teardown(void);
};

#endif // __VTSATIP_RTSP_H
