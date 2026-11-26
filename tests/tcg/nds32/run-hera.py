#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build system/atbm-hera.S with the NDS32 cross compiler, boot it on the
atbm-hera machine and wait for its line on UART2.

usage: run-hera.py QEMU CC
"""
import os
import subprocess
import sys
import tempfile
import time

here = os.path.dirname(os.path.abspath(__file__))


def main():
    qemu, cc = sys.argv[1:3]
    with tempfile.TemporaryDirectory() as d:
        elf = os.path.join(d, 'hera.elf')
        out = os.path.join(d, 'uart2.txt')
        subprocess.run([cc, '-nostdlib', '-Wl,-T',
                        os.path.join(here, 'atbm-hera.ld'), '-o', elf,
                        os.path.join(here, 'system', 'atbm-hera.S')],
                       check=True)
        proc = subprocess.Popen([qemu, '-M', 'atbm-hera', '-display', 'none',
                                 '-monitor', 'none', '-serial', 'none',
                                 '-serial', 'file:' + out, '-kernel', elf])
        text = ''
        deadline = time.time() + 20
        while time.time() < deadline and '\n' not in text:
            time.sleep(0.1)
            if os.path.exists(out):
                with open(out) as f:
                    text = f.read()
        proc.kill()
        proc.wait()
        if text.strip() != 'HERA OK':
            sys.exit('unexpected UART2 output: %r' % text)


main()
