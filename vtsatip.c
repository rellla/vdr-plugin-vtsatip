/*
 * vtsatip.c: A plugin for the Video Disk Recorder
 *
 * SAT>IP client with the behaviour of the vtuner-ng satip daemon, but
 * running inside VDR - no kernel module required.
 *
 * See the README file for copyright information.
 */

#include <ctype.h>
#include <strings.h>
#include <getopt.h>
#include <vdr/plugin.h>
#include "common.h"
#include "device.h"
#include "discover.h"
#include "servers.h"
#include "setup.h"

#if defined(APIVERSNUM) && APIVERSNUM < 20400
#error "VDR-2.4.0 API version or greater is required!"
#endif

#ifndef VTSATIP_VERSION
#define VTSATIP_VERSION "0.1.0"
#endif

static const char *VERSION_STR = VTSATIP_VERSION;
static const char DESCRIPTION[] = trNOOP("SAT>IP devices (vtuner-ng style)");

cVtSatipConfig VtSatipConfig;
static class cPluginVtSatip *PluginInstance = NULL;

class cPluginVtSatip : public cPlugin {
private:
  cVtDiscover *discoverM;
  bool ParseServer(const char *paramP);
  bool ParseSystems(const char *paramP);
  void ParseFrontends(const char *paramP);
  void ParsePortRange(const char *paramP);
public:
  cPluginVtSatip(void) : discoverM(NULL) { PluginInstance = this; }
  virtual ~cPluginVtSatip() { PluginInstance = NULL; delete discoverM; }
  virtual const char *Version(void) { return VERSION_STR; }
  virtual const char *Description(void) { return tr(DESCRIPTION); }
  virtual const char *CommandLineHelp(void);
  virtual bool ProcessArgs(int argc, char *argv[]);
  virtual bool Initialize(void);
  virtual bool Start(void);
  virtual void Stop(void);
  virtual const char *MainMenuEntry(void) { return NULL; }
  virtual cMenuSetupPage *SetupMenu(void);
  virtual bool SetupParse(const char *Name, const char *Value);
  void RescanServers(void);
  void ApplySetup(void);
  virtual const char **SVDRPHelpPages(void);
  virtual cString SVDRPCommand(const char *Command, const char *Option, int &ReplyCode);
};

const char *cPluginVtSatip::CommandLineHelp(void)
{
  return "  -s HOST[:PORT][|CAPS][|NAME], --server=...\n"
         "                           address of a SAT>IP server, can be given several times.\n"
         "                           CAPS = frontends per delivery system as announced by UPnP,\n"
         "                           e.g. DVBS2-4,DVBT2-2 (default: unlimited for the systems of -D);\n"
         "                           add CI or NOCI to the list to tell whether the server has a CI\n"
         "                           slot (default: yes; UPnP: OctopusNet, minisatip),\n"
         "                           DVBS2-4:S19.2E,S13E limits the server to these satellite positions\n"
         "                           Without -s all servers are found by UPnP.\n"
         "  -u MODE,  --upnp=MODE    UPnP discovery: 0 = off, 1 = on (default: on if no -s is given)\n"
         "  -d NUM,   --devices=NUM  number of VDR devices (= simultaneous streams), default 1\n"
         "  -D LIST,  --delsys=LIST  delivery systems the server offers, comma separated:\n"
         "                           DVBS,DVBS2,DVBT,DVBT2,DVBC (default: all)\n"
         "  -f LIST,  --frontend=LIST\n"
         "                           fixed server frontend (1..N) for each device, comma separated,\n"
         "                           0 = let the server decide (default), e.g. -f 1,2\n"
         "  -c CAIDS, --caids=CAIDS  CI extension: CA IDs supported by each CI slot of the server,\n"
         "                           ids separated by comma, slots by semicolon,\n"
         "                           e.g. -c '0x1830,0x1843;0x0500' (slot 1; slot 2)\n"
         "  -S SIDS,  --sids=SIDS    restrict the CI slots to these service IDs (slots separated by\n"
         "                           semicolon, empty = no restriction), e.g. -S '4911,5301;'\n"
         "  -i LIST,  --ciassign=LIST\n"
         "                           CI slot (1..4) used by each device, 0 = none, e.g. -i 1,0\n"
         "  -Q,       --noci-restart do not restart a stream when its CI state changes\n"
         "                           (default: restart, needed by e.g. Digital Devices OctopusNet)\n"
         "  -T MODE,  --transport=MODE\n"
         "                           how the stream is delivered: unicast (default), multicast or tcp;\n"
         "                           multicast needs IGMP snooping in the switch and falls back to\n"
         "                           unicast for servers which do not offer it; tcp (RTP over TCP,\n"
         "                           interleaved into the RTSP connection) is for networks where\n"
         "                           UDP gets lost, e.g. WLAN/VPN, and falls back to UDP as well\n"
         "  -b ADDR,  --bind=ADDR    address of the local network interface used to join multicast groups\n"
         "  -p START-END, --portrange=START-END\n"
         "                           RTP/RTCP port range (default 45000-47000)\n"
         "  -r SIZE,  --rcvbuf=SIZE  RTP socket receive buffer in bytes (default 2097152)\n"
         "  -A,       --detached     start with all devices detached (see SVDRP ATTA)\n"
         "  -N,       --nodisconnect do not tear down the stream when a device becomes idle\n"
         "  -R,       --norealtime   do not try to give the RTP threads realtime priority\n"
         "  -t MODE,  --trace=MODE   0 = off, 1 = log RTSP traffic and tuning\n";
}

