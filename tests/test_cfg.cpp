#include <vdr/tools.h>
#include "common.h"
int StubLogLevel = 0; cVtSatipConfig VtSatipConfig;
static int fails=0;
#define CHECK(c,m) do{ if(!(c)){printf("FAIL: %s\n",m);++fails;}else printf("ok:   %s\n",m);}while(0)
int main(){
  cVtSatipConfig c;
  CHECK(c.ParseCaIds("0x1830,0x1843;0x0500") && c.caids[0][1]==0x1843 && c.caids[1][0]==0x500 && c.caids[1][1]==0, "CA IDs parsed");
  CHECK(strcmp(*c.CaIdsString(), "0x1830,0x1843;0x0500")==0, "CA IDs formatted back");
  int ch[]={0x1843,0}; int other[]={0x9999,0};
  CHECK(c.SlotProvidesCa(1,ch) && !c.SlotProvidesCa(2,ch) && !c.SlotProvidesCa(1,other) && !c.SlotProvidesCa(0,ch), "slot provides CA");
  { // SID filter per slot
    int sidCh[] = {0x1843, 0};
    CHECK(c.ParseSids("4911,0x14ed;;7") && c.sids[0].size() == 2 && c.sids[0][1] == 0x14ed && c.sids[1].empty() && c.sids[2][0] == 7, "SID lists parsed");
    CHECK(strcmp(*c.SidsString(), "4911,5357;;7") == 0, "SID lists formatted back");
    CHECK(c.SlotProvidesCa(1, sidCh, 4911) && !c.SlotProvidesCa(1, sidCh, 1234) && c.SlotProvidesCa(1, sidCh, 0), "slot 1 restricted to its SIDs");
    CHECK(c.ParseCaIds("0x1830,0x1843;0x1843") && c.SlotProvidesCa(2, sidCh, 1234), "slot without SID list takes every service");
    CHECK(!c.ParseSids("abc") && !c.ParseSids("0") && c.sids[0].size() == 2, "invalid SID list rejected, old value kept");
    CHECK(c.ParseSids("") && c.sids[0].empty(), "empty list clears the restriction");
    c.ParseCaIds("0x1830,0x1843;0x0500");
  }
  CHECK(!c.ParseCaIds("0xZZ") && c.caids[0][0]==0x1830, "invalid list rejected, old value kept");
  CHECK(!c.ParseCaIds("1;2;3;4;5"), "too many slots rejected");
  CHECK(c.ParseCiSlots("1,0,2") && c.ciSlot[0]==1 && c.ciSlot[1]==0 && c.ciSlot[2]==2, "CI slot assignment");
  CHECK(!c.ParseCiSlots("9"), "invalid CI slot rejected");
  cVtSatipConfig s;
  CHECK(s.SetupParse("AllPids", "1") && s.allPids, "setup: all PIDs");
  CHECK(s.SetupParse("FrontendDevice1", "3") && s.frontend[1] == 3 && !s.SetupParse("FrontendDevice1", "99") && s.frontend[1] == 3, "setup: frontend per device");
  CHECK(s.SetupParse("EnableCIExtension", "1") && s.ciExtension, "setup: CI extension");
  CHECK(s.SetupParse("CAIDs", "0x1830;0x500") && s.caids[1][0] == 0x500, "setup: CA IDs");
  CHECK(!s.SetupParse("CAIDs", "bogus") && s.caids[1][0] == 0x500, "setup: invalid CA IDs rejected");
  CHECK(s.SetupParse("CISlotDevice1", "2") && s.ciSlot[1] == 2 && !s.SetupParse("CISlotDevice1", "9") && s.ciSlot[1] == 2, "setup: CI slot per device");
  CHECK(s.SetupParse("Discovery", "-1") && s.UseDiscovery() && s.SetupParse("Discovery", "0") && !s.UseDiscovery() && s.SetupParse("Discovery", "1") && s.UseDiscovery(), "setup: discovery modes");
  CHECK(s.SetupParse("EnableEITScan", "0") && !s.eitScan && s.SetupParse("DisconnectIdle", "0") && !s.disconnectIdle && s.SetupParse("Trace", "1") && s.trace == 1 && s.SetupParse("CIRestart", "0") && !s.ciRestart, "setup: simple switches");
  CHECK(s.SetupParse("Transport", "1") && s.transport == VTSATIP_TRANSPORT_MULTICAST && !s.SetupParse("Transport", "7") && s.SetupParse("Transport", "2") && s.transport == VTSATIP_TRANSPORT_TCP, "setup: transport");
  CHECK(!s.SetupParse("Nonsense", "1"), "setup: unknown names are reported");
  printf("%s\n", fails?"SOME TESTS FAILED":"ALL TESTS PASSED"); return fails; }
