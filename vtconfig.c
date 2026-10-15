#include <algorithm>
/*
 * vtconfig.c: vtsatip - configuration helpers
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "common.h"

bool cVtSatipConfig::ParseCaIds(const char *strP)
{
  int tmp[VTSATIP_MAX_CI_SLOTS][VTSATIP_MAX_CAIDS + 1];
  memset(tmp, 0, sizeof(tmp));
  char *buf = strdup(strP ? strP : "");
  if (!buf)
     return false;
  bool ok = true;
  int slot = 0;
  char *saveSlot = NULL;
  for (char *s = strtok_r(buf, ";", &saveSlot); s && ok; s = strtok_r(NULL, ";", &saveSlot)) {
      if (slot >= VTSATIP_MAX_CI_SLOTS) {
         ok = false;
         break;
         }
      int n = 0;
      char *saveId = NULL;
      for (char *t = strtok_r(s, ",", &saveId); t; t = strtok_r(NULL, ",", &saveId)) {
          char *end = NULL;
          long v = strtol(t, &end, 0);
          while (end && (*end == ' ' || *end == '\t'))
                ++end;
          if (!end || *end || v <= 0 || v > 0xFFFF || n >= VTSATIP_MAX_CAIDS) {
             ok = false;
             break;
             }
          tmp[slot][n++] = (int)v;
          }
      ++slot;
      }
  free(buf);
  if (!ok)
     return false;
  memcpy(caids, tmp, sizeof(caids));
  return true;
}

cString cVtSatipConfig::CaIdsString(void) const
{
  cString s = "";
  int last = -1;
  for (int i = 0; i < VTSATIP_MAX_CI_SLOTS; ++i)
      if (caids[i][0])
         last = i;
  for (int i = 0; i <= last; ++i) {
      cString slot = "";
      for (int j = 0; caids[i][j]; ++j)
          slot = cString::sprintf("%s%s0x%04X", *slot, j ? "," : "", caids[i][j]);
      s = cString::sprintf("%s%s%s", *s, i ? ";" : "", *slot);
      }
  return s;
}

bool cVtSatipConfig::ParseCiSlots(const char *strP)
{
  char *buf = strdup(strP ? strP : "");
  if (!buf)
     return false;
  int tmp[VTSATIP_MAX_DEVICES];
  for (int i = 0; i < VTSATIP_MAX_DEVICES; ++i)
      tmp[i] = 0;
  int i = 0;
  bool ok = true;
  char *save = NULL;
  for (char *t = strtok_r(buf, ",", &save); t && i < VTSATIP_MAX_DEVICES; t = strtok_r(NULL, ",", &save)) {
      int v = atoi(t);
      if (v < 0 || v > VTSATIP_MAX_CI_SLOTS) {
         ok = false;
         break;
         }
      tmp[i++] = v;
      }
  free(buf);
  if (ok)
     memcpy(ciSlot, tmp, sizeof(ciSlot));
  return ok;
}

bool cVtSatipConfig::ParseSids(const char *strP)
{
  char *buf = strdup(strP ? strP : "");
  if (!buf)
     return false;
  std::vector<int> tmp[VTSATIP_MAX_CI_SLOTS];
  bool ok = true;
  int slot = 0;
  char *saveSlot = NULL;
  // strtok would skip empty slots, so split by hand
  char *p = buf;
  while (ok) {
        char *end = strchr(p, ';');
        if (end)
           *end = 0;
        if (slot >= VTSATIP_MAX_CI_SLOTS) {
           ok = false;
           break;
           }
        char *save = NULL;
        for (char *t = strtok_r(p, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
            char *e = NULL;
            long v = strtol(t, &e, 0);
            if (*e || v < 1 || v > 0xFFFF) {
               ok = false;
               break;
               }
            tmp[slot].push_back((int)v);
            }
        ++slot;
        if (!end)
           break;
        p = end + 1;
        }
  (void)saveSlot;
  free(buf);
  if (!ok)
     return false;
  for (int i = 0; i < VTSATIP_MAX_CI_SLOTS; ++i)
      sids[i] = tmp[i];
  return true;
}

cString cVtSatipConfig::SidsString(void) const
{
  cString s = "";
  int last = -1;
  for (int i = 0; i < VTSATIP_MAX_CI_SLOTS; ++i)
      if (!sids[i].empty())
         last = i;
  for (int i = 0; i <= last; ++i) {
      cString slot = "";
      for (size_t j = 0; j < sids[i].size(); ++j)
          slot = cString::sprintf("%s%s%d", *slot, j ? "," : "", sids[i][j]);
      s = cString::sprintf("%s%s%s", *s, i ? ";" : "", *slot);
      }
  return s;
}

bool cVtSatipConfig::SlotProvidesCa(int slotP, const int *caidsP, int sidP) const
{
  if (slotP < 1 || slotP > VTSATIP_MAX_CI_SLOTS || !caidsP)
     return false;
  const std::vector<int> &allowed = sids[slotP - 1];
  if (sidP > 0 && !allowed.empty() && std::find(allowed.begin(), allowed.end(), sidP) == allowed.end())
     return false;
  for (const int *ids = caids[slotP - 1]; *ids; ++ids)
      for (const int *id = caidsP; *id; ++id)
          if (*id == *ids)
             return true;
  return false;
}

bool cVtSatipConfig::SetupParse(const char *nameP, const char *valueP)
{
  if (!nameP || !valueP)
     return false;
  int v = atoi(valueP);
  if (!strcasecmp(nameP, "EnableCIExtension"))
     ciExtension = v != 0;
  else if (!strcasecmp(nameP, "CIRestart"))
     ciRestart = v != 0;
  else if (!strcasecmp(nameP, "CAIDs")) {
     if (!ParseCaIds(valueP))
        return false;
     }
  else if (!strcasecmp(nameP, "SIDs")) {
     if (!ParseSids(valueP))
        return false;
     }
  else if (!strncasecmp(nameP, "CISlotDevice", 12)) {
     int device = atoi(nameP + 12);
     if (device < 0 || device >= VTSATIP_MAX_DEVICES || v < 0 || v > VTSATIP_MAX_CI_SLOTS)
        return false;
     ciSlot[device] = v;
     }
  else if (!strcasecmp(nameP, "Discovery"))
     discovery = (v < 0) ? -1 : (v ? 1 : 0);
  else if (!strcasecmp(nameP, "EnableEITScan"))
     eitScan = v != 0;
  else if (!strcasecmp(nameP, "DisconnectIdle"))
     disconnectIdle = v != 0;
  else if (!strcasecmp(nameP, "Transport")) {
     if (v != VTSATIP_TRANSPORT_UNICAST && v != VTSATIP_TRANSPORT_MULTICAST && v != VTSATIP_TRANSPORT_TCP)
        return false;
     transport = v;
     }
  else if (!strcasecmp(nameP, "Trace"))
     trace = v ? 1 : 0;
  else
     return false;
  return true;
}
