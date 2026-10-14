/*
 * vtconfig.c: vtsatip - configuration helpers
 */

#include <stdlib.h>
#include <string.h>
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

bool cVtSatipConfig::SlotProvidesCa(int slotP, const int *caidsP) const
{
  if (slotP < 1 || slotP > VTSATIP_MAX_CI_SLOTS || !caidsP)
     return false;
  for (const int *ids = caids[slotP - 1]; *ids; ++ids)
      for (const int *id = caidsP; *id; ++id)
          if (*id == *ids)
             return true;
  return false;
}
