/*
 * servers.c: vtsatip - list of known SAT>IP servers (manual and discovered)
 */

#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "servers.h"

cVtServers *cVtServers::Get(void)
{
  static cVtServers instance;
  return &instance;
}

uint64_t cVtServers::NowMs(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

bool cVtServers::ParseCaps(const char *strP, int capsP[VTSATIP_MEDIUM_COUNT])
{
  for (int i = 0; i < VTSATIP_MEDIUM_COUNT; ++i)
      capsP[i] = 0;
  if (!strP)
     return false;
  char *buf = strdup(strP);
  if (!buf)
     return false;
  bool found = false;
  char *save = NULL;
  for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
      while (*t == ' ')
            ++t;
      int medium = -1;
      if (!strncasecmp(t, "DVBS", 4))
         medium = VTSATIP_MEDIUM_SAT;
      else if (!strncasecmp(t, "DVBT", 4))
         medium = VTSATIP_MEDIUM_TERR;
      else if (!strncasecmp(t, "DVBC", 4))
         medium = VTSATIP_MEDIUM_CABLE;
      if (medium < 0)
         continue; // e.g. ATSC, not supported by this plugin
      const char *dash = strchr(t, '-');
      int n = dash ? atoi(dash + 1) : 1;
      if (n < 1)
         n = 1;
      if (n > capsP[medium]) // "DVBS-2,DVBS2-4": the same frontends, take the larger number
         capsP[medium] = n;
      found = true;
      }
  free(buf);
  return found;
}

int cVtServers::Find(const std::string &idP) const
{
  for (size_t i = 0; i < serversM.size(); ++i)
      if (serversM[i].id == idP)
         return (int)i;
  return -1;
}

int cVtServers::Used(const std::string &idP, int mediumP, int exceptDeviceP) const
{
  int n = 0;
  for (int d = 0; d < VTSATIP_MAX_DEVICES; ++d)
      if (d != exceptDeviceP && bindingsM[d].valid && bindingsM[d].id == idP && bindingsM[d].medium == mediumP)
         ++n;
  return n;
}

std::vector<std::string> cVtServers::ParseFilters(const char *strP)
{
  std::vector<std::string> list;
  if (!strP)
     return list;
  char *buf = strdup(strP);
  if (!buf)
     return list;
  char *save = NULL;
  for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
      while (*t == ' ')
            ++t;
      std::string f;
      for (; *t && *t != ' '; ++t)
          f += (char)toupper((unsigned char)*t);
      if (!f.empty())
         list.push_back(f);
      }
  free(buf);
  return list;
}

bool cVtServers::FilterAllows(const std::vector<std::string> &filtersP, const char *sourceP)
{
  if (filtersP.empty() || !sourceP || !*sourceP)
     return true;
  std::string s;
  for (const char *p = sourceP; *p; ++p)
      s += (char)toupper((unsigned char)*p);
  for (size_t i = 0; i < filtersP.size(); ++i)
      if (filtersP[i] == s)
         return true;
  return false;
}

void cVtServers::AddManual(const std::string &hostP, int portP, const std::string &nameP, const int capsP[VTSATIP_MEDIUM_COUNT],
                           const std::vector<std::string> &filtersP)
{
  cMutexLock lock(&mutexM);
  cVtServerInfo s;
  s.id = hostP + ":" + std::to_string(portP);
  s.host = hostP;
  s.port = portP;
  s.name = nameP.empty() ? hostP : nameP;
  s.manual = true;
  s.expires = 0;
  s.filters = filtersP;
  for (int i = 0; i < VTSATIP_MEDIUM_COUNT; ++i)
      s.busyUntil[i] = 0;
  for (int i = 0; i < VTSATIP_MEDIUM_COUNT; ++i)
      s.caps[i] = capsP[i];
  int i = Find(s.id);
  if (i >= 0)
     serversM[i] = s;
  else
     serversM.push_back(s);
}

