// compile-only stand-in for VDR's menu API
#pragma once
#include "device.h"
#include "i18n.h"
enum eKeys { kNone, kOk, kBack, kRed, kGreen, kYellow, kBlue, kInfo };
enum eOSState { osUnknown, osContinue, osBack, osEnd, osUser1 };
enum eMenuCategory { mcUndefined, mcSetupPlugins, mcText };
enum eMessageType { mtInfo, mtWarning, mtError };
struct cSkins { void Message(eMessageType, const char *) {} };
extern cSkins Skins;
inline char *strn0cpy(char *dest, const char *src, size_t n) { strncpy(dest, src, n); dest[n - 1] = 0; return dest; }
class cOsdItem { public: cOsdItem(const char *, eOSState = osUnknown, bool = true) {} virtual ~cOsdItem() {} };
class cOsdMenu {
public:
  cOsdMenu(const char * = NULL, int = 0, int = 0, int = 0, int = 0, int = 0) {}
  eOSState AddSubMenu(cOsdMenu *m) { delete m; return osContinue; }
  void Clear() {} void Add(cOsdItem *, bool = false) {} int Count() { return 0; } cOsdItem *Get(int) { return NULL; } int Current() { return 0; } void SetCurrent(cOsdItem *) {}
  virtual void Display() {} void SetHelp(const char *, const char *, const char *, const char *) {} void SetMenuCategory(eMenuCategory) {}
  virtual eOSState ProcessKey(eKeys) { return osUnknown; } virtual ~cOsdMenu() {}
};
class cMenuEditBoolItem : public cOsdItem { public: cMenuEditBoolItem(const char *n, int *, const char * = NULL, const char * = NULL) : cOsdItem(n) {} };
class cMenuEditIntItem : public cOsdItem { public: cMenuEditIntItem(const char *n, int *, int = 0, int = 2147483647, const char * = NULL, const char * = NULL) : cOsdItem(n) {} };
class cMenuEditStrItem : public cOsdItem { public: cMenuEditStrItem(const char *n, char *, int, const char * = NULL) : cOsdItem(n) {} };
class cMenuEditStraItem : public cOsdItem { public: cMenuEditStraItem(const char *n, int *, int, const char *const *) : cOsdItem(n) {} };
class cMenuSetupPage : public cOsdMenu {
protected:
  virtual void Store() = 0;
  void SetupStore(const char *, const char *) {} void SetupStore(const char *, int) {}
public:
  virtual eOSState ProcessKey(eKeys k) { return cOsdMenu::ProcessKey(k); }
};
