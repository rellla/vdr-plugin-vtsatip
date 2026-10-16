/*
 * pat.h: vtsatip - PAT parser and PMT PID cache
 *
 * Copyright (C) 2026 Andreas Baierl <ichgeh@imkreisrum.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation. See the file COPYING.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The CI extension of SAT>IP servers (x_pmt) needs the PID of the PMT of the
 * service to decrypt. The PAT is always part of the stream (standard PIDs), so
 * the plugin finds the PMT PID itself, independent of VDR internals.
 */

#ifndef __VTSATIP_PAT_H
#define __VTSATIP_PAT_H

#include <map>
#include <tuple>
#include <vdr/thread.h>
#include "common.h"
#include "sectionfilter.h"

class cVtPatParser {
private:
  cVtSecFilter *filterM;
  cMutex mutexM;
public:
  cVtPatParser(void);
  ~cVtPatParser();
  void Reset(void);
  // feeds one TS packet, returns true if a valid PAT was completed; programs maps SID -> PMT PID
  bool Process(const uchar *tsP, std::map<int, int> &programsP);
  static bool ParseSection(const uchar *dataP, int lenP, std::map<int, int> &programsP);
  static uint32_t Crc32(const uchar *dataP, int lenP);
};

class cVtPmtCache {
private:
  static cMutex mutexS;
  static std::map<std::tuple<int, int, int>, int> cacheS;
public:
  static void Set(int sourceP, int transponderP, int sidP, int pmtPidP);
  static int Get(int sourceP, int transponderP, int sidP);
};

#endif // __VTSATIP_PAT_H