bool cVtServers::Update(const std::string &idP, const std::string &hostP, int portP, const std::string &nameP,
                        const std::string &modelP, const int capsP[VTSATIP_MEDIUM_COUNT], int maxAgeSecP)
{
  cMutexLock lock(&mutexM);
  for (size_t k = 0; k < serversM.size(); ++k)
      if (serversM[k].manual && serversM[k].host == hostP && serversM[k].port == portP)
         return false; // configured by hand already
  int i = Find(idP);
  bool isNew = (i < 0);
  cVtServerInfo s;
  s.id = idP;
  s.host = hostP;
  s.port = portP;
  s.name = nameP.empty() ? hostP : nameP;
  s.model = modelP;
  s.manual = false;
  for (int k = 0; k < VTSATIP_MEDIUM_COUNT; ++k)
      s.busyUntil[k] = 0;
  s.expires = NowMs() + (uint64_t)(maxAgeSecP > 0 ? maxAgeSecP : 1800) * 1000;
  for (int k = 0; k < VTSATIP_MEDIUM_COUNT; ++k)
      s.caps[k] = capsP[k];
  if (isNew)
     serversM.push_back(s);
  else {
     if (serversM[i].manual)
        return false; // a manual entry wins over the same device found by UPnP
     serversM[i] = s;
     }
  return isNew;
}

bool cVtServers::Touch(const std::string &idP, int maxAgeSecP)
{
  cMutexLock lock(&mutexM);
  int i = Find(idP);
  if (i < 0)
     return false;
  if (!serversM[i].manual)
     serversM[i].expires = NowMs() + (uint64_t)(maxAgeSecP > 0 ? maxAgeSecP : 1800) * 1000;
  return true;
}

bool cVtServers::Remove(const std::string &idP)
{
  cMutexLock lock(&mutexM);
  int i = Find(idP);
  if (i < 0 || serversM[i].manual)
     return false;
  serversM.erase(serversM.begin() + i);
  return true;
}

bool cVtServers::Known(const std::string &idP) const
{
  cMutexLock lock(&mutexM);
  return Find(idP) >= 0;
}

void cVtServers::Expire(void)
{
  cMutexLock lock(&mutexM);
  uint64_t now = NowMs();
  for (size_t i = 0; i < serversM.size();) {
      if (!serversM[i].manual && serversM[i].expires && serversM[i].expires < now) {
         vtinfo("server '%s' (%s) expired", serversM[i].name.c_str(), serversM[i].host.c_str());
         serversM.erase(serversM.begin() + i);
         }
      else
         ++i;
      }
}

void cVtServers::Clear(void)
{
  cMutexLock lock(&mutexM);
  serversM.clear();
  for (int d = 0; d < VTSATIP_MAX_DEVICES; ++d)
      bindingsM[d] = cBinding();
}

bool cVtServers::Provides(int mediumP, const char *sourceP) const
{
  cMutexLock lock(&mutexM);
  for (size_t i = 0; i < serversM.size(); ++i)
      if (mediumP >= 0 && mediumP < VTSATIP_MEDIUM_COUNT && serversM[i].caps[mediumP] > 0 &&
          (mediumP != VTSATIP_MEDIUM_SAT || FilterAllows(serversM[i].filters, sourceP)))
         return true;
  return false;
}

int cVtServers::Count(void) const
{
  cMutexLock lock(&mutexM);
  return (int)serversM.size();
}

bool cVtServers::Assign(int deviceP, int mediumP, std::string &hostP, int &portP, std::string *nameP, const char *sourceP)
{
  if (deviceP < 0 || deviceP >= VTSATIP_MAX_DEVICES || mediumP < 0 || mediumP >= VTSATIP_MEDIUM_COUNT)
     return false;
  cMutexLock lock(&mutexM);
  cBinding &b = bindingsM[deviceP];
  if (b.valid) {
     int i = Find(b.id);
     if (i >= 0 && b.medium == mediumP && serversM[i].caps[mediumP] > 0 &&
         (mediumP != VTSATIP_MEDIUM_SAT || FilterAllows(serversM[i].filters, sourceP))) { // keep the server, it is ours already
        hostP = serversM[i].host;
        portP = serversM[i].port;
        if (nameP)
           *nameP = serversM[i].name;
        return true;
        }
     b = cBinding();
     }
  uint64_t now = NowMs();
  for (size_t i = 0; i < serversM.size(); ++i) {
      const cVtServerInfo &s = serversM[i];
      if (s.busyUntil[mediumP] > now)
         continue;
      if (mediumP == VTSATIP_MEDIUM_SAT && !FilterAllows(s.filters, sourceP))
         continue;
      if (s.caps[mediumP] > Used(s.id, mediumP, deviceP)) {
         b.valid = true;
         b.id = s.id;
         b.medium = mediumP;
         hostP = s.host;
         portP = s.port;
         if (nameP)
            *nameP = s.name;
         return true;
         }
      }
  return false;
}

