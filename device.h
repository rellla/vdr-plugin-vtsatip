/*
 * device.h: vtsatip - VDR device backed by a SAT>IP stream
 */

#ifndef __VTSATIP_DEVICE_H
#define __VTSATIP_DEVICE_H

#include <atomic>
#include <vdr/device.h>
#include "common.h"
#include "tuner.h"
#include "sectionfilter.h"
#include "pat.h"

class cVtSatipDevice : public cDevice, public cVtTunerSink {
public:
  static bool Initialize(unsigned int deviceCountP);
  static void Shutdown(void);
  static cString GetStatus(void);
  static cVtSatipDevice *GetDevice(unsigned int indexP);

private:
  unsigned int indexM;
  static cMutex mutexDevicesS;
  cString deviceNameM;
  cChannel channelM;
  bool isOpenDvrM;
  int bytesDeliveredM;
  bool checkTsBufferM;
  cRingBufferLinear *tsBufferM;
  cVtSecHandler *secHandlerM;
  cVtTuner *tunerM;
  cTimeMs createdM;
  unsigned long overflowsM;
  // CI extension
  cVtPatParser patM;
  std::atomic<bool> patWantedM;
  std::atomic<int> tunedSourceM;
  std::atomic<int> tunedTransponderM;
  std::atomic<int> tunedSidM;
  // what the tuner was told last, to be able to move to another server
  cMutex lastMutexM;
  cString lastParamsM;
  int lastCiM;
  int lastPmtM;
  int lastMediumM;

  explicit cVtSatipDevice(unsigned int indexP);
  cVtSatipDevice(const cVtSatipDevice &);
  cVtSatipDevice &operator=(const cVtSatipDevice &);

  int CiSlot(void) const;
  bool ProvidesCa(const int *caidsP) const;
  void FeedPat(const uchar *dataP, int lengthP);
  uchar *GetData(int *availableP = NULL, bool checkTsBufferP = false);
  void SkipData(int countP);

public:
  virtual ~cVtSatipDevice();
  cString GetInformation(void);

  // cVtTunerSink
  virtual int SinkIndex(void) { return (int)indexM; }
  virtual void OnTsData(const uchar *dataP, int lengthP);
  virtual void OnRetune(void);
  virtual void OnServerBusy(const std::string &hostP, int portP);

  // cDevice
  virtual bool Ready(void);
  virtual cString DeviceType(void) const;
  virtual cString DeviceName(void) const;
  virtual bool SignalStats(int &Valid, double *Strength = NULL, double *Cnr = NULL, double *BerPre = NULL, double *BerPost = NULL, double *Per = NULL, int *Status = NULL) const;
  virtual int SignalStrength(void) const;
  virtual int SignalQuality(void) const;
  virtual bool ProvidesSource(int sourceP) const;
  virtual bool ProvidesTransponder(const cChannel *channelP) const;
  virtual bool ProvidesChannel(const cChannel *channelP, int priorityP = IDLEPRIORITY, bool *needsDetachReceiversP = NULL) const;
  virtual bool ProvidesEIT(void) const;
  virtual int NumProvidedSystems(void) const;
  virtual const cChannel *GetCurrentlyTunedTransponder(void) const;
  virtual bool IsTunedToTransponder(const cChannel *channelP) const;
  virtual void SetPowerSaveMode(bool On);
  virtual bool HasLock(int timeoutMsP = 0) const;

protected:
  virtual bool SetChannelDevice(const cChannel *channelP, bool liveViewP);
  virtual bool SetPid(cPidHandle *handleP, int typeP, bool onP);
  virtual bool OpenDvr(void);
  virtual void CloseDvr(void);
  virtual bool GetTSPacket(uchar *&dataP);

public:
  virtual int OpenFilter(u_short pidP, u_char tidP, u_char maskP);
  virtual void CloseFilter(int handleP);
  virtual bool HasInternalCam(void);
};

#endif // __VTSATIP_DEVICE_H
