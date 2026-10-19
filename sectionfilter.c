/*
 * sectionfilter.c: vtsatip - software section filter
 *
 * Copyright (C) 2026 Andreas Baierl <ichgeh@imkreisrum.de>
 *
 * Adapted from the software section filter of the SAT>IP plugin for VDR.
 * Portions Copyright (C) Rolf Ahrenberg and the contributors of the SAT>IP plugin
 * for VDR (https://github.com/FireFlyVDR/vdr-plugin-satip).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation. See the file COPYING.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "sectionfilter.h"

// --- cVtSecFilter -----------------------------------------------------------

cVtSecFilter::cVtSecFilter(uint16_t pidP, uint8_t tidP, uint8_t maskP)
: pusiSeenM(0),
  feedCcM(0),
  secBufM(NULL),
  secBufpM(0),
  secLenM(0),
  tsFeedpM(0),
  pidM(pidP),
  tidM(tidP),
  maskM(maskP),
  suspendedM(false)
{
  memset(secBufBaseM, 0, sizeof(secBufBaseM));
  socketM[0] = socketM[1] = -1;
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, socketM) != 0) {
     vterror("cannot open section filter socket (pid %d): %s", pidP, strerror(errno));
     socketM[0] = socketM[1] = -1;
     }
}

cVtSecFilter::~cVtSecFilter()
{
  if (socketM[1] >= 0)
     close(socketM[1]);
  if (socketM[0] >= 0)
     close(socketM[0]);
}

void cVtSecFilter::New(void)
{
  tsFeedpM = secBufpM = secLenM = 0;
  secBufM = secBufBaseM;
}

void cVtSecFilter::Filter(void)
{
  if (secBufM && secLenM > 0 && (tidM & maskM) == (secBufM[0] & maskM)) {
     if (pendingM.size() < (size_t)eMaxPending)
        pendingM.push_back(std::string((const char *)secBufM, secLenM));
     }
}

int cVtSecFilter::CopyDump(const uint8_t *bufP, uint8_t lenP)
{
  if (tsFeedpM >= eMaxSectionFeedSize)
     return 0;
  if (tsFeedpM + lenP > eMaxSectionFeedSize)
     lenP = (uint8_t)(eMaxSectionFeedSize - tsFeedpM);
  if (lenP == 0)
     return 0;
  memcpy(secBufBaseM + tsFeedpM, bufP, lenP);
  tsFeedpM = (uint16_t)(tsFeedpM + lenP);
  uint16_t limit = tsFeedpM;
  secBufM = secBufBaseM + secBufpM;
  while (secBufpM + 2 < limit) {
        uint16_t seclen = (uint16_t)(3 + ((secBufM[1] & 0x0f) << 8) + secBufM[2]);
        if (seclen > eMaxSectionSize || (seclen + secBufpM) > limit)
           return 0;
        secLenM = seclen;
        if (pusiSeenM) {
           Filter();
           secLenM = 0;
           }
        secBufpM = (uint16_t)(secBufpM + seclen);
        secBufM += seclen;
        }
  return 0;
}

void cVtSecFilter::Process(const uint8_t *dataP)
{
  if (dataP[0] != TS_SYNC_BYTE || VtTsPid(dataP) != pidM)
     return;
  uint8_t count = VtTsPayload(dataP);
  if (count == 0)
     return;
  uint8_t p = (uint8_t)(TS_SIZE - count);
  uint8_t cc = (uint8_t)(dataP[3] & 0x0f);
  bool ccok = (((feedCcM + 1) & 0x0f) == cc);
  feedCcM = cc;
  bool dci = false;
  if (dataP[3] & 0x20) // adaptation field: discontinuity indicator
     dci = (dataP[4] > 0) && (dataP[5] & 0x80);
  if (!ccok || dci) {
     // discontinuity: stop feeding until the next PUSI
     pusiSeenM = 0;
     New();
     }
  if (dataP[1] & 0x40) { // PUSI
     if (count > 1 && dataP[p] < count) {
        const uint8_t *before = &dataP[p + 1];
        uint8_t beforeLen = dataP[p];
        const uint8_t *after = &before[beforeLen];
        uint8_t afterLen = (uint8_t)(count - 1 - beforeLen);
        CopyDump(before, beforeLen);
        pusiSeenM = 1;
        New();
        CopyDump(after, afterLen);
        }
     }
  else
     CopyDump(&dataP[p], count);
}

void cVtSecFilter::Reset(void)
{
  pusiSeenM = 0;
  feedCcM = 0;
  New();
  pendingM.clear();
}

bool cVtSecFilter::Pop(std::string &sectionP)
{
  if (pendingM.empty())
     return false;
  sectionP = pendingM.front();
  pendingM.pop_front();
  return true;
}

bool cVtSecFilter::Flush(void)
{
  while (!pendingM.empty()) {
        if (suspendedM || socketM[1] < 0) {
           pendingM.clear();
           break;
           }
        const std::string &s = pendingM.front();
        ssize_t n = send(socketM[1], s.data(), s.size(), MSG_EOR | MSG_DONTWAIT);
        if (n >= 0)
           pendingM.pop_front();
        else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
           return true;
        else {
           vterror("failed to send section data (pid %d): %s", pidM, strerror(errno));
           suspendedM = true;
           pendingM.clear();
           }
        }
  return false;
}

// --- cVtSecHandler ----------------------------------------------------------

cVtSecHandler::cVtSecHandler(int indexP, int bufferLenP)
: cThread(*cString::sprintf("vtsatip#%d sections", indexP)),
  ringM(new cRingBufferLinear(bufferLenP, TS_SIZE, false, *cString::sprintf("vtsatip %d sections", indexP))),
  countM(0),
  clearM(false),
  indexM(indexP)
{
  memset(filtersM, 0, sizeof(filtersM));
  ringM->SetTimeouts(100, 100);
  ringM->SetIoThrottle();
  Start();
}

cVtSecHandler::~cVtSecHandler()
{
  if (Running())
     Cancel(3);
  cMutexLock lock(&mutexM);
  for (int i = 0; i < eMaxFilters; ++i) {
      delete filtersM[i];
      filtersM[i] = NULL;
      }
  countM = 0;
  delete ringM;
  ringM = NULL;
}

void cVtSecHandler::Action(void)
{
  while (Running()) {
        uchar *p;
        int len = 0;
        if (clearM.exchange(false))
           ringM->Clear();
        while (Running() && (p = ringM->Get(len)) != NULL) {
              if (len < TS_SIZE)
                 break;
              if (*p != TS_SYNC_BYTE) {
                 int skip = 1;
                 while (skip < len && p[skip] != TS_SYNC_BYTE)
                       ++skip;
                 ringM->Del(skip);
                 continue;
                 }
              int packets = len / TS_SIZE;
              mutexM.Lock();
              for (int n = 0; n < packets; ++n) {
                  for (int i = 0; i < eMaxFilters; ++i)
                      if (filtersM[i])
                         filtersM[i]->Process(p + n * TS_SIZE);
                  }
              mutexM.Unlock();
              ringM->Del(packets * TS_SIZE);
              }
        // hand the finished sections over to VDR
        struct pollfd fds[eMaxFilters];
        int nfds = 0;
        mutexM.Lock();
        for (int i = 0; i < eMaxFilters; ++i) {
            if (filtersM[i] && filtersM[i]->Flush()) {
               fds[nfds].fd = filtersM[i]->SendFd();
               fds[nfds].events = POLLOUT;
               fds[nfds].revents = 0;
               ++nfds;
               }
            }
        mutexM.Unlock();
        if (nfds > 0)
           poll(fds, nfds, 10);
        }
}

bool cVtSecHandler::Exists(u_short pidP)
{
  cMutexLock lock(&mutexM);
  for (int i = 0; i < eMaxFilters; ++i)
      if (filtersM[i] && filtersM[i]->Pid() == pidP)
         return true;
  return false;
}

int cVtSecHandler::Open(u_short pidP, u_char tidP, u_char maskP)
{
  cMutexLock lock(&mutexM);
  for (int i = 0; i < eMaxFilters; ++i) {
      if (!filtersM[i]) {
         cVtSecFilter *f = new cVtSecFilter(pidP, tidP, maskP);
         if (!f->IsValid()) {
            delete f;
            return -1;
            }
         filtersM[i] = f;
         ++countM;
         return f->Fd();
         }
      }
  return -1;
}

void cVtSecHandler::Close(int handleP)
{
  cMutexLock lock(&mutexM);
  for (int i = 0; i < eMaxFilters; ++i) {
      if (filtersM[i] && filtersM[i]->Fd() == handleP) {
         delete filtersM[i];
         filtersM[i] = NULL;
         --countM;
         break;
         }
      }
}

int cVtSecHandler::GetPid(int handleP)
{
  cMutexLock lock(&mutexM);
  for (int i = 0; i < eMaxFilters; ++i)
      if (filtersM[i] && filtersM[i]->Fd() == handleP)
         return filtersM[i]->Pid();
  return -1;
}

void cVtSecHandler::Write(const uchar *bufP, int lenP)
{
  if (ringM) {
     int n = ringM->Put(bufP, lenP);
     if (n != lenP)
        ringM->ReportOverflow(lenP - n);
     }
}

void cVtSecHandler::Clear(void)
{
  if (ringM)
     clearM = true;
}