// HOST[:PORT][|CAPS][|NAME], e.g. 192.168.1.20:554|DVBS2-4,DVBT2-2|OctopusNet
bool cPluginVtSatip::ParseServer(const char *paramP)
{
  char *buf = strdup(paramP);
  if (!buf)
     return false;
  cVtManualServer srv;
  srv.port = VTSATIP_DEFAULT_RTSP_PORT;
  for (int i = 0; i < VTSATIP_MEDIUM_COUNT; ++i)
      srv.caps[i] = 0;
  char *caps = strchr(buf, '|');
  if (caps) {
     *caps++ = 0;
     char *name = strchr(caps, '|');
     if (name) {
        *name++ = 0;
        srv.name = name;
        }
     char *filter = strchr(caps, ':'); // DVBS2-4:S19.2E,S13E
     if (filter) {
        *filter++ = 0;
        srv.filters = cVtServers::ParseFilters(filter);
        }
     { // optional CI / NOCI token among the capabilities
       std::string rest;
       char *save = NULL;
       for (char *t = strtok_r(caps, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
           if (!strcasecmp(t, "CI"))
              srv.ci = 1;
           else if (!strcasecmp(t, "NOCI"))
              srv.ci = 0;
           else
              rest += (rest.empty() ? "" : ",") + std::string(t);
           }
       strcpy(caps, rest.c_str());
     }
     if (*caps && !cVtServers::ParseCaps(caps, srv.caps)) {
        free(buf);
        return false;
        }
     }
  char *colon = strrchr(buf, ':');
  if (colon && strchr(buf, ':') == colon) { // exactly one colon: host:port
     *colon = 0;
     int port = atoi(colon + 1);
     if (port <= 0 || port > 65535) {
        free(buf);
        return false;
        }
     srv.port = port;
     }
  bool ok = !isempty(buf);
  if (ok) {
     srv.host = buf;
     if (VtSatipConfig.servers.empty()) { // the first one is also the default server
        VtSatipConfig.host = buf;
        VtSatipConfig.port = srv.port;
        }
     VtSatipConfig.servers.push_back(srv);
     }
  free(buf);
  return ok;
}

bool cPluginVtSatip::ParseSystems(const char *paramP)
{
  int systems = 0;
  char *buf = strdup(paramP);
  if (!buf)
     return false;
  for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
      if (!strcasecmp(t, "DVBS") || !strcasecmp(t, "DVBS2") || !strcasecmp(t, "S"))
         systems |= VTSATIP_SYS_SAT;
      else if (!strcasecmp(t, "DVBT") || !strcasecmp(t, "DVBT2") || !strcasecmp(t, "T"))
         systems |= VTSATIP_SYS_TERR;
      else if (!strcasecmp(t, "DVBC") || !strcasecmp(t, "DVBC_A") || !strcasecmp(t, "C"))
         systems |= VTSATIP_SYS_CABLE;
      else {
         vterror("unknown delivery system '%s'", t);
         free(buf);
         return false;
         }
      }
  free(buf);
  if (!systems)
     return false;
  VtSatipConfig.systems = systems;
  return true;
}

