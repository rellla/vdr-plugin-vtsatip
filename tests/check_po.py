#!/usr/bin/env python3
# Copyright (C) 2026 Andreas Baierl <ichgeh@imkreisrum.de>
# SPDX-License-Identifier: GPL-2.0-only
"""Checks that every tr()/trNOOP() text of the sources is in the German translation and vice versa."""
import re, glob, os, sys
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
ids = set()
for f in glob.glob(os.path.join(root, "*.c")) + glob.glob(os.path.join(root, "*.h")):
    text = open(f, encoding="utf-8").read()
    for m in re.finditer(r'\btr(?:NOOP)?\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)', text):
        ids.add("".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))))
have = set()
for po in glob.glob(os.path.join(root, "po", "*.po")):
    for block in open(po, encoding="utf-8").read().split("\n\n"):
        if "msgid" not in block or "Project-Id-Version" in block:
            continue
        mid = block.split("msgstr")[0].split("msgid", 1)[1]
        have.add("".join(re.findall(r'"((?:[^"\\]|\\.)*)"', mid)))
have.discard("")
missing, unused = sorted(ids - have), sorted(have - ids)
for s in missing:
    print("not translated:", s)
for s in unused:
    print("unused translation:", s)
print("translation check:", "FAILED" if missing or unused else "OK")
sys.exit(1 if missing or unused else 0)
