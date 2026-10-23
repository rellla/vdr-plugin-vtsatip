/*
 * device.h: vtsatip - VDR device backed by a SAT>IP stream
 *
 * Copyright (C) 2026 Andreas Baierl <ichgeh@imkreisrum.de>
 *
 * The structure follows the SAT>IP plugin for VDR (https://github.com/FireFlyVDR/vdr-plugin-satip),
 * Copyright (C) Rolf Ahrenberg and contributors.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation. See the file COPYING.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __VTSATIP_DEVICE_H
#define __VTSATIP_DEVICE_H

#include <atomic>
#include <vdr/device.h>
#include "common.h"
#include "tuner.h"
#include "sectionfilter.h"
#include "pat.h"

// a snapshot of what a device is doing, for the statistics page
struct cVtDeviceStats {
  unsigned index;
  bool detached;
  bool session;          // RTSP session is up
  bool lock;
  int frontend;          // server frontend, -1 unknown
  int strength;          // 0..100, -1 unknown
  int quality;
  int transport;         // VTSATIP_TRANSPORT_*
  unsigned long bytes;
  unsigned long packets;
  unsigned long lost;
  unsigned long reordered;
  unsigned long gapMs;     // longest pause between two packets, last ~2 s
  unsigned long gapMaxMs;  // since the tuning
  int rcvBufKiB;           // receive buffer the RTP socket really got
  unsigned long pauses;    // pauses of at least 200 ms since the tuning
  unsigned long playPauses; // ... of them related to a PLAY request
  std::string server;
  std::string channel;
  cVtDeviceStats(void) : index(0), detached(false), session(false), lock(false), frontend(-1), strength(-1), quality(-1),
                         transport(0), bytes(0), packets(0), lost(0), reordered(0), gapMs(0), gapMaxMs(0), rcvBufKiB(0), pauses(0), playPauses(0) {}
};

class cVtSatipDevice : public cDevice, public cVtTunerSink {
public:
  static bool GetStats(unsigned indexP, cVtDeviceStats &statsP); // false: no such device
  static bool Initialize(unsigned int deviceCountP);
  static void Shutdown(void);
  static cString GetStatus(void);
  static void IdleCheckAll(void);   // main thread, about once a second: release unused streams
  static cVtSatipDevice *GetDevice(unsigned int indexP);
  // SVDRP ATTA/DETA: device -1 = all; returns a message
  static cString Attach(int indexP, bool attachP, int &replyCodeP);
  bool Detached(void) const { return detachedM; }

private:
  unsigned int indexM;
  static cMutex mutexDevicesS;
  cString deviceNameM;
  mutable cMutex statMutexM;
  std::string serverNameM;      // for the statistics
  std::atomic<bool> detachedM;
  bool Detach(void);
  void SetDeviceName(const cString &nameP) { cMutexLock lock(&statMutexM); deviceNameM = nameP; }
  void SetServerName(const std::string &nameP) { cMutexLock lock(&statMutexM); serverNameM = nameP; }
  cChannel channelM;            // only touched by VDR's threads that call the cDevice interface
  std::string channelNameM;     // copy for the statistics and SVDRP, protected by statMutexM
  bool channelValidM;
  std::atomic<bool> isOpenDvrM;
  std::atomic<bool> clearTsM;   // OnRetune() asks the reading thread (GetData) to drop the buffered data
  int bytesDeliveredM;
  void SetChannelName(const cChannel *channelP);
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
  cString lastSourceM;
  int lastCiM;
  int lastPmtM;
  int lastMediumM;
  std::atomic<uint64_t> lastActiveMsM; // last time the device was tuned, receiving or occupied
  void IdleCheck(void);
  std::atomic<const char *> typeM; // delivery system of the last tuning, e.g. "DVB-S2"

  explicit cVtSatipDevice(unsigned int indexP);
  cVtSatipDevice(const cVtSatipDevice &);
  cVtSatipDevice &operator=(const cVtSatipDevice &);

  int CiSlot(void) const;
  bool ProvidesCa(const int *caidsP, int sidP = 0) const;
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
