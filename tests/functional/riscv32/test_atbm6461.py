#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Boot a flash of our own on the ATBM6461: the stand-in for the boot ROM
# jumps to the image at flash 0x1000, which here is a few instructions
# that write a line to UART1 (no vendor code is involved).

import struct

from qemu_test import QemuSystemTest
from qemu_test import wait_for_console_pattern

FLASH_SIZE = 4 * 1024 * 1024
UART1_THR = 0x16200020


def rv32_image(text):
    insns = [0x162002b7]                    # lui   t0, 0x16200
    for ch in text.encode():
        insns.append((ch << 20) | 0x313)    # addi  t1, zero, ch
        insns.append(0x02628023)            # sb    t1, 0x20(t0)
    insns.append(0x0000006f)                # j     .
    return b''.join(struct.pack('<I', i) for i in insns)


class Atbm6461(QemuSystemTest):

    timeout = 10

    def test_boot_own_flash(self):
        self.set_machine('atbm6461')
        flash = self.scratch_file('flash.bin')
        with open(flash, 'wb') as f:
            f.write(b'\xff' * FLASH_SIZE)
            f.seek(0x1000)
            f.write(rv32_image('ATBM6461 OWN FLASH\r\n'))
        self.vm.add_args('-drive',
                         f'if=mtd,format=raw,file={flash},snapshot=on')
        self.vm.set_console()
        self.vm.launch()
        wait_for_console_pattern(self, 'ATBM6461 OWN FLASH')


if __name__ == '__main__':
    QemuSystemTest.main()
