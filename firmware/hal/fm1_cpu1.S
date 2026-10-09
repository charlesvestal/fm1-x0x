/* SPDX-License-Identifier: GPL-3.0-only */
/* The AC79's second core: its entry, as the AC79 SDK's cpu.a/startup.S.o (and Melodee's dual-core
 * audio) have it: interrupts off, no interrupt configuration, its own user and supervisor stacks,
 * then out of the start-up interrupt into fm1_c1_main (in RAM, fm1_cpu1.h), which never returns.
 * fm1_cpu1_start puts this address in the word the chip starts core 1 from (0x01C7FFF8). */
	.section .text.cpu1,"ax",@progbits
	.globl fm1_c1_entry
fm1_c1_entry:
	cli
	icfg = 0
	usp = _c1_ustack_top
	sp = _c1_sstack_top
	ssp = _c1_sstack_top
	reti = fm1_c1_main
	rti

	.section .bss.c1stack,"aw",@nobits
	.balign 8
_c1_ustack:
	.space 256
_c1_ustack_top:
_c1_sstack:
	.space 2816
_c1_sstack_top:
