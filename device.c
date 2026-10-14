/*
 * device.c: vtsatip - VDR device backed by a SAT>IP stream
 */

#include <vdr/dvbdevice.h>
#include "param.h"
#include "device.h"

static cVtSatipDevice *VtDevicesS[VTSATIP_MAX_DEVICES] = { NULL };

cMutex cVtSatipDevice::mutexDevicesS;

cVtSatipDevice::cVtSatipDevice(unsigned int indexP)
: indexM(indexP),
  deviceNameM(*cString::sprintf("vtSAT>IP %u", indexP)),
  channelM(),
  isOpenDvrM(false),
  bytesDeliveredM(0),
  checkTsBufferM(false),
  tsBufferM(NULL),
  secHandlerM(NULL),
  tunerM(NULL),
  createdM(0),
  overflowsM(0)
{
  unsigned int bufsize = (unsigned int)VTSATIP_TSBUFFER_SIZE;
  bufsize -= (bufsize % TS_SIZE);
  vtinfo("creating device CardIndex=%d DeviceNumber=%d [vtsatip device %u]", CardIndex(), DeviceNumber(), indexM);
  tsBufferM = new cRingBufferLinear(bufsize + 1, TS_SIZE, false, *cString::sprintf("vtsatip#%u TS", indexM));
  tsBufferM->SetTimeouts(10, 10);
  tsBufferM->SetIoThrottle();
  secHandlerM = new cVtSecHandler((int)indexM, (int)bufsize + 1);
  tunerM = new cVtTuner(*this);
  StartSectionHandler();
}

cVtSatipDevice::~cVtSatipDevice()
{
  StopSectionHandler();
  delete tunerM;
  tunerM = NULL;
  delete secHandlerM;
  secHandlerM = NULL;
  delete tsBufferM;
  tsBufferM = NULL;
}

bool cVtSatipDevice::Initialize(unsigned int deviceCountP)
{
  if (deviceCountP > VTSATIP_MAX_DEVICES)
     deviceCountP = VTSATIP_MAX_DEVICES;
  for (unsigned int i = 0; i < deviceCountP; ++i)
      VtDevicesS[i] = new cVtSatipDevice(i);
  return true;
}

void cVtSatipDevice::Shutdown(void)
{
  for (int i = 0; i < VTSATIP_MAX_DEVICES; ++i) {
      if (VtDevicesS[i])
         VtDevicesS[i]->CloseDvr();
      }
}

cVtSatipDevice *cVtSatipDevice::GetDevice(unsigned int indexP)
{
  return (indexP < VTSATIP_MAX_DEVICES) ? VtDevicesS[indexP] : NULL;
}

cString cVtSatipDevice::GetInformation(void)
{
  return cString::sprintf("vtSAT>IP device %u (CardIndex %d)\n  tuner: %s\n  overflows: %lu\n  section filters: %d\n  last channel: %s\n",
                          indexM, CardIndex(), tunerM ? *tunerM->GetInformation() : "-",
                          overflowsM, secHandlerM ? secHandlerM->Count() : 0,
                          channelM.GetChannelID() == tChannelID::InvalidID ? "none" : channelM.Name());
}

cString cVtSatipDevice::GetStatus(void)
{
  cString s = "";
  for (int i = 0; i < VTSATIP_MAX_DEVICES; ++i)
      if (VtDevicesS[i])
         s = cString::sprintf("%s%s", *s, *VtDevicesS[i]->GetInformation());
  return isempty(*s) ? cString("no vtsatip devices") : s;
}

// --- cVtTunerSink -------------------------------------------------------------

void cVtSatipDevice::OnTsData(const uchar *dataP, int lengthP)
{
  if (isOpenDvrM && tsBufferM) {
     int n = tsBufferM->Put(dataP, lengthP);
     if (n != lengthP) {
        ++overflowsM;
        tsBufferM->ReportOverflow(lengthP - n);
        }
     }
  if (secHandlerM && secHandlerM->HasFilters())
     secHandlerM->Write(dataP, lengthP);
}

void cVtSatipDevice::OnRetune(void)
{
  if (tsBufferM)
     tsBufferM->Clear();
  if (secHandlerM)
     secHandlerM->Clear();
}

// --- cDevice ------------------------------------------------------------------

bool cVtSatipDevice::Ready(void)
{
  return true;
}

cString cVtSatipDevice::DeviceType(void) const
{
  return "vtSAT>IP";
}

cString cVtSatipDevice::DeviceName(void) const
{
  return deviceNameM;
}

