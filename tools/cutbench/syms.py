#!/usr/bin/env python3
"""syms.py ELF OUT - a Hatari symbol file from the build's ELF

The code symbols only (nm types T, t and W, no .L labels), sorted, with
the C++ names made into single words Hatari accepts (:: becomes __,
anything else odd an underscore). Load it after the program has
started with `symbols OUT 0x149f2 0x149f2 0x149f2`: the TEXT address
run.sh's TOS 2.06 boot gives the program, plus the address as nm
prints it (no 0xe4 to subtract).
"""
import re
import subprocess
import sys

elf, out = sys.argv[1], sys.argv[2]
nm = subprocess.run(['bash', '-c', 'STCMD_NO_TTY=1 stcmd m68k-atari-mint-nm -n -C "%s" 2>/dev/null' % elf],
                    capture_output=True, text=True).stdout
seen = set()
with open(out, 'w') as f:
    for line in nm.splitlines():
        parts = line.split(' ', 2)
        if len(parts) != 3 or parts[1] not in 'TtWw' or parts[2].startswith('.L'):
            continue
        name = re.sub(r'[^A-Za-z0-9_]', '_', parts[2].replace('::', '__'))
        if (parts[0], name) in seen:
            continue
        seen.add((parts[0], name))
        f.write('%s %s %s\n' % (parts[0], parts[1].upper() if parts[1] != 'W' else 'T', name))