void cVtServers::Unassign(int deviceP)
{
  if (deviceP < 0 || deviceP >= VTSATIP_MAX_DEVICES)
     return;
  cMutexLock lock(&mutexM);
  bindingsM[deviceP] = cBinding();
}

cString cVtServers::Info(void) const
{
  cMutexLock lock(&mutexM);
  if (serversM.empty())
     return "no SAT>IP servers known\n";
  cString s = "";
  static const char *names[] = { "sat", "terr", "cable" };
  for (size_t i = 0; i < serversM.size(); ++i) {
      const cVtServerInfo &v = serversM[i];
      cString caps = "";
      for (int m = 0; m < VTSATIP_MEDIUM_COUNT; ++m)
          if (v.caps[m] > 0) {
             int used = Used(v.id, m, -1);
             caps = cString::sprintf("%s%s%s %d/%s", *caps, isempty(*caps) ? "" : ", ", names[m], used,
                                     v.caps[m] >= VTSATIP_UNLIMITED ? "unlimited" : *cString::sprintf("%d", v.caps[m]));
             }
      std::string flt;
      for (size_t k = 0; k < v.filters.size(); ++k)
          flt += (k ? "," : "") + v.filters[k];
      s = cString::sprintf("%sserver %s:%d '%s'%s%s [%s]%s%s (%s)\n", *s, v.host.c_str(), v.port, v.name.c_str(),
                           v.model.empty() ? "" : " model ", v.model.c_str(), *caps,
                           flt.empty() ? "" : " only ", flt.c_str(), v.manual ? "manual" : "UPnP");
      }
  return s;
}

std::vector<std::string> cVtServers::InfoLines(void) const
{
  std::vector<std::string> lines;
  cString info = Info();
  std::string s(*info ? *info : "");
  size_t pos = 0;
  while (pos < s.size()) {
        size_t eol = s.find('\n', pos);
        if (eol == std::string::npos)
           eol = s.size();
        if (eol > pos)
           lines.push_back(s.substr(pos, eol - pos));
        pos = eol + 1;
        }
  return lines;
}

void cVtServers::RemoveDiscovered(void)
{
  cMutexLock lock(&mutexM);
  for (size_t i = 0; i < serversM.size();) {
      if (!serversM[i].manual)
         serversM.erase(serversM.begin() + i);
      else
         ++i;
      }
}

void cVtServers::MarkBusy(const std::string &hostP, int portP, int mediumP, int msP)
{
  if (mediumP < 0 || mediumP >= VTSATIP_MEDIUM_COUNT)
     return;
  cMutexLock lock(&mutexM);
  for (size_t i = 0; i < serversM.size(); ++i)
      if (serversM[i].host == hostP && serversM[i].port == portP)
         serversM[i].busyUntil[mediumP] = NowMs() + msP;
}

bool cVtServers::Reassign(int deviceP, int mediumP, const std::string &failedHostP, int failedPortP,
                          std::string &hostP, int &portP, std::string *nameP, int busyMsP, const char *sourceP)
{
  MarkBusy(failedHostP, failedPortP, mediumP, busyMsP);
  if (deviceP < 0 || deviceP >= VTSATIP_MAX_DEVICES || mediumP < 0 || mediumP >= VTSATIP_MEDIUM_COUNT)
     return false;
  cMutexLock lock(&mutexM);
  uint64_t now = NowMs();
  for (size_t i = 0; i < serversM.size(); ++i) {
      const cVtServerInfo &s = serversM[i];
      if (s.busyUntil[mediumP] > now || (s.host == failedHostP && s.port == failedPortP))
         continue;
      if (mediumP == VTSATIP_MEDIUM_SAT && !FilterAllows(s.filters, sourceP))
         continue;
      if (s.caps[mediumP] > Used(s.id, mediumP, deviceP)) {
         cBinding &b = bindingsM[deviceP];
         b.valid = true;
         b.id = s.id;
         b.medium = mediumP;
         hostP = s.host;
         portP = s.port;
         if (nameP)
            *nameP = s.name;
         return true;
         }
      }
  return false;
}
