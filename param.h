/*
 * param.h: vtsatip - channel parameters to SAT>IP URL parameters
 *
 * Copyright (C) 2026 Andreas Baierl <ichgeh@imkreisrum.de>
 *
 * Taken from the SAT>IP plugin for VDR, with small changes.
 * Portions Copyright (C) Rolf Ahrenberg and the contributors of the SAT>IP plugin
 * for VDR (https://github.com/FireFlyVDR/vdr-plugin-satip).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation. See the file COPYING.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __VTSATIP_PARAM_H
#define __VTSATIP_PARAM_H

#include "common.h"

// returns e.g. "src=1&freq=11493&pol=h&ro=0.35&msys=dvbs2&mtype=8psk&plts=on&sr=22000&fec=23"
cString GetTransponderUrlParameters(const cChannel *channelP);

#endif // __VTSATIP_PARAM_H
