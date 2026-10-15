/*
 * servers.h: vtsatip - list of known SAT>IP servers (manual and discovered)
 */

#ifndef __VTSATIP_SERVERS_H
#define __VTSATIP_SERVERS_H

#include <stdint.h>
#include <string>
#include <vector>
#include <vdr/thread.h>
#include "common.h"

struct cVtServerInfo {
  std::string id;      // UUID of the device, or host:port for manual entries
  std::string host;
  int port;
  std::string name;
  std::string model;
  int caps[VTSATIP_MEDIUM_COUNT];
  bool manual;
  uint64_t expires;    // monotonic ms, 0 = never
  uint64_t busyUntil[VTSATIP_MEDIUM_COUNT]; // the server refused a stream (503) for this medium
  std::vector<std::string> filters;         // allowed satellite positions, empty = all
};

class cVtServers {
private:
  struct cBinding {
    bool valid;
    std::string id;
    int medium;
    cBinding(void) : valid(false), medium(0) {}
  };
  mutable cMutex mutexM;
  std::vector<cVtServerInfo> serversM;
  cBinding bindingsM[VTSATIP_MAX_DEVICES];

  int Find(const std::string &idP) const;
  int Used(const std::string &idP, int mediumP, int exceptDeviceP) const;
public:
  static cVtServers *Get(void);
  // "DVBS2-4,DVBT2-2" -> frontends per medium; returns false if nothing usable was found
  static bool ParseCaps(const char *strP, int capsP[VTSATIP_MEDIUM_COUNT]);
  static uint64_t NowMs(void);

  void AddManual(const std::string &hostP, int portP, const std::string &nameP, const int capsP[VTSATIP_MEDIUM_COUNT],
                 const std::vector<std::string> &filtersP = std::vector<std::string>());
  // "S19.2E,S13E" -> list; entries are normalised to upper case
  static std::vector<std::string> ParseFilters(const char *strP);
  static bool FilterAllows(const std::vector<std::string> &filtersP, const char *sourceP);
  // returns true if the server was not known before
  bool Update(const std::string &idP, const std::string &hostP, int portP, const std::string &nameP,
              const std::string &modelP, const int capsP[VTSATIP_MEDIUM_COUNT], int maxAgeSecP);
  bool Touch(const std::string &idP, int maxAgeSecP);
  bool Remove(const std::string &idP);
  bool Known(const std::string &idP) const;
  void Expire(void);
  void Clear(void);

  // sourceP: VDR source ("S19.2E") the stream is needed for, NULL = do not care
  bool Provides(int mediumP, const char *sourceP = NULL) const;
  int Count(void) const;
  // binds a device to a server with a free frontend of the medium
  bool Assign(int deviceP, int mediumP, std::string &hostP, int &portP, std::string *nameP = NULL, const char *sourceP = NULL);
  void Unassign(int deviceP);
  // the server of a device refused a stream (503): remember that for a while and move the device to another
  // server if there is one with a free frontend (false: nothing else available)
  void MarkBusy(const std::string &hostP, int portP, int mediumP, int msP);
  bool Reassign(int deviceP, int mediumP, const std::string &failedHostP, int failedPortP,
                std::string &hostP, int &portP, std::string *nameP = NULL, int busyMsP = 15000, const char *sourceP = NULL);
  cString Info(void) const;
  std::vector<std::string> InfoLines(void) const;
  void RemoveDiscovered(void);
};

#endif // __VTSATIP_SERVERS_H
