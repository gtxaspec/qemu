/* SPDX-License-Identifier: GPL-2.0-or-later */

ENTRY(_start)

SECTIONS
{
    . = 0;
    .text : { *(.text*) }
    .data : { *(.data*) }
    /DISCARD/ : { *(.note*) *(.comment*) }
}
