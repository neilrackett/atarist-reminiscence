
/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * The platform layer both Atari builds share (systemstub_atari.h).
 */

#include <osbind.h>

#include "systemstub_atari.h"
#include "util.h"
#include "video_st.h"

// key releases are applied at the start of the *next* poll so a
// press+release inside one slow frame is still seen by the game
static uint8_t g_upDirMask;
static bool g_upEnter, g_upSpace, g_upShift;

void SystemStub_Atari::initMachine() {
	memset(&_pi, 0, sizeof(_pi));
	g_upDirMask = 0;
	g_upEnter = g_upSpace = g_upShift = false;
	if (STDL_Init(0x20 | 0x200) != 0) { // VIDEO | JOYSTICK
		error("STDL_Init failed");
	}
	// The game never reads the mouse, and leaving it reporting costs
	// three IKBD interrupts per movement. With a border open those
	// delay the timer interrupt that reopens it, so moving the mouse
	// flickered the screen - measured on hardware, where turning
	// reporting off stopped both the flicker and the growth of the
	// missed-border count.
	//
	// It also retires a workaround: while the mouse reports, the
	// IKBD takes joystick 1's fire button and delivers it as the
	// right mouse button instead, which STDL has to fold back.
	// Silenced, the button arrives in the joystick's own packet.
	// STDL puts reporting back when it releases the keyboard,
	// including down the terminate path, so the desktop still gets
	// a working pointer if we crash.
	STDL_EnableMouse(0);
	if (!g_options.megaste_speedup) {
		// megaste_speedup=false: leave a Mega STE at whatever speed
		// it was set to before the game ran, rather than switching
		// it to 16MHz with the cache. Through the library only: it
		// keeps the border timing calibrated to the clock in use.
		STDL_UseMegaSteSpeedup(0);
	}
	if (!g_options.blitter) {
		// diagnostic: a BLiTTER operation runs in hog mode while a
		// border is open and cannot be interrupted once started, so
		// turning it off says whether it is what makes a border
		// interrupt miss its window on a busy frame
		STDL_UseBlitter(0);
		info("BLiTTER disabled (blitter=false)");
	}
}

static void ST_logMachine();

void SystemStub_Atari::logMachine() {
	ST_ramK();
	if (g_options.logging) {
		ST_logMachine();
	}
}

void SystemStub_Atari::initJoystick() {
	// Joystick and pad both drive the game as synthesised keypresses,
	// so the input code below needs to know nothing about either.
	//
	// Arrows and fire are all an ST joystick has, and fire is Shift,
	// Flashback's main verb. The rest exist only when a controller is
	// present; binding them on a machine without one is harmless, as
	// unbound inputs stay silent.
	//
	// Save, load, quit and the state-slot keys are deliberately absent:
	// they are Ctrl combinations, and emulation synthesises one key per
	// input, so they stay on the keyboard where they belong.
	STDL_JoystickKeyMapping(STDLK_UP, STDLK_DOWN, STDLK_LEFT, STDLK_RIGHT, STDLK_RSHIFT);

	STDL_JoystickKeyBind(STDL_JOYSTICK_EAST,   STDLK_RETURN);     // use item
	STDL_JoystickKeyBind(STDL_JOYSTICK_NORTH,  STDLK_BACKSPACE);  // inventory
	STDL_JoystickKeyBind(STDL_JOYSTICK_WEST,   STDLK_SPACE);
	STDL_JoystickKeyBind(STDL_JOYSTICK_START,  STDLK_ESCAPE);     // options
	STDL_JoystickKeyBind(STDL_JOYSTICK_SELECT, STDLK_BACKSPACE);  // inventory

	// The two you reach for constantly, put where a thumb is not
	// needed: inventory and use, on the shoulders.
	STDL_JoystickKeyBind(STDL_JOYSTICK_TL, STDLK_BACKSPACE);
	STDL_JoystickKeyBind(STDL_JOYSTICK_TR, STDLK_RETURN);

	STDL_JoystickKeyEmulation(1);
}

static int g_ramK = -1;           // ST RAM and alt-RAM, in K, once looked at