void cPluginVtSatip::ParseFrontends(const char *paramP)
{
  char *buf = strdup(paramP);
  if (!buf)
     return;
  int i = 0;
  for (char *t = strtok(buf, ","); t && i < VTSATIP_MAX_DEVICES; t = strtok(NULL, ","))
      VtSatipConfig.frontend[i++] = atoi(t);
  free(buf);
}

void cPluginVtSatip::ParsePortRange(const char *paramP)
{
  int start = 0, stop = 0;
  if (sscanf(paramP, "%d-%d", &start, &stop) == 2 && start > 0 && stop > start) {
     VtSatipConfig.portStart = start;
     VtSatipConfig.portStop = stop;
     }
  else
     vterror("invalid port range '%s'", paramP);
}

bool cPluginVtSatip::ProcessArgs(int argc, char *argv[])
{
  static const struct option long_options[] = {
    { "server",       required_argument, NULL, 's' },
    { "upnp",         required_argument, NULL, 'u' },
    { "devices",      required_argument, NULL, 'd' },
    { "delsys",       required_argument, NULL, 'D' },
    { "frontend",     required_argument, NULL, 'f' },
    { "caids",        required_argument, NULL, 'c' },
    { "sids",         required_argument, NULL, 'S' },
    { "ciassign",     required_argument, NULL, 'i' },
    { "noci-restart", no_argument,       NULL, 'Q' },
    { "transport",    required_argument, NULL, 'T' },
    { "bind",         required_argument, NULL, 'b' },
    { "portrange",    required_argument, NULL, 'p' },
    { "rcvbuf",       required_argument, NULL, 'r' },
    { "nodisconnect", no_argument,       NULL, 'N' },
    { "norealtime",   no_argument,       NULL, 'R' },
    { "detached",     no_argument,       NULL, 'A' },
    { "trace",        required_argument, NULL, 't' },
    { NULL,           no_argument,       NULL,  0  }
    };
  int c;
  while ((c = getopt_long(argc, argv, "s:u:d:D:f:c:i:S:QT:b:p:r:NRAt:", long_options, NULL)) != -1) {
        switch (c) {
          case 's':
               if (!ParseServer(optarg)) {
                  fprintf(stderr, "vtsatip: invalid server '%s'\n", optarg);
                  return false;
                  }
               break;
          case 'u':
               VtSatipConfig.discovery = atoi(optarg) ? 1 : 0;
               break;
          case 'd':
               VtSatipConfig.devices = atoi(optarg);
               if (VtSatipConfig.devices < 1 || VtSatipConfig.devices > VTSATIP_MAX_DEVICES) {
                  fprintf(stderr, "vtsatip: invalid number of devices '%s'\n", optarg);
                  return false;
                  }
               break;
          case 'D':
               if (!ParseSystems(optarg)) {
                  fprintf(stderr, "vtsatip: invalid delivery system list '%s'\n", optarg);
                  return false;
                  }
               break;
          case 'f':
               ParseFrontends(optarg);
               break;
          case 'c':
               if (!VtSatipConfig.ParseCaIds(optarg)) {
                  fprintf(stderr, "vtsatip: invalid CA ID list '%s'\n", optarg);
                  return false;
                  }
               VtSatipConfig.ciExtension = true;
               break;
          case 'S':
               if (!VtSatipConfig.ParseSids(optarg)) {
                  fprintf(stderr, "vtsatip: invalid SID list '%s'\n", optarg);
                  return false;
                  }
               break;
          case 'i':
               if (!VtSatipConfig.ParseCiSlots(optarg)) {
                  fprintf(stderr, "vtsatip: invalid CI slot assignment '%s'\n", optarg);
                  return false;
                  }
               VtSatipConfig.ciExtension = true;
               break;
          case 'Q':
               VtSatipConfig.ciRestart = false;
               break;
          case 'T':
               if (!strcasecmp(optarg, "unicast"))
                  VtSatipConfig.transport = VTSATIP_TRANSPORT_UNICAST;
               else if (!strcasecmp(optarg, "multicast"))
                  VtSatipConfig.transport = VTSATIP_TRANSPORT_MULTICAST;
               else if (!strcasecmp(optarg, "tcp"))
                  VtSatipConfig.transport = VTSATIP_TRANSPORT_TCP;
               else {
                  fprintf(stderr, "vtsatip: invalid transport '%s' (unicast, multicast or tcp)\n", optarg);
                  return false;
                  }
               break;
          case 'b':
               VtSatipConfig.mcastInterface = optarg;
               break;
          case 'p':
               ParsePortRange(optarg);
               break;
          case 'r':
               VtSatipConfig.rcvBufSize = atoi(optarg);
               break;
          case 'N':
               VtSatipConfig.disconnectIdle = false;
               break;
          case 'R':
               VtSatipConfig.realtime = false;
               break;
          case 'A':
               VtSatipConfig.detached = true;
               break;
          case 't':
               VtSatipConfig.trace = atoi(optarg);
               break;
          default:
               return false;
          }
        }
  return true;
}

