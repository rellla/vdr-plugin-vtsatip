/*
 * setup.c: vtsatip - setup menu
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

#include <vdr/menu.h>
#include "device.h"
#include "servers.h"
#include "setup.h"

// --- cMenuVtSatipStats ------------------------------------------------------

cMenuVtSatipStats::cMenuVtSatipStats(void)
: cOsdMenu(tr("vtsatip statistics")),
  refreshM(1000),
  lastTimeM(0)
{
  for (int i = 0; i < VTSATIP_MAX_DEVICES; ++i)
      lastBytesM[i] = 0;
  SetMenuCategory(mcText);
  Fill();
  SetHelp(NULL, NULL, NULL, NULL);
}

void cMenuVtSatipStats::Fill(void)
{
  // The lines can be selected, so that the cursor (and with it the page) can be moved when the list is longer
  // than the screen. The page is built again every second; VDR keeps the position if the cursor stays on the
  // same line.
  int cur = Current() < 0 ? 0 : Current();
  Clear();
  int n = 0;
  uint64_t now = cTimeMs::Now();
  double secs = lastTimeM ? (now - lastTimeM) / 1000.0 : 0;
  lastTimeM = now;
  for (int i = 0; i < VtSatipConfig.devices && i < VTSATIP_MAX_DEVICES; ++i) {
      cVtDeviceStats st;
      if (!cVtSatipDevice::GetStats(i, st))
         continue;
      const char *state = st.detached ? tr("detached") : !st.session ? tr("idle") : st.lock ? tr("locked") : tr("no lock");
      Add(new cOsdItem(*cString::sprintf(tr("Device %d: %s (%s)"), i + 1, st.server.empty() ? "-" : st.server.c_str(), state), osUnknown, true), n++ == cur);
      if (st.session) {
         if (!st.channel.empty())
            Add(new cOsdItem(*cString::sprintf(tr("  Channel: %s"), st.channel.c_str()), osUnknown, true), n++ == cur);
         static const char *const transports[] = { "UDP", "multicast", "TCP" };
         Add(new cOsdItem(*cString::sprintf(tr("  Frontend %d, signal %d%%, quality %d%%, %s"), st.frontend, st.strength < 0 ? 0 : st.strength,
                                            st.quality < 0 ? 0 : st.quality, transports[st.transport % 3]), osUnknown, true), n++ == cur);
         double mbit = (secs > 0 && st.bytes >= lastBytesM[i]) ? (st.bytes - lastBytesM[i]) * 8.0 / 1e6 / secs : 0;
         Add(new cOsdItem(*cString::sprintf(tr("  %.1f Mbit/s, total %.1f MB"), mbit, st.bytes / 1e6), osUnknown, true), n++ == cur);
         double pct = (st.packets + st.lost) ? 100.0 * st.lost / (st.packets + st.lost) : 0;
         Add(new cOsdItem(*cString::sprintf(tr("  RTP packets %lu, lost %lu (%.2f%%), reordered %lu"), st.packets, st.lost, pct, st.reordered), osUnknown, true), n++ == cur);
         Add(new cOsdItem(*cString::sprintf(tr("  Longest pause between packets: %lu ms now, %lu ms since tuning"), st.gapMs, st.gapMaxMs), osUnknown, true), n++ == cur);
         Add(new cOsdItem(*cString::sprintf(tr("  Pauses of 200 ms or more: %lu, after a PLAY request: %lu"), st.pauses, st.playPauses), osUnknown, true), n++ == cur);
         if (st.rcvBufKiB > 0)
            Add(new cOsdItem(*cString::sprintf(tr("  RTP receive buffer: %d KiB"), st.rcvBufKiB), osUnknown, true), n++ == cur);
         else if (st.transport == VTSATIP_TRANSPORT_TCP)
            Add(new cOsdItem(tr("  RTP receive buffer: automatic (TCP)"), osUnknown, true), n++ == cur);
         }
      lastBytesM[i] = st.bytes;
      }
  Add(new cOsdItem(tr("Known SAT>IP servers:"), osUnknown, true), n++ == cur);
  std::vector<std::string> lines = cVtServers::Get()->InfoLines();
  for (size_t i = 0; i < lines.size(); ++i)
      Add(new cOsdItem(lines[i].c_str(), osUnknown, true), n++ == cur);
  if (Current() < 0 && Count() > 0) // the list became shorter
     SetCurrent(Get(Count() - 1));
  Display();
}

eOSState cMenuVtSatipStats::ProcessKey(eKeys keyP)
{
  eOSState state = cOsdMenu::ProcessKey(keyP);
  if (state == osUnknown)
     state = osContinue;
  if (keyP == kNone && refreshM.TimedOut()) {
     Fill();
     refreshM.Set(1000);
     }
  return state;
}

// --- cMenuSetupVtSatip -------------------------------------------------------

cMenuSetupVtSatip::cMenuSetupVtSatip(void)
: ciExtensionM(VtSatipConfig.ciExtension),
  ciRestartM(VtSatipConfig.ciRestart),
  discoveryM(VtSatipConfig.discovery + 1),
  eitScanM(VtSatipConfig.eitScan + 1),
  disconnectIdleM(VtSatipConfig.disconnectIdle),
  idleTimeoutM(VtSatipConfig.idleTimeout),
  allPidsM(VtSatipConfig.allPids),
  traceM(VtSatipConfig.trace ? 1 : 0),
  transportM(VtSatipConfig.transport),
  serverLinesM(0)
{
  for (int i = 0; i < VTSATIP_MAX_DEVICES; ++i)
      ciSlotM[i] = VtSatipConfig.ciSlot[i];
  for (int i = 0; i < VTSATIP_MAX_DEVICES; ++i)
      frontendM[i] = VtSatipConfig.frontend[i];
  for (int i = 0; i < VTSATIP_MAX_DEVICES; ++i)
      eitDeviceM[i] = VtSatipConfig.eitDevice[i] + 1;
  cString ids = VtSatipConfig.CaIdsString();
  strn0cpy(caIdsM, *ids, sizeof(caIdsM));
  cString sids = VtSatipConfig.SidsString();
  strn0cpy(sidsM, *sids, sizeof(sidsM));
  discoveryTextsM[0] = tr("automatic");
  discoveryTextsM[1] = trVDR("no");
  discoveryTextsM[2] = trVDR("yes");
  eitDeviceTextsM[0] = tr("as above");
  eitDeviceTextsM[1] = trVDR("no");
  eitDeviceTextsM[2] = trVDR("yes");
  transportTextsM[0] = tr("unicast");
  transportTextsM[1] = tr("multicast");
  transportTextsM[2] = tr("TCP");
  SetMenuCategory(mcSetupPlugins);
  Setup();
  SetHelp(trVDR("Button$Scan"), NULL, NULL, NULL);
}

void cMenuSetupVtSatip::Setup(void)
{
  int current = Current();
  Clear();
  Add(new cMenuEditStraItem(tr("UPnP discovery"), &discoveryM, 3, discoveryTextsM));
  Add(new cMenuEditStraItem(tr("Transport mode"), &transportM, 3, transportTextsM));
  for (int i = 0; i < VtSatipConfig.devices && i < VTSATIP_MAX_DEVICES; ++i)
      Add(new cMenuEditIntItem(*cString::sprintf(tr("VDR device %d: server frontend"), i + 1), &frontendM[i], 0, VTSATIP_MAX_FRONTENDS, tr("auto")));
  Add(new cMenuEditStraItem(tr("Enable EPG scanning"), &eitScanM, 3, discoveryTextsM));
  for (int i = 0; i < VtSatipConfig.devices && i < VTSATIP_MAX_DEVICES; ++i)
      Add(new cMenuEditStraItem(*cString::sprintf(tr("VDR device %d: EPG scanning"), i + 1), &eitDeviceM[i], 3, eitDeviceTextsM));
  Add(new cMenuEditBoolItem(tr("Disconnect idle streams"), &disconnectIdleM));
  Add(new cMenuEditIntItem(tr("Release unused stream after (s)"), &idleTimeoutM, 5, 3600));
  Add(new cMenuEditBoolItem(tr("Request all PIDs (pids=all)"), &allPidsM));
  Add(new cMenuEditBoolItem(tr("Enable CI extension"), &ciExtensionM));
  if (ciExtensionM) {
     for (int i = 0; i < VtSatipConfig.devices && i < VTSATIP_MAX_DEVICES; ++i)
         Add(new cMenuEditIntItem(*cString::sprintf(tr("  VDR device %d: CI slot"), i + 1), &ciSlotM[i], 0, VTSATIP_MAX_CI_SLOTS, tr("none")));
     Add(new cMenuEditStrItem(tr("  CA IDs (slot 1;slot 2;...)"), caIdsM, sizeof(caIdsM), "0123456789abcdefABCDEFxX,;"));
     Add(new cMenuEditStrItem(tr("  Service IDs (slot 1;slot 2;...)"), sidsM, sizeof(sidsM), "0123456789xXabcdefABCDEF,; "));
     Add(new cMenuEditBoolItem(tr("  Restart stream on CI change"), &ciRestartM));
     }
  Add(new cMenuEditBoolItem(tr("Tracing"), &traceM));
  Add(new cOsdItem(tr("Statistics..."), osUser1));
  Add(new cOsdItem(tr("Known SAT>IP servers:"), osUnknown, false));
  std::vector<std::string> lines = cVtServers::Get()->InfoLines();
  serverLinesM = lines.size();
  for (size_t i = 0; i < lines.size(); ++i)
      Add(new cOsdItem(lines[i].c_str(), osUnknown, false));
  SetCurrent(Get(current));
  Display();
}

eOSState cMenuSetupVtSatip::ProcessKey(eKeys keyP)
{
  int oldCi = ciExtensionM;
  eOSState state = cMenuSetupPage::ProcessKey(keyP);
  if (state == osUser1)
     return AddSubMenu(new cMenuVtSatipStats);
  if (state == osUnknown) {
     if (keyP == kRed) {
        VtSatipRescan();
        return osContinue;
        }
     state = osContinue;
     }
  if (keyP != kNone && oldCi != ciExtensionM)
     Setup(); // show or hide the CI items
  else if (keyP == kNone && cVtServers::Get()->InfoLines().size() != serverLinesM)
     Setup(); // a server appeared or vanished
  return state;
}

void cMenuSetupVtSatip::Store(void)
{
  // CA IDs: keep the old list if the new one cannot be parsed
  cVtSatipConfig probe;
  if (probe.ParseCaIds(caIdsM)) {
     VtSatipConfig.ParseCaIds(caIdsM);
     SetupStore("CAIDs", caIdsM);
     }
  else
     Skins.Message(mtError, tr("Invalid CA ID list"));
  cVtSatipConfig probeSids;
  if (probeSids.ParseSids(sidsM)) {
     VtSatipConfig.ParseSids(sidsM);
     SetupStore("SIDs", sidsM);
     }
  else
     Skins.Message(mtError, tr("Invalid service ID list"));
  VtSatipConfig.ciExtension = ciExtensionM != 0;
  VtSatipConfig.ciRestart = ciRestartM != 0;
  VtSatipConfig.discovery = discoveryM - 1;
  VtSatipConfig.eitScan = eitScanM - 1;
  VtSatipConfig.disconnectIdle = disconnectIdleM != 0;
  VtSatipConfig.idleTimeout = idleTimeoutM < 5 ? 5 : idleTimeoutM;
  VtSatipConfig.trace = traceM;
  VtSatipConfig.transport = transportM;
  SetupStore("Transport", VtSatipConfig.transport);
  SetupStore("EnableCIExtension", ciExtensionM);
  SetupStore("CIRestart", ciRestartM);
  SetupStore("Discovery", VtSatipConfig.discovery);
  SetupStore("EnableEITScan", VtSatipConfig.eitScan);
  SetupStore("DisconnectIdle", disconnectIdleM);
  SetupStore("IdleTimeout", VtSatipConfig.idleTimeout);
  VtSatipConfig.allPids = allPidsM != 0;
  SetupStore("AllPids", allPidsM);
  SetupStore("Trace", traceM);
  for (int i = 0; i < VtSatipConfig.devices && i < VTSATIP_MAX_DEVICES; ++i) {
      VtSatipConfig.ciSlot[i] = ciSlotM[i];
      SetupStore(*cString::sprintf("CISlotDevice%d", i), ciSlotM[i]);
      VtSatipConfig.frontend[i] = frontendM[i];
      SetupStore(*cString::sprintf("FrontendDevice%d", i), frontendM[i]);
      VtSatipConfig.eitDevice[i] = eitDeviceM[i] - 1;
      SetupStore(*cString::sprintf("EpgScanDevice%d", i), VtSatipConfig.eitDevice[i]);
      }
  VtSatipSetupChanged();
}
