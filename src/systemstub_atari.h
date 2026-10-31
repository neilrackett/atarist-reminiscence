
/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * The platform layer both Atari builds share, on STDL: the keyboard,
 * joystick and pads, the clock, and the machine set-up. The ST build's
 * stub adds its planar picture (systemstub_stdl.cpp), the 030 build's
 * its 256-colour one (systemstub_030.cpp).
 */

#ifndef SYSTEMSTUB_ATARI_H__
#define SYSTEMSTUB_ATARI_H__

extern "C" {
#include <stdl/stdl.h>
}
#include "systemstub.h"

// No data members: the ST stub's fields keep the offsets they had
// before this was split out of it.
struct SystemStub_Atari : SystemStub {
	// What each build's init does around its screen: STDL and the
	// machine before it, the RAM and the machine in the log, and the
	// joystick and pad bindings.
	void initMachine();
	void logMachine();
	void initJoystick();

	virtual bool hasWidescreen() const { return false; }
	virtual void rumble(int low, int high, int ms) { STDL_PadRumble(low, high, ms); }
	virtual void setOverscanColor(int i) {}
	virtual void copyRectRgb24(int x, int y, int w, int h, const uint8_t *rgb) {}
	virtual void zoomRect(int x, int y, int w, int h) {}
	virtual void copyWidescreenLeft(int w, int h, const uint8_t *buf) {}
	virtual void copyWidescreenRight(int w, int h, const uint8_t *buf) {}
	virtual void copyWidescreenMirror(int w, int h, const uint8_t *buf) {}
	virtual void copyWidescreenBlur(int w, int h, const uint8_t *buf) {}
	virtual void copyWidescreenCDi(int w, int h, const uint8_t *buf, const uint8_t *pal) {}
	virtual void clearWidescreen() {}
	virtual void enableWidescreen(bool enable) {}
	virtual void processEvents();
	virtual void sleep(int duration);
	virtual uint32_t getTimeStamp();
	virtual void startAudio(AudioCallback callback, void *param) {}
	virtual void stopAudio() {}
	virtual uint32_t getOutputSampleRate() { return 12517; }
	virtual void lockAudio() {}
	virtual void unlockAudio() {}
};

// the RAM the machine has, in K, looked at once (supervisor mode)
int ST_ramK();

#endif // SYSTEMSTUB_ATARI_H__
