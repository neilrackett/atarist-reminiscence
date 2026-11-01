
/*
 * REminiscence - Flashback interpreter
 * Atari port Copyright (C) 2026 Neil Rackett
 *
 * The program's entry point, both Atari builds. The Makefile links with
 * -e __st_entry and lists this file first, so the code sits at the start
 * of the program, straight after mintlib's crt0: the ST build's hand-over
 * to the 030 build below keeps that much of itself and no more.
 */

#ifdef ATARIST

#ifdef ATARIST_030
// What __st_entry shows on any machine but a TT or a Falcon, in the
// 40 columns of an ST's console
extern "C" const char st_need030[];
const char st_need030[] =
	"This is Flashback for TT & Falcon,\r\n"
	"download the ST version from\r\n"
	"neilrackett.com/atarist\r\n";
#endif

// Ahead of mintlib's start-up, __st_entry clears the BSS before anything
// reads it, then goes on to crt0 with sp, the basepage at 4(sp) and a0
// as they came. The loader is meant to clear it, and TOS does, but
// ACSI2STM's GemDrive clears in one DMA run that stops after 255 sectors
// (130,560 bytes) of the game's 158K. The rest keeps the last program's
// values, and a second run of the game from a GemDrive drive bombed in
// mintlib's mktime on a stale pointer, or in STDL_Init on a stale
// "initialised" (October 2026). 32 bytes a movem: about 60ms at 8MHz.
//
// Before that both builds ask _MCH what the machine is, in instructions
// a 68000 has: the 030 build's code is 68020 code, which on an ST bombed
// before main. _MCH rather than _CPU, because an ST with a 68030 card
// would run that code but has neither machine's video modes.
//
// - The 030 build, on anything but a TT or a Falcon, says where the ST
//   version is, waits for a key and quits.
// - The ST build, on a TT or a Falcon with FBACK030.TOS beside it, runs
//   that instead, so a player launches one file and each machine gets
//   its build. TOS cannot replace a running program, so this one gives
//   back all of itself but its basepage and this file's code (about a
//   kilobyte; Mshrink keeps the start of a program's memory) and waits
//   for it: started from main, after crt0, it would have kept 800K of
//   a 4MB Falcon - its code, its BSS and its 64K stack. Nothing is
//   copied, so no 68030 cache can hold a stale copy. If the 030 build
//   will not start, this one is already gone: it says so and quits. A
//   player who wants the ST build there removes FBACK030.TOS.
__asm__(
"	.text\n"
"	.globl	__st_entry\n"
"__st_entry:\n"
"	move.l	%a0,-(%sp)\n"
"	pea	9f\n"
"	move.w	#38,-(%sp)\n"             // Supexec, as $5a0 is supervisor-only
"	trap	#14\n"
"	addq.l	#6,%sp\n"
"	move.l	(%sp)+,%a0\n"
"	swap	%d0\n"
"	subq.w	#2,%d0\n"                 // _MCH's high word: 2 a TT, 3 a Falcon
"	cmp.w	#1,%d0\n"
#ifdef ATARIST_030
"	bls.s	5f\n"
"	pea	_st_need030\n"
"	move.w	#9,-(%sp)\n"              // Cconws
"	trap	#1\n"
"	move.w	#7,-(%sp)\n"              // Crawcin
"	trap	#1\n"
"	move.w	#-1,-(%sp)\n"
"	move.w	#76,-(%sp)\n"             // Pterm
"	trap	#1\n"
#else
"	bhi.w	5f\n"                     // an ST, STE or Mega STE: this build
"	move.l	%a0,-(%sp)\n"
"	pea	15f(%pc)\n"               // Fsetdta: the search's result in the
"	move.w	#26,-(%sp)\n"             // stack below, not over the command
"	trap	#1\n"                     // line in the basepage, which crt0
"	addq.l	#6,%sp\n"                 // reads if this build goes on
"	clr.w	-(%sp)\n"                 // Fsfirst, a plain file: is it here?
"	pea	12f(%pc)\n"
"	move.w	#78,-(%sp)\n"
"	trap	#1\n"
"	addq.l	#8,%sp\n"
"	move.l	(%sp)+,%a0\n"
"	tst.l	%d0\n"
"	bne.w	5f\n"                     // no: this build after all
"	move.l	4(%sp),%a4\n"             // the basepage
"	lea	14f(%pc),%sp\n"           // a stack in what is kept
"	lea	14f(%pc),%a3\n"
"	sub.l	%a4,%a3\n"                // basepage to here: what is kept
"	move.l	%a3,-(%sp)\n"
"	move.l	%a4,-(%sp)\n"
"	clr.w	-(%sp)\n"
"	move.w	#74,-(%sp)\n"             // Mshrink: the rest given back
"	trap	#1\n"
"	lea	12(%sp),%sp\n"
"	clr.l	-(%sp)\n"                 // Pexec 0: load and go, this
"	pea	13f(%pc)\n"               // program's environment, no
"	pea	12f(%pc)\n"               // arguments (the game reads none)
"	clr.w	-(%sp)\n"
"	move.w	#75,-(%sp)\n"
"	trap	#1\n"
"	lea	16(%sp),%sp\n"
"	tst.l	%d0\n"
"	bpl.s	11f\n"
"	pea	16f(%pc)\n"               // it would not start
"	move.w	#9,-(%sp)\n"              // Cconws
"	trap	#1\n"
"	addq.l	#6,%sp\n"
"	move.w	#7,-(%sp)\n"              // Crawcin
"	trap	#1\n"
"	addq.l	#2,%sp\n"
"	moveq	#-1,%d0\n"
"11:	move.w	%d0,-(%sp)\n"             // its exit code is this one's
"	move.w	#76,-(%sp)\n"             // Pterm
"	trap	#1\n"
"12:	.asciz	\"FBACK030.TOS\"\n"
"13:	.byte	0,0\n"
"16:	.ascii	\"FBACK030.TOS would not start.\\r\\n\"\n"
"	.asciz	\"Press a key\\r\\n\"\n"
"	.even\n"
"15:	.space	512\n"
"14:\n"
#endif
"5:	move.l	4(%sp),%a1\n"
"	move.l	28(%a1),%d0\n"            // p_blen
"	move.l	24(%a1),%a1\n"            // p_bbase
"	add.l	%d0,%a1\n"                // cleared from the end down
"	moveq	#0,%d1\n"
"	moveq	#31,%d2\n"
"	and.w	%d0,%d2\n"                // the bytes past a multiple of 32
"	bra.s	2f\n"
"1:	move.b	%d1,-(%a1)\n"
"2:	dbf	%d2,1b\n"
"	lsr.l	#5,%d0\n"
"	beq.s	4f\n"
"	moveq	#0,%d2\n"
"	moveq	#0,%d3\n"
"	moveq	#0,%d4\n"
"	moveq	#0,%d5\n"
"	moveq	#0,%d6\n"
"	moveq	#0,%d7\n"
"	move.l	%d1,%a2\n"
"3:	movem.l	%d1-%d7/%a2,-(%a1)\n"
"	subq.l	#1,%d0\n"
"	bne.s	3b\n"
"4:	jmp	__start\n"
"9:	moveq	#0,%d0\n"                 // _MCH, or 0 with no cookie jar (TOS 1.0x)
"	move.l	0x5a0,%d1\n"              // _p_cookies
"	beq.s	7f\n"
"	move.l	%d1,%a0\n"
"6:	move.l	(%a0)+,%d1\n"             // a cookie's name, 0 after the last
"	beq.s	7f\n"
"	move.l	(%a0)+,%d2\n"
"	cmp.l	#0x5f4d4348,%d1\n"        // "_MCH"
"	bne.s	6b\n"
"	move.l	%d2,%d0\n"
"7:	rts\n"
);

#endif // ATARIST
