/* GDEMU.TOS: what ACSI2STM GemDrive's loader does to a second run
 * (tools/gemdrive/run.sh builds and runs it). The game runs twice from
 * the same address, and before the second run starts the part of its
 * BSS past 130,560 bytes (one 255-sector DMA run, as far as GemDrive's
 * clear reaches) gets back what the first run left there, with the
 * rest of free memory scribbled over as the desktop would. */
#include <osbind.h>
#include <stdio.h>
#include <string.h>

#ifndef PROG
#define PROG "FLASHBAK.TOS"
#endif

#define REACH (255L * 512)
static char save[64 * 1024];

static long load(const char *name, long *bbase, long *blen)
{
	long bp = Pexec(3, name, "\0", 0);
	if (bp > 0) {
		*bbase = ((long *)bp)[6];
		*blen = ((long *)bp)[7];
	}
	return bp;
}

static void go(const char *name, long bp, int run)
{
	printf("GDEMU %s run %d returned %ld\n", name, run, Pexec(4, 0, (char *)bp, 0));
	fflush(stdout);
	Mfree((void *)((long *)bp)[11]);   /* Pexec 3 leaves the child's memory to us */
	Mfree((void *)bp);
}

static void pass(const char *name)
{
	long bbase = 0, blen = 0, bp, bp2, bbase2, blen2, tail;

	bp = load(name, &bbase, &blen);
	if (bp <= 0 || blen <= REACH || blen - REACH > (long)sizeof(save)) {
		printf("GDEMU %s: load %ld bss %ld\n", name, bp, blen);
		return;
	}
	tail = blen - REACH;
	go(name, bp, 1);
	memcpy(save, (char *)bbase + REACH, tail);   /* run 1's leftovers, now free memory */
	{
		/* the desktop reuses free memory between runs: scribble it, so
		 * whatever a stale pointer in the tail points at is gone */
		long n = (long)Malloc(-1L);
		char *m = (char *)Malloc(n);
		if (m) {
			memset(m, 0xA5, n);
			Mfree(m);
		}
	}
	bp2 = load(name, &bbase2, &blen2);
	if (bp2 != bp) {
		printf("GDEMU %s: second load at %lx, not %lx\n", name, bp2, bp);
		if (bp2 > 0) {
			Mfree((void *)((long *)bp2)[11]);
			Mfree((void *)bp2);
		}
		return;
	}
	memcpy((char *)bbase + REACH, save, tail);
	printf("GDEMU %s: %ld bytes of BSS left from run 1\n", name, tail);
	fflush(stdout);
	go(name, bp2, 2);
}

int main(void)
{
	/* stdout's buffer first, so the launcher's heap is not carved out
	 * of the memory the game is loaded into */
	printf("GDEMU %s\n", PROG);
	fflush(stdout);
	pass(PROG);
	printf("GDEMU done\n");
	return 0;
}
