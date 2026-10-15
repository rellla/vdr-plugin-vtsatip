/*
 * setup.h: vtsatip - setup menu
 */

#ifndef __VTSATIP_SETUP_H
#define __VTSATIP_SETUP_H

#include <vdr/menuitems.h>
#include "common.h"

// implemented by the plugin
void VtSatipRescan(void);          // search for servers now
void VtSatipSetupChanged(void);    // apply changed settings (e.g. start/stop the discovery)

class cMenuSetupVtSatip : public cMenuSetupPage {
private:
  int ciExtensionM;
  int ciRestartM;
  int discoveryM;       // index in discoveryTextsM: 0 = automatic, 1 = off, 2 = on
  int eitScanM;
  int disconnectIdleM;
  int traceM;
  int transportM;       // VTSATIP_TRANSPORT_*
  const char *transportTextsM[3];
  int ciSlotM[VTSATIP_MAX_DEVICES];
  char caIdsM[128];
  char sidsM[128];
  const char *discoveryTextsM[3];
  size_t serverLinesM;

  void Setup(void);
protected:
  virtual eOSState ProcessKey(eKeys keyP);
  virtual void Store(void);
public:
  cMenuSetupVtSatip(void);
};

#endif // __VTSATIP_SETUP_H
