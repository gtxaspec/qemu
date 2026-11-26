#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compute the reference checksum of each gen-diff.py group on the
instruction set simulator inside the Andes gdb ("target sim").

usage: oracle-digests.py GDB CC [GROUP...] > diff-expected.txt
"""
import os
import re
import subprocess
import sys
import tempfile

here = os.path.dirname(os.path.abspath(__file__))
ARGS = sys.argv[1:]
import importlib.util
spec = importlib.util.spec_from_file_location('gen', os.path.join(here, 'gen-diff.py'))
gen = importlib.util.module_from_spec(spec)
sys.argv = ['gen-diff.py', '--list']
try:
    spec.loader.exec_module(gen)
except SystemExit:
    pass


def main():
    gdb, cc = ARGS[0], ARGS[1]
    names = ARGS[2:] or list(gen.body_groups)
    for name in names:
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, 't.S')
            elf = os.path.join(d, 't.elf')
            with open(src, 'w') as f:
                f.write(gen.emit([name], None))
            subprocess.run([cc, '-nostdlib', '-Wl,-Ttext=0x30000',
                            '-Wl,-Tdata=0x500000', '-o', elf, src], check=True)
            n = gen.GC[name]
            script = os.path.join(d, 't.gdb')
            with open(script, 'w') as f:
                f.write('target sim\nload\nbreak done\nrun\nx/%dwx 0x400000\n' % n)
            r = subprocess.run([gdb, '-batch', '-x', script, elf],
                               capture_output=True, text=True, timeout=600)
            words = []
            for line in r.stdout.splitlines():
                m = re.match(r'0x[0-9a-f]+(?: <[^>]*>)?:\s+(.*)', line)
                if m:
                    words += [int(w, 16) for w in m.group(1).split()]
            assert len(words) == n, (name, len(words), n)
            print('%s %08x' % (name, gen.checksum(words)))


main()