bool cPluginVtSatip::Initialize(void)
{
  if (VtSatipConfig.servers.empty() && !VtSatipConfig.UseDiscovery()) {
     vterror("no SAT>IP server configured, use -s HOST[:PORT] or enable UPnP discovery (-u 1)");
     return false;
     }
  // manual servers; without explicit capabilities all enabled delivery systems are offered
  for (size_t i = 0; i < VtSatipConfig.servers.size(); ++i) {
      cVtManualServer &s = VtSatipConfig.servers[i];
      bool any = false;
      for (int m = 0; m < VTSATIP_MEDIUM_COUNT; ++m)
          any |= s.caps[m] > 0;
      if (!any) {
         s.caps[VTSATIP_MEDIUM_SAT]   = (VtSatipConfig.systems & VTSATIP_SYS_SAT)   ? VTSATIP_UNLIMITED : 0;
         s.caps[VTSATIP_MEDIUM_TERR]  = (VtSatipConfig.systems & VTSATIP_SYS_TERR)  ? VTSATIP_UNLIMITED : 0;
         s.caps[VTSATIP_MEDIUM_CABLE] = (VtSatipConfig.systems & VTSATIP_SYS_CABLE) ? VTSATIP_UNLIMITED : 0;
         }
      cVtServers::Get()->AddManual(s.host, s.port, s.name, s.caps, s.filters, s.ci);
      vtinfo("server %s:%d configured", s.host.c_str(), s.port);
      }
  vtinfo("%d device(s), UPnP discovery %s", VtSatipConfig.devices, VtSatipConfig.UseDiscovery() ? "on" : "off");
  if (VtSatipConfig.ciExtension) {
     bool assigned = false;
     for (int i = 0; i < VtSatipConfig.devices; ++i)
         assigned |= VtSatipConfig.ciSlot[i] > 0;
     if (!assigned) {
        // only -c given: slot n is used by device n-1
        for (int i = 0; i < VtSatipConfig.devices && i < VTSATIP_MAX_CI_SLOTS; ++i)
            if (VtSatipConfig.caids[i][0])
               VtSatipConfig.ciSlot[i] = i + 1;
        }
     vtinfo("CI extension enabled, CA IDs: %s", *VtSatipConfig.CaIdsString());
     }
  return cVtSatipDevice::Initialize(VtSatipConfig.devices);
}

