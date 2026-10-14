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

#if defined(APIVERSNUM) && APIVERSNUM < 20400
#error "VDR-2.4.0 API version or greater is required!"
#endif

#ifndef VTSATIP_VERSION
#define VTSATIP_VERSION "0.1.0"
#endif

static const char *VERSION_STR = VTSATIP_VERSION;
static const char DESCRIPTION[] = trNOOP("SAT>IP devices (vtuner-ng style)");

cVtSatipConfig VtSatipConfig;

class cPluginVtSatip : public cPlugin {
private:
  bool ParseServer(const char *paramP);
  bool ParseSystems(const char *paramP);
  void ParseFrontends(const char *paramP);
  void ParsePortRange(const char *paramP);
public:
  cPluginVtSatip(void) {}
  virtual ~cPluginVtSatip() {}
  virtual const char *Version(void) { return VERSION_STR; }
  virtual const char *Description(void) { return tr(DESCRIPTION); }
  virtual const char *CommandLineHelp(void);
  virtual bool ProcessArgs(int argc, char *argv[]);
  virtual bool Initialize(void);
  virtual bool Start(void);
  virtual void Stop(void);
  virtual const char *MainMenuEntry(void) { return NULL; }
  virtual const char **SVDRPHelpPages(void);
  virtual cString SVDRPCommand(const char *Command, const char *Option, int &ReplyCode);
};

const char *cPluginVtSatip::CommandLineHelp(void)
{
  return "  -s HOST[:PORT], --server=HOST[:PORT]\n"
         "                           address of the SAT>IP server (required, RTSP port default 554)\n"
         "  -d NUM,   --devices=NUM  number of VDR devices (= simultaneous streams), default 1\n"
         "  -D LIST,  --delsys=LIST  delivery systems the server offers, comma separated:\n"
         "                           DVBS,DVBS2,DVBT,DVBT2,DVBC (default: all)\n"
         "  -f LIST,  --frontend=LIST\n"
         "                           fixed server frontend (1..N) for each device, comma separated,\n"
         "                           0 = let the server decide (default), e.g. -f 1,2\n"
         "  -p START-END, --portrange=START-END\n"
         "                           RTP/RTCP port range (default 45000-47000)\n"
         "  -r SIZE,  --rcvbuf=SIZE  RTP socket receive buffer in bytes (default 2097152)\n"
         "  -N,       --nodisconnect do not tear down the stream when a device becomes idle\n"
         "  -R,       --norealtime   do not try to give the RTP threads realtime priority\n"
         "  -t MODE,  --trace=MODE   0 = off, 1 = log RTSP traffic and tuning\n";
}

bool cPluginVtSatip::ParseServer(const char *paramP)
{
  cString s(paramP);
  char *buf = strdup(*s);
  if (!buf)
     return false;
  char *colon = strrchr(buf, ':');
  if (colon && strchr(buf, ':') == colon) { // exactly one colon: host:port
     *colon = 0;
     int port = atoi(colon + 1);
     if (port <= 0 || port > 65535) {
        free(buf);
        return false;
        }
     VtSatipConfig.port = port;
     }
  bool ok = !isempty(buf);
  if (ok)
     VtSatipConfig.host = buf;
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
    { "devices",      required_argument, NULL, 'd' },
    { "delsys",       required_argument, NULL, 'D' },
    { "frontend",     required_argument, NULL, 'f' },
    { "portrange",    required_argument, NULL, 'p' },
    { "rcvbuf",       required_argument, NULL, 'r' },
    { "nodisconnect", no_argument,       NULL, 'N' },
    { "norealtime",   no_argument,       NULL, 'R' },
    { "trace",        required_argument, NULL, 't' },
    { NULL,           no_argument,       NULL,  0  }
    };
  int c;
  while ((c = getopt_long(argc, argv, "s:d:D:f:p:r:NRt:", long_options, NULL)) != -1) {
        switch (c) {
          case 's':
               if (!ParseServer(optarg)) {
                  fprintf(stderr, "vtsatip: invalid server '%s'\n", optarg);
                  return false;
                  }
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
  if (isempty(*VtSatipConfig.host)) {
     vterror("no SAT>IP server configured, use -s HOST[:PORT]");
     return false;
     }
  vtinfo("server %s:%d, %d device(s)", *VtSatipConfig.host, VtSatipConfig.port, VtSatipConfig.devices);
  return cVtSatipDevice::Initialize(VtSatipConfig.devices);
}

bool cPluginVtSatip::Start(void)
{
  return true;
}

void cPluginVtSatip::Stop(void)
{
  cVtSatipDevice::Shutdown();
}

const char **cPluginVtSatip::SVDRPHelpPages(void)
{
  static const char *HelpPages[] = {
    "INFO\n"
    "    Print state of all vtsatip devices.",
    "TRAC [ <mode> ]\n"
    "    Get or set the tracing mode (0 = off, 1 = on).",
    NULL
    };
  return HelpPages;
}

cString cPluginVtSatip::SVDRPCommand(const char *Command, const char *Option, int &ReplyCode)
{
  if (strcasecmp(Command, "INFO") == 0)
     return cVtSatipDevice::GetStatus();
  if (strcasecmp(Command, "TRAC") == 0) {
     if (Option && *Option)
        VtSatipConfig.trace = atoi(Option);
     return cString::sprintf("tracing mode: %d", VtSatipConfig.trace);
     }
  return NULL;
}

VDRPLUGINCREATOR(cPluginVtSatip); // Don't touch this!
