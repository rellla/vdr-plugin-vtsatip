/*
 * common.h: vtsatip - common definitions and configuration
 *
 * Copyright (C) 2026 Andreas Baierl <ichgeh@imkreisrum.de>
 *
 * The structure follows the SAT>IP plugin for VDR (https://github.com/FireFlyVDR/vdr-plugin-satip),
 * Copyright (C) Rolf Ahrenberg and contributors.
 * The behaviour follows the satip daemon of vtuner-ng (https://github.com/joed74/vtuner-ng),
 * Copyright (C) 2014 mc.fishdish@gmail.com, maintained by Jochen Dolze;
 * vtuner-ng is based on vtuner by Honza Petrous.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation. See the file COPYING.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Merges the SAT>IP client logic of vtuner-ng (satip daemon + kernel
 * proxy driver) into an in-process VDR device, so no kernel module is
 * needed anymore.
 */

#ifndef __VTSATIP_COMMON_H
#define __VTSATIP_COMMON_H

#include <stdint.h>
#include <string>
#include <vector>
#include <vdr/device.h>
#include <vdr/tools.h>
#include <vdr/config.h>
#include <vdr/i18n.h>

#define VTSATIP_DEFAULT_RTSP_PORT 554
#define VTSATIP_MAX_DEVICES       MAXDEVICES
#define VTSATIP_TSBUFFER_SIZE     KILOBYTE(4096)
#define VTSATIP_MAX_CI_SLOTS      4
#define VTSATIP_MAX_CAIDS         16

// Source types a device may provide (bit mask)
#define VTSATIP_SYS_SAT   0x01
#define VTSATIP_SYS_TERR  0x02
#define VTSATIP_SYS_CABLE 0x04

// PIDs which are always requested from the server, so that PAT, NIT, SDT,
// EIT, ... are available immediately after tuning (same as vtunerc does).
static const int VtSatipStdPids[] = { 0, 16, 17, 18, 19, 20 };
#define VTSATIP_STDPID_COUNT 6

// mediums a SAT>IP server can deliver
enum { VTSATIP_MEDIUM_SAT = 0, VTSATIP_MEDIUM_TERR = 1, VTSATIP_MEDIUM_CABLE = 2, VTSATIP_MEDIUM_COUNT = 3 };
#define VTSATIP_UNLIMITED 99

// how the stream is delivered
// highest server frontend number which can be selected (fe=)
#define VTSATIP_MAX_FRONTENDS 16

enum { VTSATIP_TRANSPORT_UNICAST = 0, VTSATIP_TRANSPORT_MULTICAST = 1, VTSATIP_TRANSPORT_TCP = 2 };

struct cVtManualServer {
  std::string host;
  int port;
  std::string name;
  int caps[VTSATIP_MEDIUM_COUNT]; // number of frontends per medium
  int ci;                           // -1 = assume CI capable (manual default), 0 = no CI, 1 = has CI
  std::vector<std::string> filters; // satellite positions this server may be used for ("S19.2E"), empty = all
};

struct cVtSatipConfig {
  cString host;
  int port;
  int devices;
  int systems;
  int rcvBufSize;
  int portStart;
  int portStop;
  bool disconnectIdle;
  bool allPids;                      // request the whole transponder (pids=all) instead of a PID list
  bool eitScan;                      // let VDR use the devices for EPG scanning
  int transport;                     // VTSATIP_TRANSPORT_*
  std::string mcastInterface;        // address of the local interface used for multicast, empty = system choice
  bool realtime;
  int trace;
  bool detached;                    // start with all devices detached (SVDRP ATTA/DETA)
  int frontend[VTSATIP_MAX_DEVICES]; // 0 = let the server decide
  std::vector<cVtManualServer> servers; // all -s entries; host/port above are the first one
  int discovery;                      // 0 = off, 1 = on, -1 = automatic (on if no -s was given)
  // CI extension (x_pmt / x_ci)
  bool ciExtension;
  bool ciRestart;                    // restart the stream if the "CI state" of a session changes
  int ciSlot[VTSATIP_MAX_DEVICES];   // CI slot (1..VTSATIP_MAX_CI_SLOTS) used by a device, 0 = none
  int caids[VTSATIP_MAX_CI_SLOTS][VTSATIP_MAX_CAIDS + 1]; // zero terminated CA ID lists per slot
  std::vector<int> sids[VTSATIP_MAX_CI_SLOTS]; // service IDs a slot is restricted to, empty = any
  cVtSatipConfig(void)
  : host(""), port(VTSATIP_DEFAULT_RTSP_PORT), devices(1),
    systems(VTSATIP_SYS_SAT | VTSATIP_SYS_TERR | VTSATIP_SYS_CABLE),
    rcvBufSize(0), portStart(0), portStop(0), disconnectIdle(true), allPids(false), eitScan(true), transport(VTSATIP_TRANSPORT_UNICAST),
    realtime(true), trace(0), detached(false), discovery(-1), ciExtension(false), ciRestart(true)
  {
    for (int i = 0; i < VTSATIP_MAX_DEVICES; ++i)
        frontend[i] = ciSlot[i] = 0;
    for (int i = 0; i < VTSATIP_MAX_CI_SLOTS; ++i)
        for (int j = 0; j <= VTSATIP_MAX_CAIDS; ++j)
            caids[i][j] = 0;
  }
  // "0x1000;0x2000,0x2020": CA IDs of slot 1; CA IDs of slot 2
  bool ParseCaIds(const char *strP);
  cString CaIdsString(void) const;
  bool ParseCiSlots(const char *strP);   // "1,2,0": CI slot per device
  // "4911,5301;0x1f40": service IDs allowed for slot 1; for slot 2 (empty = no restriction)
  bool ParseSids(const char *strP);
  cString SidsString(void) const;
  bool SlotProvidesCa(int slotP, const int *caidsP, int sidP = 0) const; // slotP: 1..N
  // settings stored in VDR's setup.conf; returns false for unknown names or invalid values
  bool SetupParse(const char *nameP, const char *valueP);
  bool UseDiscovery(void) const { return discovery < 0 ? servers.empty() : discovery > 0; }
};

extern cVtSatipConfig VtSatipConfig;

#define vtinfo(fmt, ...)  isyslog("vtsatip: " fmt, ##__VA_ARGS__)
#define vterror(fmt, ...) esyslog("vtsatip: " fmt, ##__VA_ARGS__)
#define vtdebug(fmt, ...) do { if (VtSatipConfig.trace) dsyslog("vtsatip: " fmt, ##__VA_ARGS__); } while (0)

inline uint16_t VtTsPid(const uint8_t *p)
{
  return (uint16_t)(((p[1] & 0x1f) << 8) + p[2]);
}

inline uint8_t VtTsPayload(const uint8_t *p)
{
  if (!(p[3] & 0x10))
     return 0;
  if (p[3] & 0x20) {
     if (p[4] > 183)
        return 0;
     return (uint8_t)((184 - 1) - p[4]);
     }
  return 184;
}

#endif // __VTSATIP_COMMON_H
