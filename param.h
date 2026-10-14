/*
 * param.h: vtsatip - channel parameters to SAT>IP URL parameters
 */

#ifndef __VTSATIP_PARAM_H
#define __VTSATIP_PARAM_H

#include "common.h"

// returns e.g. "src=1&freq=11493&pol=h&ro=0.35&msys=dvbs2&mtype=8psk&plts=on&sr=22000&fec=23"
cString GetTransponderUrlParameters(const cChannel *channelP);

#endif // __VTSATIP_PARAM_H