bool cVtSatipDevice::SignalStats(int &Valid, double *Strength, double *Cnr, double *BerPre, double *BerPost, double *Per, int *Status) const
{
  Valid = DTV_STAT_VALID_NONE;
  if (Strength && tunerM) {
     *Strength = tunerM->SignalStrengthDBm();
     if (*Strength < -18.0) // valid: -71.458 .. -18.541, invalid: 0.0
        Valid |= DTV_STAT_VALID_STRENGTH;
     }
  if (Status) {
     *Status = HasLock() ? (DTV_STAT_HAS_SIGNAL | DTV_STAT_HAS_CARRIER | DTV_STAT_HAS_VITERBI | DTV_STAT_HAS_SYNC | DTV_STAT_HAS_LOCK) : DTV_STAT_HAS_NONE;
     Valid |= DTV_STAT_VALID_STATUS;
     }
  return Valid != DTV_STAT_VALID_NONE;
}

int cVtSatipDevice::SignalStrength(void) const
{
  return tunerM ? tunerM->SignalStrength() : -1;
}

int cVtSatipDevice::SignalQuality(void) const
{
  return tunerM ? tunerM->SignalQuality() : -1;
}

bool cVtSatipDevice::ProvidesSource(int sourceP) const
{
  cSource *s = Sources.Get(sourceP);
  // source descriptions starting with '0' disable the source for SAT>IP
  if (s && s->Description() && *(s->Description()) == '0')
     return false;
  switch (cSource::ToChar(sourceP)) {
    case 'S': return (VtSatipConfig.systems & VTSATIP_SYS_SAT) != 0;
    case 'T': return (VtSatipConfig.systems & VTSATIP_SYS_TERR) != 0;
    case 'C': return (VtSatipConfig.systems & VTSATIP_SYS_CABLE) != 0;
    default:  return false;
    }
}

bool cVtSatipDevice::ProvidesTransponder(const cChannel *channelP) const
{
  if (!channelP || !ProvidesSource(channelP->Source()))
     return false;
  return DeviceHooksProvidesTransponder(channelP);
}

bool cVtSatipDevice::ProvidesChannel(const cChannel *channelP, int priorityP, bool *needsDetachReceiversP) const
{
  bool result = false;
  bool hasPriority = (priorityP == IDLEPRIORITY) || (priorityP > this->Priority());
  bool needsDetachReceivers = false;

  if (channelP && ProvidesTransponder(channelP)) {
     result = hasPriority;
     if (priorityP > IDLEPRIORITY && Receiving()) {
        if (IsTunedToTransponder(channelP)) {
           if ((channelP->Vpid() && !HasPid(channelP->Vpid())) ||
               (channelP->Apid(0) && !HasPid(channelP->Apid(0))) ||
               (channelP->Dpid(0) && !HasPid(channelP->Dpid(0)))) {
              if (channelP->Ca() >= CA_ENCRYPTED_MIN && CamSlot()) {
                 if (CamSlot()->CanDecrypt(channelP))
                    result = true;
                 else
                    needsDetachReceivers = true;
                 }
              else
                 result = true; // another service of the transponder we are already tuned to
              }
           }
        else
           needsDetachReceivers = true;
        }
     }
  if (needsDetachReceiversP)
     *needsDetachReceiversP = needsDetachReceivers;
  return result;
}

bool cVtSatipDevice::ProvidesEIT(void) const
{
#if defined(APIVERSNUM) && APIVERSNUM < 20403
  return true;
#else
  return DeviceHooksProvidesEIT();
#endif
}

int cVtSatipDevice::NumProvidedSystems(void) const
{
  int n = 0;
  if (VtSatipConfig.systems & VTSATIP_SYS_SAT)
     n += 2; // DVB-S, DVB-S2
  if (VtSatipConfig.systems & VTSATIP_SYS_TERR)
     n += 2; // DVB-T, DVB-T2
  if (VtSatipConfig.systems & VTSATIP_SYS_CABLE)
     n += 1; // DVB-C
  return n < 1 ? 1 : (n > 15 ? 15 : n);
}

const cChannel *cVtSatipDevice::GetCurrentlyTunedTransponder(void) const
{
  return &channelM;
}

bool cVtSatipDevice::IsTunedToTransponder(const cChannel *channelP) const
{
  if (tunerM && !tunerM->IsTuned())
     return false;
  if (channelM.Source() != channelP->Source() || !ISTRANSPONDER(channelM.Transponder(), channelP->Transponder()))
     return false;
  return strcmp(channelM.Parameters(), channelP->Parameters()) == 0;
}

