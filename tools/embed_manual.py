#!/usr/bin/env python3
"""Compile the in-game manual (n3ds/manual.txt) into a C header: embed_manual.py IN OUT"""
import sys

text = open(sys.argv[1], encoding="utf-8").read()
out = ["// Generated from n3ds/manual.txt by tools/embed_manual.py. Do not edit.",
       "static const char N3DS_MANUAL[] ="]
for line in text.splitlines(True):
    out.append('  "' + line.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"')
out.append(";")
open(sys.argv[2], "w").write("\n".join(out) + "\n")
