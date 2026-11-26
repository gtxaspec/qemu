/*
 * Boots on the atbm-hera machine from its ILM and prints a line on UART2:
 * "HERA OK" when a jal reached through an ifcall returns to the ifcall's
 * successor, "HERA FAIL" otherwise.
 */
	.equ UART2, 0x16300000
	.equ THR, 0x20
	.equ LSR, 0x34

	.text
	.global _start
_start:
	sethi	$r10, hi20(UART2)
	ori	$r10, $r10, lo12(UART2)
	movi55	$r6, 0
	movi55	$r7, 0
	.byte	0xf8, 0x04		/* ifcall9 to 1f, eight bytes on */
	movi55	$r7, 1			/* the ifcall returns here */
	j	2f
1:	jal	callee			/* returns after the ifcall */
	movi55	$r7, 2
2:	la	$r0, ok
	beqc	$r7, 1, 3f
	la	$r0, fail
3:	jal	puts
4:	j8	4b

callee:
	ret

puts:
	lbi.bi	$r1, [$r0], 1
	beqz	$r1, 6f
5:	lwi	$r2, [$r10 + LSR]
	andi	$r2, $r2, 0x20
	beqz	$r2, 5b
	swi	$r1, [$r10 + THR]
	j8	puts
6:	ret

	.data
ok:	.asciz	"HERA OK\n"
fail:	.asciz	"HERA FAIL\n"
