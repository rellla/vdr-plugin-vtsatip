/*
 * device.c: vtsatip - VDR device backed by a SAT>IP stream
 */

#include <vdr/dvbdevice.h>
#include "param.h"
#include "servers.h"
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
  overflowsM(0),
  patWantedM(false),
  tunedSourceM(0),
  tunedTransponderM(0),
  tunedSidM(0),
  lastCiM(0),
  lastPmtM(0),
  lastMediumM(-1)
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
  return cString::sprintf("vtSAT>IP device %u (CardIndex %d)%s\n  tuner: %s\n  overflows: %lu\n  section filters: %d\n  last channel: %s\n",
                          indexM, CardIndex(), CiSlot() > 0 ? *cString::sprintf(" with CI slot %d", CiSlot()) : "",
                          tunerM ? *tunerM->GetInformation() : "-",
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
  if (patWantedM.load())
     FeedPat(dataP, lengthP);
}

// The PAT tells us the PMT PID of the encrypted service; the CI of the server needs it (x_pmt).
void cVtSatipDevice::FeedPat(const uchar *dataP, int lengthP)
{
  std::map<int, int> programs;
  for (int i = 0; i + TS_SIZE <= lengthP; i += TS_SIZE) {
      if (dataP[i] != TS_SYNC_BYTE || VtTsPid(dataP + i) != 0)
         continue;
      if (patM.Process(dataP + i, programs)) {
         int source = tunedSourceM.load();
         int transponder = tunedTransponderM.load();
         for (std::map<int, int>::const_iterator it = programs.begin(); it != programs.end(); ++it)
             cVtPmtCache::Set(source, transponder, it->first, it->second);
         int pmt = cVtPmtCache::Get(source, transponder, tunedSidM.load());
         if (pmt > 0 && tunerM) {
            vtdebug("device %u: PMT PID %d for service %d", indexM, pmt, tunedSidM.load());
            tunerM->SetPmt(pmt);
            patWantedM = false;
            }
         break;
         }
      }
}

// The server answered 503 (no free frontend, e.g. used by another client): try another server.
void cVtSatipDevice::OnServerBusy(const std::string &hostP, int portP)
{
  cString params, source;
  int ci, pmt, medium;
  {
    cMutexLock lock(&lastMutexM);
    params = lastParamsM;
    source = lastSourceM;
    ci = lastCiM;
    pmt = lastPmtM;
    medium = lastMediumM;
  }
  std::string host, name;
  int port = 0;
  if (medium >= 0 && !isempty(*params) && cVtServers::Get()->Reassign((int)indexM, medium, hostP, portP, host, port, &name, 15000, *source)) {
     vtinfo("server %s:%d has no free frontend, device %u moves to %s:%d", hostP.c_str(), portP, indexM, host.c_str(), port);
     deviceNameM = cString::sprintf("vtSAT>IP %u (%s)", indexM, name.c_str());
     tunerM->SetChannel(*params, ci, pmt, host.c_str(), port);
     }
}

