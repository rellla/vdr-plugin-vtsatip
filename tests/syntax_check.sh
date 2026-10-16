#!/bin/bash
# Copyright (C) 2026 Andreas Baierl <ichgeh@imkreisrum.de>
# SPDX-License-Identifier: GPL-2.0-only
# Compile-only check of ALL plugin sources against the stand-in VDR headers
# (device, plugin and menu classes cannot be run without a real VDR).
cd "$(dirname "$0")/.." || exit 1
rc=0
for f in *.c; do
  [ "$f" = param.c ] && continue   # needs the real linux/dvb headers (unchanged code of the SAT>IP plugin)
  g++ -std=c++17 -Wall -fsyntax-only -Itests/stubs -I. -DVTSATIP_VERSION='"check"' -x c++ "$f" || rc=1
done
[ $rc = 0 ] && echo "syntax check OK"
exit $rc