bool cPluginVtSatip::Start(void)
{
  if (VtSatipConfig.UseDiscovery() && !discoverM) {
     discoverM = new cVtDiscover();
     discoverM->Start();
     }
  return true;
}

void cPluginVtSatip::Stop(void)
{
  if (discoverM)
     discoverM->Stop();
  cVtSatipDevice::Shutdown();
}

cMenuSetupPage *cPluginVtSatip::SetupMenu(void)
{
  return new cMenuSetupVtSatip;
}

bool cPluginVtSatip::SetupParse(const char *Name, const char *Value)
{
  return VtSatipConfig.SetupParse(Name, Value);
}

void cPluginVtSatip::RescanServers(void)
{
  if (discoverM)
     discoverM->Rescan();
}

// called after the setup was stored: start or stop the UPnP discovery
void cPluginVtSatip::ApplySetup(void)
{
  if (VtSatipConfig.UseDiscovery() && !discoverM) {
     discoverM = new cVtDiscover();
     discoverM->Start();
     }
  else if (!VtSatipConfig.UseDiscovery() && discoverM) {
     discoverM->Stop();
     DELETENULL(discoverM);
     cVtServers::Get()->RemoveDiscovered();
     }
}

void VtSatipRescan(void)
{
  if (PluginInstance)
     PluginInstance->RescanServers();
}

void VtSatipSetupChanged(void)
{
  if (PluginInstance)
     PluginInstance->ApplySetup();
}

const char **cPluginVtSatip::SVDRPHelpPages(void)
{
  static const char *HelpPages[] = {
    "INFO\n"
    "    Print the known SAT>IP servers and the state of all vtsatip devices.",
    "SCAN\n"
    "    Search the network for SAT>IP servers now (UPnP).",
    "ATTA [ <device> ]\n"
    "    Attach the devices (or one device) to the SAT>IP servers again.",
    "DETA [ <device> ]\n"
    "    Detach the devices (or one device) and release their servers,\n"
    "    e.g. to use the frontends with another client. Devices that are\n"
    "    receiving stay attached.",
    "TRAC [ <mode> ]\n"
    "    Get or set the tracing mode (0 = off, 1 = on).",
    NULL
    };
  return HelpPages;
}

cString cPluginVtSatip::SVDRPCommand(const char *Command, const char *Option, int &ReplyCode)
{
  if (strcasecmp(Command, "INFO") == 0)
     return cString::sprintf("%s%s", *cVtServers::Get()->Info(), *cVtSatipDevice::GetStatus());
  if (strcasecmp(Command, "SCAN") == 0) {
     if (!discoverM) {
        ReplyCode = 550;
        return "UPnP discovery is not enabled";
        }
     discoverM->Rescan();
     return "scanning for SAT>IP servers";
     }
  if (strcasecmp(Command, "ATTA") == 0 || strcasecmp(Command, "DETA") == 0) {
     int n = -1;
     if (Option && *Option) {
        n = atoi(Option);
        if (n < 0 || (n == 0 && *Option != '0')) {
           ReplyCode = 501;
           return "invalid device number";
           }
        }
     return cVtSatipDevice::Attach(n, strcasecmp(Command, "ATTA") == 0, ReplyCode);
     }
  if (strcasecmp(Command, "TRAC") == 0) {
     if (Option && *Option)
        VtSatipConfig.trace = atoi(Option);
     return cString::sprintf("tracing mode: %d", VtSatipConfig.trace);
     }
  return NULL;
}

VDRPLUGINCREATOR(cPluginVtSatip); // Don't touch this!
