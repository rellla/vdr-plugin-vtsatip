#pragma once
#include "device.h"
#include "i18n.h"
#include "menuitems.h"
class cOsdObject;
class cPlugin {
public:
  virtual ~cPlugin() {}
  virtual const char *Version() = 0; virtual const char *Description() = 0;
  virtual const char *CommandLineHelp() { return NULL; }
  virtual bool ProcessArgs(int, char **) { return true; }
  virtual bool Initialize() { return true; } virtual bool Start() { return true; } virtual void Stop() {}
  virtual const char *MainMenuEntry() { return NULL; }
  virtual cMenuSetupPage *SetupMenu() { return NULL; }
  virtual bool SetupParse(const char *, const char *) { return false; }
  virtual const char **SVDRPHelpPages() { return NULL; }
  virtual cString SVDRPCommand(const char *, const char *, int &) { return NULL; }
};
#define VDRPLUGINCREATOR(PluginClass) extern "C" void *VDRPluginCreator() { return new PluginClass; }
