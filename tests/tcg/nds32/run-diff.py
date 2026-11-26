#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a gen-diff.py group with the NDS32 cross compiler and run it on
qemu-system-nds32; the program checks its results against diff-expected.txt.

usage: run-diff.py QEMU CC GROUP
"""
import os
import subprocess
import sys
import tempfile

here = os.path.dirname(os.path.abspath(__file__))


def main():
    qemu, cc, group = sys.argv[1:4]
    expect = None
    with open(os.path.join(here, 'diff-expected.txt')) as f:
        for line in f:
            name, val = line.split()
            if name == group:
                expect = val
    if expect is None:
        sys.exit('no reference checksum for %s' % group)
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, 't.S')
        elf = os.path.join(d, 't.elf')
        with open(src, 'w') as f:
            subprocess.run([sys.executable, os.path.join(here, 'gen-diff.py'),
                            '--expect', expect, group], stdout=f, check=True)
        subprocess.run([cc, '-nostdlib', '-Wl,-Ttext=0x30000',
                        '-Wl,-Tdata=0x500000', '-o', elf, src], check=True)
        r = subprocess.run([qemu, '-M', 'nds32-virt', '-display', 'none',
                            '-serial', 'none', '-monitor', 'none',
                            '-kernel', elf], timeout=300)
        sys.exit(r.returncode)


main()