int ST_ramK() {
	if (g_ramK < 0) {
		const uint32_t st = *(volatile uint32_t *)0x42EUL;             // phystop
		uint32_t alt = 0;
		if (*(volatile uint32_t *)0x5A8UL == 0x1357BD13UL              // ramvalid
		    && *(volatile uint32_t *)0x5A4UL > 0x01000000UL) {         // ramtop
			alt = *(volatile uint32_t *)0x5A4UL - 0x01000000UL;
		} else {
			// the largest free block, where TOS keeps no record; -32
			// from a GEMDOS without Mxalloc, which has none to give
			const long free = Mxalloc(-1L, 1);
			if (free > 0) {
				alt = (uint32_t)free;
			}
		}
		g_ramK = (int)((st + alt) >> 10);
		info("Memory: %dK ST RAM, %dK alt-RAM", (int)(st >> 10), (int)(alt >> 10));
	}
	return g_ramK;
}

// The RAM the machine has, as found at start-up (0 before then): for
// code that runs in user mode, where low memory is out of reach
int ATARIST_ramK() {
	return g_ramK < 0 ? 0 : g_ramK;
}

// The CPU clock, measured: a fixed loop run for four ticks of the 200Hz
// counter. 32 cycles an iteration on a 68000 with no wait states, so
// 5000 iterations in 20ms is 8MHz; interrupts take a few percent of
// that, which the reading carries. It is there for an accelerator,
// which no cookie names - a "Mega ST at 16MHz" can be an accelerated
// Mega ST or a Mega STE, and only the clock tells them apart.
static int ST_cpuMHz10() {
	volatile uint32_t *hz200 = (volatile uint32_t *)0x4BAUL;
	uint32_t t = *hz200;
	while (*hz200 == t) {
	}
	const uint32_t end = *hz200 + 4;
	uint32_t n = 0;
	__asm__ volatile(
		"1:\taddq.l #1,%0\n\t"
		"cmp.l (%1),%2\n\t"
		"bhi.s 1b"
		: "+d"(n) : "a"(hz200), "d"(end) : "cc");
	return (int)(n * 16 / 1000);
}

// One line for bug reports: the machine, as the library and the clock
// see it. Nothing said before which of several very different machines
// a report came from.
static void ST_logMachine() {
	const STDL_MachineInfo *mi = STDL_GetMachineInfo();
	const uint32_t sysbase = *(volatile uint32_t *)0x4F2UL;
	const unsigned tos = *(volatile uint16_t *)(sysbase + 2);
	char speed[32] = "";
	if (mi->is_megaste) {
		// bit 1 the 16MHz clock, bit 0 the cache (see STDL_UseMegaSteSpeedup)
		const unsigned r = *(volatile uint8_t *)0xFFFF8E21UL;
		snprintf(speed, sizeof(speed), ", %dMHz cache %s", (r & 2) ? 16 : 8, (r & 1) ? "on" : "off");
	}
	const int mhz10 = ST_cpuMHz10();
	// _MCH's high word: 1 the STE family, 2 a TT, 3 a Falcon
	const unsigned family = (unsigned)(mi->mch_cookie >> 16);
	info("Machine: %s, _MCH %08lx, TOS %x.%02x, BLiTTER %s%s, CPU about %d.%dMHz",
		family == 3 ? "Falcon" : family == 2 ? "TT" : mi->is_megaste ? "Mega STE" : (mi->is_ste ? "STE" : "ST"),
		(unsigned long)mi->mch_cookie, tos >> 8, tos & 0xFF,
		mi->has_blitter ? "yes" : "no", speed, mhz10 / 10, mhz10 % 10);
}

// What TOS could still hand the C library in one piece. A probe with
// malloc would count the library's own free list as well, but it is
// not free of side effects: the library keeps the big blocks it takes
// from TOS, splits them later, and the probe alone was enough to stop
// level 1 finding room for its tile pool on a 2MB machine.
void ST_logFreeMemory(const char *where) {
	if (!g_options.logging) {
		return;
	}
	info("Memory at %s: largest free block %ldK", where, ST_freeK());
}

long ST_freeK() {
	return (long)Malloc(-1) >> 10;
}

