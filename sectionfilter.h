/*
 * sectionfilter.h: vtsatip - software section filter
 *
 * Section reassembly logic taken from the SAT>IP plugin by Rolf Ahrenberg
 * (which itself follows the Linux dvb_demux code).
 */

#ifndef __VTSATIP_SECTIONFILTER_H
#define __VTSATIP_SECTIONFILTER_H

#include <atomic>
#include <deque>
#include <string>
#include <vdr/ringbuffer.h>
#include <vdr/thread.h>
#include "common.h"

class cVtSecFilter {
private:
  enum {
    eMaxSectionSize     = 4096,
    eMaxSectionFeedSize = (eMaxSectionSize + TS_SIZE),
    eMaxPending         = 64
  };
  int pusiSeenM;
  int feedCcM;
  uint8_t *secBufM;
  uint8_t secBufBaseM[eMaxSectionFeedSize];
  uint16_t secBufpM;
  uint16_t secLenM;
  uint16_t tsFeedpM;
  uint16_t pidM;
  uint8_t tidM;
  uint8_t maskM;
  bool suspendedM;
  int socketM[2];
  std::deque<std::string> pendingM;

  void New(void);
  void Filter(void);
  int CopyDump(const uint8_t *bufP, uint8_t lenP);
public:
  cVtSecFilter(uint16_t pidP, uint8_t tidP, uint8_t maskP);
  ~cVtSecFilter();
  bool IsValid(void) const { return socketM[0] >= 0 && socketM[1] >= 0; }
  void Process(const uint8_t *dataP);
  bool Flush(void);                    // true if data is still pending
  void Reset(void);                    // forget partial data and pending sections
  bool Pop(std::string &sectionP);      // take the next finished section (instead of sending it)
  int Fd(void) const { return socketM[0]; }
  int SendFd(void) const { return socketM[1]; }
  uint16_t Pid(void) const { return pidM; }
};

class cVtSecHandler : public cThread {
private:
  enum { eMaxFilters = 32 };
  cRingBufferLinear *ringM;
  cMutex mutexM;
  cVtSecFilter *filtersM[eMaxFilters];
  std::atomic<int> countM;
  int indexM;
protected:
  virtual void Action(void);
public:
  cVtSecHandler(int indexP, int bufferLenP);
  virtual ~cVtSecHandler();
  bool HasFilters(void) const { return countM.load() > 0; }
  bool Exists(u_short pidP);
  int Open(u_short pidP, u_char tidP, u_char maskP);
  void Close(int handleP);
  int GetPid(int handleP);
  void Write(const uchar *bufP, int lenP);
  void Clear(void);
  int Count(void) const { return countM.load(); }
};

#endif // __VTSATIP_SECTIONFILTER_H
