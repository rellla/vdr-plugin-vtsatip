// Compile-only stand-in for the parts of VDR's device/channel API used by vtsatip
#pragma once
#include "tools.h"
#include "thread.h"
#include "ringbuffer.h"
#define MAXDEVICES 16
#define IDLEPRIORITY -100
#define CA_ENCRYPTED_MIN 16
#define APIVERSNUM 20608
#define DTV_STAT_VALID_NONE 0
#define DTV_STAT_VALID_STRENGTH 1
#define DTV_STAT_VALID_STATUS 8
#define DTV_STAT_HAS_NONE 0
#define DTV_STAT_HAS_SIGNAL 1
#define DTV_STAT_HAS_CARRIER 2
#define DTV_STAT_HAS_VITERBI 4
#define DTV_STAT_HAS_SYNC 8
#define DTV_STAT_HAS_LOCK 16
#define ISTRANSPONDER(a, b) (abs((a) - (b)) < 4)
struct tChannelID { static const tChannelID InvalidID; bool operator==(const tChannelID &) const { return true; } };
class cChannel {
public:
  int Source() const { return 0; } int Transponder() const { return 0; } const char *Parameters() const { return ""; }
  int Vpid() const { return 0; } int Apid(int) const { return 0; } int Dpid(int) const { return 0; }
  int Ca() const { return 0; } int Sid() const { return 0; } const int *Caids() const { static int z[1] = {0}; return z; } int Rid() const { return 0; } int Frequency() const { return 0; } int Srate() const { return 0; }
  const char *Name() const { return ""; } tChannelID GetChannelID() const { return tChannelID(); }
};
class cSource { public: static char ToChar(int) { return 'S'; } static cString ToString(int) { return cString("S19.2E"); } const char *Description() const { return ""; } };
class cSources { public: cSource *Get(int) { return NULL; } };
extern cSources Sources;
struct cPidHandle { int pid; int streams; int handle; int used; };
class cCamSlot { public: bool CanDecrypt(const cChannel *) { return true; } bool WantsTsData() const { return false; } uchar *Decrypt(uchar *D, int &) { return D; } };
class cDevice {
public:
  cDevice() {}
  virtual ~cDevice() {}
  int CardIndex() const { return 0; } int DeviceNumber() const { return 0; }
  int Priority() const { return 0; } bool Receiving() const { return false; } int Occupied(int = 0) const { return 0; } bool HasPid(int) const { return false; }
  cCamSlot *CamSlot() const { return NULL; }
  void DetachAllReceivers() {} void Cancel(int = 0) {}
  virtual bool Ready() { return true; }
  virtual bool HasInternalCam() { return false; }
  virtual cString DeviceType() const { return ""; }
  virtual cString DeviceName() const { return ""; }
  virtual bool SignalStats(int &, double * = NULL, double * = NULL, double * = NULL, double * = NULL, double * = NULL, int * = NULL) const { return false; }
  virtual int SignalStrength() const { return -1; }
  virtual int SignalQuality() const { return -1; }
  virtual bool ProvidesSource(int) const { return false; }
  virtual bool ProvidesTransponder(const cChannel *) const { return false; }
  virtual bool ProvidesChannel(const cChannel *, int = IDLEPRIORITY, bool * = NULL) const { return false; }
  virtual bool ProvidesEIT() const { return false; }
  virtual int NumProvidedSystems() const { return 0; }
  virtual const cChannel *GetCurrentlyTunedTransponder() const { return NULL; }
  virtual bool IsTunedToTransponder(const cChannel *) const { return false; }
  virtual void SetPowerSaveMode(bool) {}
  virtual bool HasLock(int = 0) const { return false; }
  virtual int OpenFilter(u_short, u_char, u_char) { return -1; }
  virtual void CloseFilter(int) {}
protected:
  void StartSectionHandler() {} void StopSectionHandler() {}
  bool DeviceHooksProvidesTransponder(const cChannel *) const { return true; }
  bool DeviceHooksProvidesEIT() const { return true; }
  virtual bool SetChannelDevice(const cChannel *, bool) { return false; }
  virtual bool SetPid(cPidHandle *, int, bool) { return true; }
  virtual bool OpenDvr() { return false; }
  virtual void CloseDvr() {}
  virtual bool GetTSPacket(uchar *&D) { D = NULL; return true; }
};
