/* SPDX-License-Identifier: GPL-3.0-only */
/* The AC79's second core: its entry. fm1_cpu1_start (fm1_cpu1.h) puts this address in the word
 * the chip starts core 1 from (0x01C7FFF8, as the AC79 SDK's EnableOtherCpu does), then releases
 * the core. It takes its own stacks, interrupts off, and runs fm1_c1_main (never returns). */
	.section .text.cpu1,"ax",@progbits
	.globl fm1_c1_entry
fm1_c1_entry:
	cli
	sp = _c1_ustack_top
	ssp = _c1_sstack_top
	call fm1_c1_main
	goto .

	.section .bss.c1stack,"aw",@nobits
	.balign 8
_c1_ustack:
	.space 2048
	.globl _c1_ustack_top
_c1_ustack_top:
_c1_sstack:
	.space 512
	.globl _c1_sstack_top
_c1_sstack_top:
