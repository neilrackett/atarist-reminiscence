/*
 * Sound effects on the YM2149, for an ST that cannot play samples.
 * Copyright (C) 2026 Neil Rackett
 *
 * The game's effects are Amiga samples, and an ST without DMA sound
 * (a plain ST or Mega ST, or an STE with ste_sound=false) has no way
 * to play them: the level's sample bank is not even loaded there. It
 * does have the YM, so the effects worth hearing are approximated on
 * it instead - a noise crack over a tone thump for a gunshot, a noise
 * sinking into a rumble for an explosion - with STDL_PlaySfx.
 *
 * Each recipe is a handful of numbers, written by hand from what
 * tools/ym-sfx-report.py measures of the sample (its length, how its
 * level falls, how noisy and how bright it is), and expanded into
 * STDL_Sfx step arrays the first time an effect plays. Ambient loops
 * (jungle, birds, drips, fans) have none and stay silent: as square
 * waves they would grate. Sounds the Amiga banks do not carry have
 * none either, so nothing plays here that the Amiga would not.
 *
 * All effects share voice C - the voice a YM stream's drums are on,
 * and the one its melody misses least - so music keeps two voices
 * throughout. A playing effect is not cut short by a less important
 * one; an equal or greater one replaces it, so a burst of gunfire
 * retriggers.
 */

#ifdef ATARIST

#include <stdlib.h>
#include <string.h>
#include "mixer.h"
#include "resource.h"
#include "systemstub.h"
#include "util.h"
extern "C" {
#include <stdl/stdl.h>
}

namespace {

// how much an effect matters: a playing one is not cut short by a
// lower one
enum {
	kPrioStep,       // footsteps, the lift's hum
	kPrioWorld,      // doors, landing, machinery
	kPrioFeedback,   // beeps, the shield, teleporters, blows
	kPrioGun,        // gunfire
	kPrioHit,        // being hit, dying
	kPrioBoom        // explosions
};

enum {
	kAlt = 1,        // tone alternates t0 / t1 each step (t1 0: a gap)
	kPulse = 2       // every other step 5 quieter: a rattle
};

/*
 * steps of ms each. Volume (0-15, 3dB a step) runs v0 to vp over the
 * first `peak` steps, then vp to v1 by the last; with no attack, v0 is
 * vp and peak 0. Noise period (1-31, low is bright) slides n0 to n1
 * over the first noiseSteps steps (0: all of them), none when n0 is 0;
 * tone period (YM clock / 16 / Hz) the same with t0, t1, toneSteps.
 *
 * The levels are set so each effect is about as loud as its sample
 * on an STE - measured by playing the two in turn at a level's start
 * - keeping the balance an STE has between its effects and YM music.
 * Written first at full scale they came out 8dB over on average, 14
 * for the explosion.
 */
struct Recipe {
	uint8_t id;          // the game's sound number (Resource::_splNames)
	uint8_t prio;
	uint8_t steps, ms;
	uint8_t v0, vp, peak, v1;
	uint8_t n0, n1, noiseSteps;
	uint16_t t0, t1;
	uint8_t toneSteps, flags;
};

static const Recipe kRecipes[] = {
	//  id  prio           st  ms   v0 vp  pk  v1   n0  n1 nS     t0    t1  tS  flags
	// gunfire: a gunshot, the laser, the machine gun
	{   4, kPrioGun,      14, 20,  12, 12,  0,  0,  10, 26, 0,  1250, 1600,  3, 0 },
	{   3, kPrioGun,      12, 25,  10, 10,  0,  3,   0,  0, 0,    55,  150,  0, 0 },
	{  21, kPrioGun,      22, 20,  13, 13,  0,  6,   8, 14, 0,     0,    0,  0, kPulse },
	// Conrad's pistol, and explosions: the game plays "explo" for both.
	// Loud, then sinking into a rumble
	{   5, kPrioBoom,     36, 30,  10, 10,  8,  0,   8, 31, 0,  2000, 3600, 12, 0 },
	// hits, deaths, the shield taking a shot
	{  22, kPrioHit,      12, 30,  12, 12,  0,  1,  14, 24, 0,   900, 1400,  4, 0 },
	{  36, kPrioHit,      14, 40,  10, 10,  0,  0,  10, 16, 0,   356,   70,  0, 0 },
	{  16, kPrioHit,      16, 40,  11, 11,  0,  0,  12, 16, 0,   121,  190,  0, kAlt },
	{   7, kPrioHit,      22, 40,   9, 11,  4,  0,   0,  0, 0,   217,  520,  0, 0 },
	{  19, kPrioHit,      24, 45,   2,  9, 10,  0,   0,  0, 0,    67,  900,  0, 0 },
	// the shield, a mine, beeps, teleporters, glass, alarm
	{   8, kPrioFeedback, 20, 45,  11, 11,  0,  1,  16, 18, 0,   300,  780,  0, 0 },
	{  63, kPrioFeedback, 12, 40,   8,  8,  0,  6,   0,  0, 0,    62,    0,  0, kAlt },
	{   2, kPrioFeedback,  6, 40,   6,  6,  0,  5,   0,  0, 0,    89,   50,  0, 0 },
	{  18, kPrioFeedback, 28, 50,   9,  9,  0,  0,  20, 20, 4,  1000,   50,  0, 0 },
	{  12, kPrioFeedback, 18, 45,   9,  9,  0,  0,   1,  4, 0,    49,   58,  0, kAlt },
	{  55, kPrioFeedback, 18, 40,  10, 10,  0,  9,   0,  0, 0,   800,  400,  0, 0 },
	{  30, kPrioFeedback, 20, 45,   2,  9, 18,  8,   0,  0, 0,   500,   90,  0, 0 },
	{  31, kPrioFeedback, 30, 50,  10, 10,  0,  2,   0,  0, 0,  1500,  650,  0, 0 },
	// blows
	{  23, kPrioFeedback,  5, 40,  10, 10,  0,  2,  14, 22, 0,   700,  900,  2, 0 },
	{  65, kPrioFeedback,  4, 40,   9,  9,  0,  0,   6, 12, 0,     0,    0,  0, 0 },
	// doors, trapdoor, lift, landing, machinery
	{   0, kPrioWorld,    16, 50,   9, 11,  2,  1,   3,  6, 0,     0,    0,  0, 0 },
	{  15, kPrioWorld,     8, 40,   9,  9,  0,  0,  26, 26, 2,  1000, 1300,  0, 0 },
	{  27, kPrioWorld,     8, 40,   9,  9,  0,  0,  28, 28, 2,  1300, 1600,  0, 0 },
	{   9, kPrioWorld,     8, 40,   8,  8,  0,  5,  10, 10, 3,   800,  150,  0, 0 },
	{  11, kPrioWorld,    10, 40,   9,  9,  0,  0,  12, 12, 2,   150,  800,  0, 0 },
	{  26, kPrioWorld,     5, 40,   7,  7,  0,  0,  22, 30, 0,   900,  900,  2, 0 },
	{  20, kPrioWorld,    10, 40,   6, 10,  4,  1,   8,  8, 0,   400,  100,  0, 0 },
	{  32, kPrioWorld,    14, 45,   5,  8,  8,  3,   8, 12, 0,   550,  230,  0, 0 },
	// quiet ones: the lift moving, footsteps
	{  10, kPrioStep,      3, 40,   4,  4,  0,  4,   5,  5, 0,   150,  150,  0, 0 },
	{  44, kPrioStep,      2, 20,   3,  3,  0,  1,  26, 26, 0,   350,  350,  1, 0 },
	{  45, kPrioStep,      2, 20,   3,  3,  0,  1,  26, 26, 0,   400,  400,  1, 0 },
	// the save-point jingle (play_gamesaved_sound)
	{ ST_YMSFX_SAVED, kPrioFeedback,  6, 60,   9,  9,  0,  7,   0,  0, 0,   140,  105,  0, kAlt },
};

enum {
	kNumRecipes = sizeof(kRecipes) / sizeof(kRecipes[0]),
	kDistances = 4,          // Game::playSound's softVol, 0 (near) to 3
	kVoice = 2               // C
};

// Each recipe at each distance: the game halves a distant sample's
// level twice a step of softVol (12dB), four of the YM's 3dB volume
// steps. Built with the rest rather than copied per play, so nothing
// STDL is still reading is ever rewritten.
static STDL_Sfx _fx[kNumRecipes][kDistances];
static uint8_t _byId[256];       // recipe index + 1, 0 = none
static bool _built;
static uint8_t _playingPrio;

// step i of n from a to b (i < n). 32-bit division, but only while
// the recipes are built.
static int slide(int a, int b, int i, int n) {
	return (n > 1) ? a + (b - a) * i / (n - 1) : a;
}

// The step arrays are allocated here rather than kept in bss: only a
// machine that cannot play the samples ever builds them, so an STE
// does not carry them. Built at a level's load where that is known
// (ST_prepareYmSfx), since the 900-odd 32-bit divides take about a
// frame of an 8MHz machine; the first effect builds them otherwise.
static void build() {
	_built = true;
	int total = 0;
	for (int r = 0; r < kNumRecipes; ++r) {
		total += kRecipes[r].steps;
	}
	uint8_t *pool = (uint8_t *)malloc(total * (sizeof(uint16_t) + 1 + kDistances));
	if (!pool) {
		return;                  // no memory: no effects, as before
	}
	uint16_t *periods = (uint16_t *)pool;
	uint8_t *noises = pool + total * sizeof(uint16_t);
	uint8_t *volumes = noises + total;
	for (int r = 0; r < kNumRecipes; ++r) {
		const Recipe &rc = kRecipes[r];
		const int tn = rc.toneSteps ? rc.toneSteps : rc.steps;
		const int nn = rc.noiseSteps ? rc.noiseSteps : rc.steps;
		for (int i = 0; i < rc.steps; ++i) {
			int t = 0;
			if (rc.t0 != 0 && i < tn) {
				t = (rc.flags & kAlt) ? ((i & 1) ? rc.t1 : rc.t0) : slide(rc.t0, rc.t1, i, tn);
			}
			periods[i] = (uint16_t)t;
			noises[i] = (rc.n0 != 0 && i < nn) ? (uint8_t)slide(rc.n0, rc.n1, i, nn) : 0;
			int v = (i <= rc.peak) ? slide(rc.v0, rc.vp, i, rc.peak + 1)
			                       : slide(rc.vp, rc.v1, i - rc.peak, rc.steps - rc.peak);
			if ((rc.flags & kPulse) && (i & 1)) {
				v -= 5;
			}
			for (int d = 0; d < kDistances; ++d) {
				volumes[d * rc.steps + i] = (uint8_t)CLIP(v - 4 * d, 0, 15);
			}
		}
		for (int d = 0; d < kDistances; ++d) {
			STDL_Sfx &fx = _fx[r][d];
			fx.periods = periods;
			fx.volumes = volumes + d * rc.steps;
			fx.nsteps = rc.steps;
			fx.volume = 0;
			fx.step_ms = rc.ms;
			fx.noise = 0;
			fx.noises = noises;
		}
		_byId[rc.id] = (uint8_t)(r + 1);
		periods += rc.steps;
		noises += rc.steps;
		volumes += kDistances * rc.steps;
	}
}

// the recipe for sound `num`, or 0 for none
static int recipeIndex(int num) {
	if (num < 0 || num > 255) {
		return -1;
	}
	if (!_built) {
		build();
	}
	return _byId[num] - 1;
}

} // namespace

void ST_prepareYmSfx() {
	if (!_built) {
		build();
	}
}

void ST_playYmSfx(int num, int softVol) {
	const int r = recipeIndex(num);
	if (r < 0 || softVol < 0 || softVol >= kDistances) {
		return;                  // no recipe, or too far off to hear
	}
	// A distant sound gives way to a near one. The first room of the
	// game retriggers a distant beep about twenty times a second, and
	// at its own priority it would have held off footsteps and doors
	// the whole time it played.
	const int prio = MAX(kRecipes[r].prio - 2 * softVol, 0);
	if (STDL_SfxActive(kVoice) && prio < _playingPrio) {
		return;                  // something that matters more is playing
	}
	if (STDL_PlaySfx(&_fx[r][softVol], kVoice) >= 0) {
		_playingPrio = (uint8_t)prio;
	}
}

#endif
