#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the atbm-hera boot ROM stand-in: build a flash image of our own
(a boot_config and a small program at flash 0x1000, linked for the uncached
window), boot it and wait for its line on UART2. A header without the magic
must leave the core stopped, and a chip the camera SoC's QEMU powers off and
on over the link must run its boot_config again.

usage: run-hera-flash.py QEMU CC
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

here = os.path.dirname(os.path.abspath(__file__))
FLASH_SIZE = 2 * 1024 * 1024
AHL_HELLO, AHL_POWER, AHL_RESET = 1, 9, 10
BOOT1 = 0x1000


def config(magic, entry, length):
    words = [magic, 0, entry, length, 0, 0, 115200, 0]
    raw = struct.pack('<8I', *words)
    return raw + struct.pack('<I', 0x12341234 + sum(raw))


def run(qemu, flash, out, log):
    return subprocess.Popen([qemu, '-M', 'atbm-hera', '-display', 'none',
                             '-monitor', 'none', '-serial', 'none',
                             '-serial', 'file:' + out,
                             '-drive', 'if=mtd,format=raw,file=' + flash,
                             '-d', 'guest_errors', '-D', log])


def link_send(sock, kind, level):
    sock.sendall(struct.pack('<4I', kind, 0, 0, level))


def wait_count(out, want, count, secs):
    deadline = time.time() + secs
    while time.time() < deadline:
        if os.path.exists(out) and open(out).read().count(want) >= count:
            return True
        time.sleep(0.1)
    return False


def wait_for(out, want, secs):
    deadline = time.time() + secs
    text = ''
    while time.time() < deadline and want not in text:
        time.sleep(0.1)
        if os.path.exists(out):
            with open(out) as f:
                text = f.read()
    return text


def main():
    qemu, cc = sys.argv[1:3]
    objcopy = cc[:-3] + 'objcopy' if cc.endswith('gcc') else 'objcopy'
    with tempfile.TemporaryDirectory() as d:
        elf = os.path.join(d, 'boot1.elf')
        binf = os.path.join(d, 'boot1.bin')
        subprocess.run([cc, '-nostdlib', '-Wl,--no-relax', '-Wl,-T',
                        os.path.join(here, 'atbm-hera-flash.ld'), '-o', elf,
                        os.path.join(here, 'system', 'atbm-hera-flash.S')],
                       check=True)
        subprocess.run([objcopy, '-O', 'binary', elf, binf], check=True)
        boot1 = open(binf, 'rb').read()

        def image(name, cfg):
            img = bytearray(b'\xff' * FLASH_SIZE)
            img[0:len(cfg)] = cfg
            img[BOOT1:BOOT1 + len(boot1)] = boot1
            path = os.path.join(d, name)
            with open(path, 'wb') as f:
                f.write(img)
            return path

        # entry 0x401040 is _start, past the decoy at the start of boot1
        good = image('good.bin', config(0xab45, 0x401040, len(boot1)))
        out = os.path.join(d, 'good.txt')
        proc = run(qemu, good, out, os.path.join(d, 'good.log'))
        text = wait_for(out, 'HERAFLASH', 20)
        proc.kill()
        proc.wait()
        if text.strip() != 'HERAFLASH OK':
            sys.exit('unexpected UART2 output: %r' % text)

        # the SoC's QEMU powers the chip off and on, then pulses its reset pin
        path = os.path.join(d, 'hera.link')
        out = os.path.join(d, 'link.txt')
        proc = subprocess.Popen(
            [qemu, '-M', 'atbm-hera', '-display', 'none', '-monitor', 'none',
             '-serial', 'none', '-serial', 'file:' + out,
             '-drive', 'if=mtd,format=raw,file=' + good,
             '-chardev', 'socket,id=hl,server=on,wait=off,path=' + path,
             '-global', 'atbm-hera-link.chardev=hl'])
        try:
            deadline = time.time() + 20
            while not os.path.exists(path) and time.time() < deadline:
                time.sleep(0.1)
            sock = socket.socket(socket.AF_UNIX)
            sock.connect(path)
            if struct.unpack('<4I', sock.recv(16))[0] != AHL_HELLO:
                sys.exit('the chip did not open with HELLO')
            link_send(sock, AHL_HELLO, 1)
            if not wait_count(out, 'HERAFLASH OK', 1, 20):
                sys.exit('no boot with the link up')
            link_send(sock, AHL_POWER, 0)
            link_send(sock, AHL_POWER, 1)
            if not wait_count(out, 'HERAFLASH OK', 2, 20):
                sys.exit('no boot from the flash after a power cycle')
            link_send(sock, AHL_RESET, 1)
            link_send(sock, AHL_RESET, 0)
            if not wait_count(out, 'HERAFLASH OK', 3, 20):
                sys.exit('no boot from the flash after the reset pin')
        finally:
            proc.kill()
            proc.wait()

        bad = image('bad.bin', config(0x1234, 0x401040, len(boot1)))
        out = os.path.join(d, 'bad.txt')
        log = os.path.join(d, 'bad.log')
        proc = run(qemu, bad, out, log)
        text = wait_for(out, 'HERAFLASH', 2)
        time.sleep(0.5)
        proc.kill()
        proc.wait()
        if text:
            sys.exit('a header without the magic booted: %r' % text)
        if 'not a boot_config' not in open(log).read():
            sys.exit('no boot ROM complaint in the log')


main()
