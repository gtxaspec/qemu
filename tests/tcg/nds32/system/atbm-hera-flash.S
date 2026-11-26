/*
 * A flash image's boot1 code for the atbm-hera machine, linked at the
 * flash's uncached window (0x401000, flash offset 0x1000). The boot_config's
 * entry is _start, not the first word: the word at 0x401000 prints the
 * wrong line, so a ROM that jumped to the start of boot1 instead of the
 * entry fails.
 */
	.equ UART2, 0x16300000
	.equ THR, 0x20
	.equ LSR, 0x34

	.section .boot1, "ax"
	.global _boot1
_boot1:
	sethi	$r0, hi20(wrong)
	ori	$r0, $r0, lo12(wrong)
	j	run

	.section .boot1entry, "ax"
	.global _start
_start:
	sethi	$r0, hi20(ok)
	ori	$r0, $r0, lo12(ok)
run:
	sethi	$r10, hi20(UART2)
	ori	$r10, $r10, lo12(UART2)
1:	lbi.bi	$r1, [$r0], 1
	beqz	$r1, 3f
2:	lwi	$r2, [$r10 + LSR]
	andi	$r2, $r2, 0x20
	beqz	$r2, 2b
	swi	$r1, [$r10 + THR]
	j8	1b
3:	j8	3b

	.section .boot1rodata, "a"
ok:	.asciz	"HERAFLASH OK\n"
wrong:	.asciz	"HERAFLASH WRONG ENTRY\n"
