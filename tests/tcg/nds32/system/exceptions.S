/*
 * NDS32 exception, interrupt and standby test for the nds32-virt machine.
 * Exits through the test finisher: 0x5555 on success, (code << 16) | 0x3333
 * on failure.
 */
	.equ FINISHER, 0x0f000000
	.equ VT, 0x30000

	.macro FAIL code
	sethi	$r15, hi20(FINISHER)
	ori	$r15, $r15, lo12(FINISHER)
	movi	$r14, \code
	slli	$r14, $r14, 16
	ori	$r14, $r14, 0x3333
	swi	$r14, [$r15]
1:	j8	1b
	.endm

	.text
	.global _start
	.org 0
_start:
	j	main

/* vector table, 16 bytes per entry: ESZ = 1 */
	.org 0x70		/* vector 7: general exception */
	j	general_handler
	.org 0x80		/* vector 8: syscall */
	j	syscall_handler
	.org 0x170		/* vector 23: software interrupt */
	j	swi_handler
	.org 0x1c0		/* vector 28: external interrupt 19 */
	j	uart_handler

	.org 0x400
main:
	sethi	$sp, 0x100
	/* IVB: base 0, 16-byte entries, 32 interrupts, priority levels */
	movi	$r0, 0x400b
	mtsr	$r0, $ivb
	movi	$r9, 0
	movi	$r8, 0

	/* syscall: ipc = syscall address, itype swid, return to next insn */
	la	$r1, sys_site
sys_site:
	syscall	0x123
	movi	$r2, 1
	bne	$r9, $r2, fail1
	sethi	$r3, 0
	ori	$r3, $r3, 0x123
	slli	$r3, $r3, 16
	bne	$r7, $r3, fail2

	/* reserved instruction raises a general exception, etype 1 */
	movi	$r9, 0
	.byte	0x60, 0x00, 0x00, 0x00
	movi	$r2, 1
	bne	$r9, $r2, fail3
	bne	$r6, $r2, fail4

	/* misaligned word load: alignment check, etype 0, eva = address */
	movi	$r9, 0
	movi	$r6, 5
	addi	$r16, $sp, 1
	movi	$r0, 0
	lw	$r17, [$r16+($r0<<0)]
	movi	$r2, 1
	bne	$r9, $r2, fail15
	bnez	$r6, fail16
	mfsr	$r4, $eva
	bne	$r4, $r16, fail17

	/* software interrupt: pend, unmask, enable gie */
	movi	$r9, 0
	mfsr	$r0, $int_mask
	sethi	$r1, 0x10
	or	$r0, $r0, $r1
	mtsr	$r0, $int_mask
	mtsr	$r1, $int_pend
	mfsr	$r2, $psw
	andi	$r3, $r2, 1
	bnez	$r3, fail5		/* gie must still be clear */
	movi	$r2, 0
	bne	$r9, $r2, fail6		/* pending but gie clear: not taken */
	setgie.e
	nop
	nop
	movi	$r2, 1
	bne	$r9, $r2, fail7
	mfsr	$r2, $int_pend
	bnez	$r2, fail8		/* the handler cleared it */
	mfsr	$r2, $psw
	andi	$r3, $r2, 0x7
	movi	$r4, 1			/* gie set again, intl 0 */
	bne	$r3, $r4, fail9

	/* standby returns at once when an enabled interrupt is pending */
	setgie.d
	movi	$r9, 0
	sethi	$r1, 0x10
	mtsr	$r1, $int_pend
	standby	no_wake_grant
	movi	$r2, 0
	bne	$r9, $r2, fail10
	setgie.e
	nop
	nop
	movi	$r2, 1
	bne	$r9, $r2, fail11

	/* external interrupt 19 from the UART: level, vector 28 */
	setgie.d
	movi	$r13, 0
	movi	$r12, 0
	sethi	$r1, hi20(0x0f001000)
	ori	$r1, $r1, lo12(0x0f001000)
	sethi	$r0, 0x80		/* INT_MASK2 bit 19 */
	mtsr	$r0, $int_mask2
	movi	$r0, 0x0
	swi	$r0, [$r1+4]		/* IER = 0 */
	setgie.e
	movi	$r0, 2
	swi	$r0, [$r1+4]		/* INTL 1, GIE clear inside */
	nop
	nop
	movi	$r2, 1
	bne	$r13, $r2, fail12
	movi	$r2, 2			/* INTL was 1 and GIE clear inside */
	bne	$r12, $r2, fail13
	mfsr	$r0, $psw
	andi	$r0, $r0, 7
	movi	$r2, 1
	bne	$r0, $r2, fail14

	sethi	$r15, hi20(FINISHER)
	ori	$r15, $r15, lo12(FINISHER)
	movi	$r14, 0x5555
	swi	$r14, [$r15]
1:	j8	1b

syscall_handler:
	mfsr	$r5, $ipc
	mfsr	$r7, $itype
	addi	$r9, $r9, 1
	bne	$r5, $r1, fail20
	addi	$r5, $r5, 4
	mtsr	$r5, $ipc
	iret

general_handler:
	mfsr	$r6, $itype
	andi	$r6, $r6, 0xf
	addi	$r9, $r9, 1
	mfsr	$r5, $ipc
	addi	$r5, $r5, 4
	mtsr	$r5, $ipc
	iret

uart_handler:
	mfsr	$r12, $psw
	andi	$r12, $r12, 7
	sethi	$r5, hi20(0x0f001000)
	ori	$r5, $r5, lo12(0x0f001000)
	lwi	$r6, [$r5+8]		/* IIR */
	movi	$r6, 0
	swi	$r6, [$r5+4]		/* IER = 0 drops the line */
	addi	$r13, $r13, 1
	iret

swi_handler:
	addi	$r9, $r9, 1
	movi	$r0, 0
	mtsr	$r0, $int_pend
	iret

fail1:	FAIL 1
fail2:	FAIL 2
fail3:	FAIL 3
fail4:	FAIL 4
fail5:	FAIL 5
fail6:	FAIL 6
fail7:	FAIL 7
fail8:	FAIL 8
fail9:	FAIL 9
fail10:	FAIL 10
fail11:	FAIL 11
fail12:	FAIL 12
fail13:	FAIL 13
fail14:	FAIL 14
fail15:	FAIL 15
fail16:	FAIL 16
fail17:	FAIL 17
fail20:	FAIL 20
