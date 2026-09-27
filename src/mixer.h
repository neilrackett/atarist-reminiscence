
/*
 * REminiscence - Flashback interpreter
 * Copyright (C) 2005-2019 Gregory Montoir (cyx@users.sourceforge.net)
 * Atari ST port changes Copyright (C) 2026 Neil Rackett
 */

#ifndef MIXER_H__
#define MIXER_H__

#include "intern.h"
#include "cpc_player.h"
#include "mod_player.h"
#include "ogg_player.h"
#include "prf_player.h"
#include "sfx_player.h"

struct MixerChannel {
	bool active;
	uint8_t volume;
	const uint8_t *soundData;
	uint32_t soundSize;
	uint32_t soundPos;
	uint32_t soundInc;
};

struct FileSystem;
struct PrfMidiDriver;
struct Resource;
struct SystemStub;

struct Mixer {
	typedef bool (*PremixHook)(void *userData, int16_t *buf, int len);

	enum MusicType {
		MT_NONE,
		MT_MOD,
		MT_OGG,
		MT_PRF,
		MT_SFX,
		MT_CPC,
	};

	enum {
		MUSIC_TRACK = 1000,
		NUM_CHANNELS = 4,
		FRAC_BITS = 12,
		MAX_VOLUME = 64
	};

	FileSystem *_fs;
	SystemStub *_stub;
	MixerChannel _channels[NUM_CHANNELS];
	PremixHook _premixHook;
	void *_premixHookData;
	MusicType _backgroundMusicType;
	MusicType _musicType;
	CpcPlayer _cpc;
	ModPlayer _mod;
	OggPlayer _ogg;
	PrfPlayer _prf;
	SfxPlayer _sfx;
	int _musicTrack;

	Mixer(FileSystem *fs, SystemStub *stub, const PrfMidiDriver *midiDriver, const char *midiSoundFont);
	void init();
	void free();
	void setPremixHook(PremixHook premixHook, void *userData);
	void play(const uint8_t *data, uint32_t len, uint16_t freq, uint8_t volume);
	bool isPlaying(const uint8_t *data) const;
	uint32_t getSampleRate() const;
	void stopAll();
	void playMusic(int num, int tempo = 0);
	void stopMusic();
	void mix(int16_t *buf, int len);
#ifdef ATARIST
	// for the title's Options: set the music level (percent) and hear
	// it at once, and whether there is any music to play
	void ST_setMusicVolume(int percent);
	static bool ST_musicInstalled(bool mod);
	// what the music setting plays here: the modules only where they
	// are installed and the STE can play them, the YM streams only
	// where they are installed
	enum { kMusicOff, kMusicYm, kMusicMod };
	static int ST_musicKind();
#endif

	static void mixCallback(void *param, int16_t *buf, int len);
};

#ifdef ATARIST
// track num's name in MUSIC\, GEMDOS 8.3 without the extension;
// false for a number that is no track (mixer.cpp)
bool ST_musicStem(int num, char *out);
// start STDL voice v on a Paula-style sample from `off` bytes in,
// looping [loopPos, loopPos + loopLen) when that is longer than one
// word; false when `off` is past the first pass (mixer.cpp)
bool ST_playPaulaVoice(int v, const int8_t *data, uint32_t len, uint32_t loopPos, uint32_t loopLen,
                       uint32_t off, uint32_t freq, int vol);
// a sound effect on voice 3: 0 none, 1 playing, 2 being set up - a
// module's fourth channel keeps off the voice in both (mixer.cpp)
extern volatile uint8_t g_stSfxVoice3;
// STDL_SetVoiceTick for the music players, which keeps the device
// from pausing under a sequencer (mixer.cpp)
void ST_setVoiceTick(void (*fn)(void *), void *ud);
// the YM's version of sound effect `num`, for a machine that cannot
// play the samples; softVol as Game::playSound's (ym_sfx_st.cpp)
enum { ST_YMSFX_SAVED = 0xF0 };      // the save-point jingle
void ST_prepareYmSfx();              // build them now, at a level's load
void ST_playYmSfx(int num, int softVol);
void ST_auditionYmSfx(Resource *res, Mixer *mix, SystemStub *stub);
#endif

#endif // MIXER_H__