void cVtSatipDevice::SetPowerSaveMode(bool On)
{
  if (On && VtSatipConfig.disconnectIdle && tunerM) {
     cMutexLock lock(&mutexDevicesS);
     tunerM->Release();
     channelM = cChannel();
     }
}

bool cVtSatipDevice::HasLock(int timeoutMsP) const
{
  if (timeoutMsP > 0) {
     cTimeMs timer(timeoutMsP);
     while (!timer.TimedOut()) {
           if (tunerM && tunerM->HasLock())
              return true;
           cCondWait::SleepMs(10);
           }
     }
  return tunerM && tunerM->HasLock();
}

bool cVtSatipDevice::SetChannelDevice(const cChannel *channelP, bool liveViewP)
{
  (void)liveViewP;
  if (!tunerM)
     return false;
  if (!channelP) {
     tunerM->SetChannel(NULL);
     return true;
     }
  cString params = GetTransponderUrlParameters(channelP);
  if (isempty(*params)) {
     vterror("unrecognized channel parameters: %s [device %u]", channelP->Parameters(), indexM);
     return false;
     }
  // No waiting for the tuner here: the tuning request is handed to the tuner
  // thread and the lock is detected from the first TS packet. This keeps
  // channel switching as fast as with vtuner-ng.
  channelM = *channelP;
  tunerM->SetChannel(*params);
  return true;
}

bool cVtSatipDevice::SetPid(cPidHandle *handleP, int typeP, bool onP)
{
  (void)typeP;
  if (tunerM && handleP && handleP->pid >= 0 && handleP->pid <= 8191) {
     if (onP)
        tunerM->SetPid(handleP->pid, true);
     else if (!handleP->used && !(secHandlerM && secHandlerM->Exists(handleP->pid)))
        tunerM->SetPid(handleP->pid, false);
     }
  return true;
}

int cVtSatipDevice::OpenFilter(u_short pidP, u_char tidP, u_char maskP)
{
  if (secHandlerM) {
     int handle = secHandlerM->Open(pidP, tidP, maskP);
     if (tunerM && handle >= 0)
        tunerM->SetPid(pidP, true);
     return handle;
     }
  return -1;
}

void cVtSatipDevice::CloseFilter(int handleP)
{
  if (secHandlerM) {
     int pid = secHandlerM->GetPid(handleP);
     secHandlerM->Close(handleP);
     // keep the PID if a receiver still uses it
     if (tunerM && pid >= 0 && !HasPid(pid))
        tunerM->SetPid(pid, false);
     }
}

bool cVtSatipDevice::OpenDvr(void)
{
  bytesDeliveredM = 0;
  tsBufferM->Clear();
  isOpenDvrM = true;
  return true;
}

void cVtSatipDevice::CloseDvr(void)
{
  isOpenDvrM = false;
}

uchar *cVtSatipDevice::GetData(int *availableP, bool checkTsBufferP)
{
  if (isOpenDvrM && tsBufferM) {
     int count = 0;
     if (bytesDeliveredM) {
        tsBufferM->Del(bytesDeliveredM);
        bytesDeliveredM = 0;
        }
     if (checkTsBufferP && tsBufferM->Available() < TS_SIZE)
        return NULL;
     uchar *p = tsBufferM->Get(count);
     if (p && count >= TS_SIZE) {
        if (*p != TS_SYNC_BYTE) {
           for (int i = 1; i < count; ++i) {
               if (p[i] == TS_SYNC_BYTE) {
                  count = i;
                  break;
                  }
               }
           tsBufferM->Del(count);
           vtinfo("skipped %d bytes to sync on TS packet [device %u]", count, indexM);
           return NULL;
           }
        bytesDeliveredM = TS_SIZE;
        if (availableP)
           *availableP = count;
        return p;
        }
     }
  return NULL;
}

void cVtSatipDevice::SkipData(int countP)
{
  bytesDeliveredM = countP;
}

bool cVtSatipDevice::GetTSPacket(uchar *&dataP)
{
  if (tsBufferM) {
     if (cCamSlot *cs = CamSlot()) {
        if (cs->WantsTsData()) {
           int available = 0;
           dataP = GetData(&available, checkTsBufferM);
           if (!dataP)
              available = 0;
           dataP = cs->Decrypt(dataP, available);
           SkipData(available);
           checkTsBufferM = dataP != NULL;
           return true;
           }
        }
     dataP = GetData();
     return true;
     }
  dataP = NULL;
  return true;
}