void SystemStub_Atari::processEvents() {
	// The sound DMA is stopped while nothing plays (see mixer.cpp).
	// Driven from here because it is the one per-frame call the game
	// loop and the cutscene player both make - and cutscenes, where
	// nothing plays for a minute at a time, are most of the win.
	{ extern void ATARIST_mixerTick(); ATARIST_mixerTick(); }
	_pi.dirMask &= ~g_upDirMask;
	g_upDirMask = 0;
	if (g_upEnter) { _pi.enter = false; g_upEnter = false; }
	if (g_upSpace) { _pi.space = false; g_upSpace = false; }
	if (g_upShift) { _pi.shift = false; g_upShift = false; }
	STDL_Event ev;
	while (STDL_PollEvent(&ev)) {
		switch (ev.type) {
		case STDL_QUIT:
			_pi.quit = true;
			break;
		case STDL_KEYDOWN:
		case STDL_KEYUP: {
			const bool down = (ev.type == STDL_KEYDOWN);
			const uint16_t sym = ev.key.keysym.sym;
			const uint16_t mod = ev.key.keysym.mod;
			if (down) {
				_pi.anyKey = true;
			}
			switch (sym) {
			case STDLK_UP:
				if (down && (mod & (STDL_KMOD_LCTRL | STDL_KMOD_RCTRL))) { _pi.panUp = true; break; }
				if (down) { _pi.dirMask |= PlayerInput::DIR_UP; g_upDirMask &= ~PlayerInput::DIR_UP; } else g_upDirMask |= PlayerInput::DIR_UP;
				break;
			case STDLK_DOWN:
				if (down && (mod & (STDL_KMOD_LCTRL | STDL_KMOD_RCTRL))) { _pi.panDown = true; break; }
				if (down) { _pi.dirMask |= PlayerInput::DIR_DOWN; g_upDirMask &= ~PlayerInput::DIR_DOWN; } else g_upDirMask |= PlayerInput::DIR_DOWN;
				break;
			case STDLK_LEFT:
				if (down) { _pi.dirMask |= PlayerInput::DIR_LEFT; g_upDirMask &= ~PlayerInput::DIR_LEFT; } else g_upDirMask |= PlayerInput::DIR_LEFT;
				break;
			case STDLK_RIGHT:
				if (down) { _pi.dirMask |= PlayerInput::DIR_RIGHT; g_upDirMask &= ~PlayerInput::DIR_RIGHT; } else g_upDirMask |= PlayerInput::DIR_RIGHT;
				break;
			case STDLK_RETURN:
			case STDLK_KP_ENTER:
				if (down) { _pi.enter = true; g_upEnter = false; } else g_upEnter = true;
				break;
			case STDLK_SPACE:
				if (down) { _pi.space = true; g_upSpace = false; } else g_upSpace = true;
				break;
			case STDLK_LSHIFT:
			case STDLK_RSHIFT:
				if (down) { _pi.shift = true; g_upShift = false; } else g_upShift = true;
				break;
			// backspace and escape are edge-consumed by the game
			// (it writes false back). Frames can be slow enough
			// that press and release arrive in the same poll, so
			// never clear them here or the game misses the press.
			case STDLK_BACKSPACE:
			case STDLK_TAB:
				if (down) {
					_pi.backspace = true;
				}
				break;
			case STDLK_ESCAPE:
				if (down) {
					_pi.escape = true;
				}
				break;
			default:
				if (down) {
					if (mod & (STDL_KMOD_LCTRL | STDL_KMOD_RCTRL)) {
						switch (sym) {
						case STDLK_s:
							_pi.save = true;
							break;
						case STDLK_l:
							_pi.load = true;
							break;
						case STDLK_q:
							_pi.quit = true;
							break;
						case STDLK_p:
							if (mod & STDL_KMOD_SHIFT) {
								_pi.reloadPalette = true;
							} else {
								_pi.dumpPalette = true;
							}
							break;
						case STDLK_KP_PLUS:
						case STDLK_EQUALS:
							_pi.stateSlot = 1;
							break;
						case STDLK_KP_MINUS:
						case STDLK_MINUS:
							_pi.stateSlot = -1;
							break;
						}
					} else if (sym >= STDLK_a && sym <= STDLK_z) {
						_pi.lastChar = sym - STDLK_a + 'A';
					} else if (sym >= STDLK_0 && sym <= STDLK_9) {
						_pi.lastChar = sym - STDLK_0 + '0';
					}
				}
				break;
			}
			break;
		}
		default:
			break;
		}
	}
}

void SystemStub_Atari::sleep(int duration) {
	STDL_Delay(duration);
}

uint32_t SystemStub_Atari::getTimeStamp() {
	return STDL_GetTicks();
}

#ifndef ST_PLANAR
// The counters the ST build's log lines carry (video_st.cpp keeps them
// there). Of its sprite path the 030 build has none, so what it counts
// is the rooms decoded (video.cpp) and the banks and monsters loaded
// (resource.cpp, game.cpp).
STPerf g_stPerf;

void ST_perfFormat(const STPerf &p, char *buf, int len) {
	snprintf(buf, len, "room %u mon %u bank %u/%u/%luK",
		(unsigned)p.roomDecodes, (unsigned)p.monsterLoads,
		(unsigned)p.bankLoads, (unsigned)p.bankClears, (unsigned long)(p.bankBytes >> 10));
}
#endif
