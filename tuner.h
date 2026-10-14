/*
 * tuner.h: vtsatip - SAT>IP tuner (RTSP control + RTP/RTCP receive)
 *
 * The control flow follows the vtuner-ng satip daemon:
 *  - one persistent RTSP session per device
 *  - SETUP (pids=none) only for the first tuning, every retune is a single PLAY
 *    carrying tuning parameters *and* the full PID list
 *  - PID changes are sent immediately as addpids/delpids (no timer)
 *  - PAT/NIT/SDT/EIT/... are always requested
 *  - "lock" is declared as soon as the first real TS packet arrives
 *  - packets which arrive while a retune is in flight are discarded
 */

#ifndef __VTSATIP_TUNER_H
#define __VTSATIP_TUNER_H

#include <atomic>
#include <set>
#include <string>
#include <vdr/thread.h>
#include <vdr/tools.h>
#include "common.h"
#include "rtsp.h"

class cVtTunerSink {
public:
  virtual ~cVtTunerSink() {}
  virtual int  SinkIndex(void) = 0;
  virtual void OnTsData(const uchar *dataP, int lengthP) = 0; // multiple of TS_SIZE
  virtual void OnRetune(void) = 0;                            // flush all buffered data
};

class cVtTuner;

class cVtReceiver : public cThread {
private:
  cVtTuner &tunerM;
  int rtpFdM;
  int rtcpFdM;
  int rtpPortM;
  void ProcessRtp(const uchar *bufP, int lenP);
  void ProcessRtcp(const uchar *bufP, int lenP);
protected:
  virtual void Action(void);
public:
  explicit cVtReceiver(cVtTuner &tunerP);
  virtual ~cVtReceiver();
  bool Open(void);
  void Stop(void);
  int RtpPort(void) const { return rtpPortM; }
  int RtcpPort(void) const { return rtpPortM + 1; }
};

class cVtTuner : public cThread {
  friend class cVtReceiver;
private:
  enum {
    eRetryDelayMs     = 5000,
    eIdlePollMs       = 1000,
    eMinKeepAliveSec  = 5
  };
  cVtTunerSink &sinkM;
  int indexM;
  cMutex mutexM;
  cCondVar condM;
  // requests from VDR (protected by mutexM)
  std::string wantParamsM;
  bool tunePendingM;
  unsigned wantSeqM;
  bool wantReleaseM;
  std::set<int> wantPidsM;
  std::set<int> wantPmtM;      // PMT PIDs of the services the CI should decrypt
  int wantCiSlotM;
  std::string wantHostM;       // empty = server from the configuration
  int wantPortM;
  // session state (control thread only)
  cVtRtsp rtspM;
  std::set<int> activePidsM;
  std::string activeParamsM;
  std::string activeHostM;
  int activePortM;
  std::set<int> activePmtM;
  bool sessionHadPmtM;
  cTimeMs keepAliveM;
  cVtReceiver receiverM;
  // status (read by other threads)
  std::atomic<bool> acceptM;
  std::atomic<bool> lockM;
  std::atomic<int> strengthM;   // 0..100, -1 unknown
  std::atomic<int> qualityM;    // 0..100, -1 unknown
  std::atomic<int> levelM;      // raw 0..255
  std::atomic<int> frontendM;
  std::atomic<bool> sessionUpM;
  std::atomic<unsigned long> bytesM;

  std::set<int> WantedPids(void);     // with standard PIDs, mutexM must be held
  static std::string JoinPids(const std::set<int> &pidsP);
  static std::string CiParams(const std::set<int> &pmtP, int ciSlotP);
  bool DoTune(const std::string &paramsP, const std::set<int> &pidsP, const std::set<int> &pmtP, int ciSlotP,
              const std::string &hostP, int portP);
  bool DoPids(const std::set<int> &wantP, const std::set<int> &pmtP, int ciSlotP);
  void DoRelease(void);
  void DropSession(void);
  void OnTsPayload(const uchar *dataP, int lenP);
  void OnRtcpInfo(const char *infoP);
protected:
  virtual void Action(void);
public:
  explicit cVtTuner(cVtTunerSink &sinkP);
  virtual ~cVtTuner();
  // params: SAT>IP query string without leading '?', e.g. "src=1&freq=11493&pol=h&msys=dvbs2..."
  // ciSlotP > 0: decrypt with this CI slot of the server (x_ci), pmtPidP: PMT PID of the service (0 = not known yet)
  // hostP/portP: SAT>IP server to use (NULL = the one from the configuration)
  void SetChannel(const char *paramsP, int ciSlotP = 0, int pmtPidP = 0, const char *hostP = NULL, int portP = 0);
  void SetPmt(int pmtPidP);
  void SetPid(int pidP, bool onP);
  void Release(void);
  bool HasLock(void) const { return acceptM.load() && lockM.load(); }
  bool IsTuned(void) const { return sessionUpM.load() && acceptM.load(); }
  int SignalStrength(void) const { return strengthM.load(); }
  double SignalStrengthDBm(void) const;
  int SignalQuality(void) const { return qualityM.load(); }
  int FrontendId(void) const { return frontendM.load(); }
  cString GetInformation(void);
};

#endif // __VTSATIP_TUNER_H
