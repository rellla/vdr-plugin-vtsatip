/*
 * rtsp.h: vtsatip - minimal blocking RTSP client for SAT>IP
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

#ifndef __VTSATIP_RTSP_H
#define __VTSATIP_RTSP_H

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "common.h"

// what the server answered to SETUP
struct cVtTransport {
  bool multicast;
  std::string destination;   // multicast group
  std::string source;        // optional multicast source (SSM)
  int rtpPort;
  int rtcpPort;
  bool tcp;                  // RTP/AVP/TCP: the stream is interleaved into the RTSP connection
  int rtpChannel;            // interleaved channel numbers
  int rtcpChannel;
  cVtTransport(void) : multicast(false), rtpPort(0), rtcpPort(0), tcp(false), rtpChannel(0), rtcpChannel(1) {}
};

// receives the payload of '$'-framed data (RTP over TCP)
typedef void (*cVtInterleavedFn)(void *ctxP, int channelP, const uchar *dataP, int lengthP);

class cVtRtsp {
private:
  enum { eMaxResponse = 4096 };
  int fdM;
  std::atomic<bool> abortM;
  cString hostM;
  int portM;
  int cseqM;
  char sessionM[96];
  int streamIdM;
  int timeoutS;
  int lastStatusM;
  cVtTransport transportM;
  char rxM[eMaxResponse + 1];
  // RTP over TCP: the RTSP socket carries responses and '$'-framed data. Whoever reads (the tuner thread while it
  // waits for a response, the receiver thread otherwise) holds ioM.
  std::mutex ioM;
  std::atomic<int> wantersM;
  bool tcpModeM;
  bool brokenM;
  std::vector<uchar> ibufM;
  size_t ilenM;
  cVtInterleavedFn handlerM;
  void *handlerCtxM;
  void LockIo(void);
  int ParseInput(bool wantResponseP);   // 1 = response in rxM, 0 = need more data
  bool TransactTcp(int timeoutMsP);

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
  // Makes every wait for the server give up within ~100 ms (shutdown); SetAbort(false) allows
  // waiting again, e.g. for the final TEARDOWN.
  void SetAbort(bool abortP) { abortM = abortP; }
  bool HasSession(void) const { return streamIdM >= 0; }
  int StreamId(void) const { return streamIdM; }
  int SessionTimeout(void) const { return timeoutS; }
  // RTSP status of the last request: 0 = no answer (transport problem), otherwise the status code
  int LastStatus(void) const { return lastStatusM; }
  bool Options(void);
  // query: the part after '?' (without '?')
  // modeP: VTSATIP_TRANSPORT_UNICAST, _MULTICAST or _TCP; what the server really chose is in Transport()
  bool Setup(const char *queryP, int rtpPortP, int rtcpPortP, int modeP = VTSATIP_TRANSPORT_UNICAST);
  // RTP over TCP: where '$'-framed data goes, and the reader loop of the receiver thread. Pump() waits up to
  // timeoutMsP for data and dispatches it; false = nothing to read (not connected, not TCP, or connection lost).
  void SetInterleavedHandler(cVtInterleavedFn fnP, void *ctxP) { handlerM = fnP; handlerCtxM = ctxP; }
  bool Pump(int timeoutMsP);
  int WaitingThreads(void) const { return wantersM.load(); }
  const cVtTransport &Transport(void) const { return transportM; }
  bool PlayOnce(const char *queryP);
  bool Play(const char *queryP);
  bool Teardown(void);
};

#endif // __VTSATIP_RTSP_H
