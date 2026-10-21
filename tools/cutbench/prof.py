#!/usr/bin/env python3
"""prof.py PROFILE [N] - the N functions (default 45) taking most cycles

Reads a profile Hatari saved (tools/cutbench/prof.sh) and prints, per
symbol, the milliseconds spent in the function itself, with what it
calls, and its call count. Call counts for code inside the large asm
blocks are unreliable; switch a stage off with ab= to cost it instead.
"""
import re,sys
hz=8021247
tot={}; inc={}; calls={}
for l in open(sys.argv[1]):
    m=re.match(r'Cycles/second:\s+(\d+)',l)
    if m: hz=int(m.group(1))
    if not l.startswith('0x'): continue
    name=l.rsplit(', ',1)[1].strip()
    for m in re.finditer(r'= (\d+) \w+ (\d+)/(\d+)/(\d+)(?:/\d+/\d+)? (\d+)/(\d+)/(\d+)', l):
        c,_,_,ic,_,_,ec=m.groups()
        tot[name]=tot.get(name,0)+int(ec); inc[name]=inc.get(name,0)+int(ic); calls[name]=calls.get(name,0)+int(c)
k=hz/1000
T=sum(tot.values())
print("total exclusive ms %.0f"%(T/k))
pat=sys.argv[3] if len(sys.argv)>3 else None
for n,v in sorted(tot.items(),key=lambda x:-x[1])[:int(sys.argv[2]) if len(sys.argv)>2 else 45]:
    print("%8.0f ms excl %8.0f ms incl %7d calls  %s"%(v/k,inc[n]/k,calls[n],n))
