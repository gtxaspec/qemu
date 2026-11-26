/* SPDX-License-Identifier: GPL-2.0-or-later */

ENTRY(_start)

SECTIONS
{
    . = 0x401000;
    .boot1 : { *(.boot1) }
    . = 0x401040;
    .boot1entry : { *(.boot1entry) }
    .boot1rodata : { *(.boot1rodata) }
    /DISCARD/ : { *(.note*) *(.comment*) }
}
