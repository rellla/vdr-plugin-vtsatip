/*
 * setup.c: vtsatip - setup menu
 */

#include <vdr/menu.h>
#include "servers.h"
#include "setup.h"

cMenuSetupVtSatip::cMenuSetupVtSatip(void)
: ciExtensionM(VtSatipConfig.ciExtension),
  ciRestartM(VtSatipConfig.ciRestart),
  discoveryM(VtSatipConfig.discovery + 1),
  eitScanM(VtSatipConfig.eitScan),
  disconnectIdleM(VtSatipConfig.disconnectIdle),
  traceM(VtSatipConfig.trace ? 1 : 0),
  serverLinesM(0)
{
  for (int i = 0; i < VTSATIP_MAX_DEVICES; ++i)
      ciSlotM[i] = VtSatipConfig.ciSlot[i];
  cString ids = VtSatipConfig.CaIdsString();
  strn0cpy(caIdsM, *ids, sizeof(caIdsM));
  discoveryTextsM[0] = tr("automatic");
  discoveryTextsM[1] = trVDR("no");
  discoveryTextsM[2] = trVDR("yes");
  SetMenuCategory(mcSetupPlugins);
  Setup();
  SetHelp(trVDR("Button$Scan"), NULL, NULL, NULL);
}

void cMenuSetupVtSatip::Setup(void)
{
  int current = Current();
  Clear();
  Add(new cMenuEditStraItem(tr("UPnP discovery"), &discoveryM, 3, discoveryTextsM));
  Add(new cMenuEditBoolItem(tr("Enable EPG scanning"), &eitScanM));
  Add(new cMenuEditBoolItem(tr("Disconnect idle streams"), &disconnectIdleM));
  Add(new cMenuEditBoolItem(tr("Enable CI extension"), &ciExtensionM));
  if (ciExtensionM) {
     for (int i = 0; i < VtSatipConfig.devices && i < VTSATIP_MAX_DEVICES; ++i)
         Add(new cMenuEditIntItem(*cString::sprintf(tr("  VDR device %d: CI slot"), i + 1), &ciSlotM[i], 0, VTSATIP_MAX_CI_SLOTS, tr("none")));
     Add(new cMenuEditStrItem(tr("  CA IDs (slot 1;slot 2;...)"), caIdsM, sizeof(caIdsM), "0123456789abcdefABCDEFxX,;"));
     Add(new cMenuEditBoolItem(tr("  Restart stream on CI change"), &ciRestartM));
     }
  Add(new cMenuEditBoolItem(tr("Tracing"), &traceM));
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
  VtSatipConfig.ciExtension = ciExtensionM != 0;
  VtSatipConfig.ciRestart = ciRestartM != 0;
  VtSatipConfig.discovery = discoveryM - 1;
  VtSatipConfig.eitScan = eitScanM != 0;
  VtSatipConfig.disconnectIdle = disconnectIdleM != 0;
  VtSatipConfig.trace = traceM;
  SetupStore("EnableCIExtension", ciExtensionM);
  SetupStore("CIRestart", ciRestartM);
  SetupStore("Discovery", VtSatipConfig.discovery);
  SetupStore("EnableEITScan", eitScanM);
  SetupStore("DisconnectIdle", disconnectIdleM);
  SetupStore("Trace", traceM);
  for (int i = 0; i < VtSatipConfig.devices && i < VTSATIP_MAX_DEVICES; ++i) {
      VtSatipConfig.ciSlot[i] = ciSlotM[i];
      SetupStore(*cString::sprintf("CISlotDevice%d", i), ciSlotM[i]);
      }
  VtSatipSetupChanged();
}