void cVtSatipDevice::OnRetune(void)
{
  patM.Reset();
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

static int MediumOfSource(int sourceP)
{
  switch (cSource::ToChar(sourceP)) {
    case 'S': return VTSATIP_MEDIUM_SAT;
    case 'T': return VTSATIP_MEDIUM_TERR;
    case 'C': return VTSATIP_MEDIUM_CABLE;
    default:  return -1;
    }
}

static bool SystemEnabled(int mediumP)
{
  switch (mediumP) {
    case VTSATIP_MEDIUM_SAT:   return (VtSatipConfig.systems & VTSATIP_SYS_SAT) != 0;
    case VTSATIP_MEDIUM_TERR:  return (VtSatipConfig.systems & VTSATIP_SYS_TERR) != 0;
    case VTSATIP_MEDIUM_CABLE: return (VtSatipConfig.systems & VTSATIP_SYS_CABLE) != 0;
    default:                   return false;
    }
}

bool cVtSatipDevice::ProvidesSource(int sourceP) const
{
  cSource *s = Sources.Get(sourceP);
  // source descriptions starting with '0' disable the source for SAT>IP
  if (s && s->Description() && *(s->Description()) == '0')
     return false;
  int medium = MediumOfSource(sourceP);
  // the delivery system must be enabled (-D) and a known server must have a frontend for it
  return medium >= 0 && SystemEnabled(medium) && cVtServers::Get()->Provides(medium, *cSource::ToString(sourceP));
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

  // A device with an own CI must not claim encrypted channels its CI cannot decrypt:
  // VDR assumes that devices with an internal CAM take care of it.
  bool ciMismatch = channelP && channelP->Ca() >= CA_ENCRYPTED_MIN && CiSlot() > 0 && !ProvidesCa(channelP->Caids());
  if (channelP && !ciMismatch && ProvidesTransponder(channelP)) {
     result = hasPriority;
     if (priorityP > IDLEPRIORITY && Receiving()) {
        if (IsTunedToTransponder(channelP)) {
           if ((channelP->Vpid() && !HasPid(channelP->Vpid())) ||
               (channelP->Apid(0) && !HasPid(channelP->Apid(0))) ||
               (channelP->Dpid(0) && !HasPid(channelP->Dpid(0)))) {
              if (channelP->Ca() >= CA_ENCRYPTED_MIN) {
                 if (CiSlot() > 0 && ProvidesCa(channelP->Caids()))
                    result = true; // decrypted by the CI of the server
                 else if (CamSlot()) {
                    if (CamSlot()->CanDecrypt(channelP))
                       result = true;
                    else
                       needsDetachReceivers = true;
                    }
                 else if (CiSlot() == 0)
                    result = true;
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

int cVtSatipDevice::CiSlot(void) const
{
  return VtSatipConfig.ciExtension ? VtSatipConfig.ciSlot[indexM] : 0;
}

bool cVtSatipDevice::ProvidesCa(const int *caidsP) const
{
  return VtSatipConfig.SlotProvidesCa(CiSlot(), caidsP);
}

bool cVtSatipDevice::HasInternalCam(void)
{
  return CiSlot() > 0;
}

bool cVtSatipDevice::ProvidesEIT(void) const
{
#if defined(APIVERSNUM) && APIVERSNUM < 20403
  return VtSatipConfig.eitScan;
#else
  return VtSatipConfig.eitScan && DeviceHooksProvidesEIT();
#endif
}

int cVtSatipDevice::NumProvidedSystems(void) const
{
  int n = 0;
  if (SystemEnabled(VTSATIP_MEDIUM_SAT) && cVtServers::Get()->Provides(VTSATIP_MEDIUM_SAT))
     n += 2; // DVB-S, DVB-S2
  if (SystemEnabled(VTSATIP_MEDIUM_TERR) && cVtServers::Get()->Provides(VTSATIP_MEDIUM_TERR))
     n += 2; // DVB-T, DVB-T2
  if (SystemEnabled(VTSATIP_MEDIUM_CABLE) && cVtServers::Get()->Provides(VTSATIP_MEDIUM_CABLE))
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
     cVtServers::Get()->Unassign((int)indexM);
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
     cVtServers::Get()->Unassign((int)indexM);
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
  // pick the server with a free frontend for this kind of source (kept while the device stays on it)
  std::string host, serverName;
  int port = 0;
  if (!cVtServers::Get()->Assign((int)indexM, MediumOfSource(channelP->Source()), host, port, &serverName, *cSource::ToString(channelP->Source()))) {
     vterror("no SAT>IP server with a free frontend for '%s' [device %u]", channelP->Name(), indexM);
     return false;
     }
  deviceNameM = cString::sprintf("vtSAT>IP %u (%s)", indexM, serverName.c_str());
  int ci = 0;
  int pmt = 0;
  if (channelP->Ca() >= CA_ENCRYPTED_MIN && CiSlot() > 0 && ProvidesCa(channelP->Caids())) {
     ci = CiSlot();
     pmt = cVtPmtCache::Get(channelP->Source(), channelP->Transponder(), channelP->Sid());
     }
  tunedSourceM = channelP->Source();
  tunedTransponderM = channelP->Transponder();
  tunedSidM = channelP->Sid();
  patM.Reset();
  patWantedM = (ci > 0 && pmt == 0); // learn the PMT PID from the PAT
  channelM = *channelP;
  vtdebug("device %u: '%s' ci=%d pmt=%d", indexM, channelP->Name(), ci, pmt);
  {
    cMutexLock lock(&lastMutexM);
    lastParamsM = params;
    lastSourceM = cSource::ToString(channelP->Source());
    lastCiM = ci;
    lastPmtM = pmt;
    lastMediumM = MediumOfSource(channelP->Source());
  }
  tunerM->SetChannel(*params, ci, pmt, host.c_str(), port);
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
