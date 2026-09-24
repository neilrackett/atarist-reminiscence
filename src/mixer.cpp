
/*
 * REminiscence - Flashback interpreter
 * Copyright (C) 2005-2019 Gregory Montoir (cyx@users.sourceforge.net)
 * Atari ST port changes Copyright (C) 2026 Neil Rackett
 */

#include "mixer.h"
#include "systemstub.h"
#include "util.h"

#ifdef ATARIST
extern "C" {
#include <stdl/stdl.h>
}

// Sound effects on the STE: voice 3 of the STDL_Voice mixer, so
// they coexist with the SfxPlayer music on voices 0-2. If the voice
// device is not open (it failed, or a plain ST) fall back to a raw
// one-shot DMA sample - resampled and volume-scaled into a scratch
// buffer, since the bare DMA does neither.
static uint8_t *_dmaBuf;
static uint32_t _dmaBufSize;

// The one-shot fallback needs DMA sound, and a plain ST has none:
// the conversion below costs an 8MHz machine about 75ms - more than
// a whole frame - and the play call then fails, so nothing is heard
// and the game stutters for it. Ask the hardware once, with two
// bytes of silence, and stay quiet from then on.
static bool ATARIST_dmaSample() {
	static int available = -1;
	if (available < 0) {
		static const int8_t silence[2] = { 0, 0 };
		available = (STDL_PlaySample(silence, sizeof(silence), 6258) == 0);
		STDL_StopSample();
	}
	return available != 0;
}

// Can this machine play the sampled effects at all? A plain ST has
// neither the voice device nor DMA sound, and ste_sound=false turns
// both off; either way a level's effects are never heard, and they
// are the biggest thing a level loads.
bool ATARIST_samplesPlayable() {
	return g_options.ste_sound && (STDL_VoicesOpen() || ATARIST_dmaSample());
}

// volume scaling as a table: the 68000 has no 32-bit multiply, so
// scaling each of a sample's thousands of bytes cost a __mulsi3 call
static const uint8_t *ATARIST_volumeTable(uint8_t volume) {
	static uint8_t table[256];
	static int builtFor = -1;
	if (builtFor != (int)volume) {
		for (int i = 0; i < 256; ++i) {
			const int16_t s = (int16_t)(int8_t)i;
			table[i] = (uint8_t)((s * (int16_t)volume) >> 6);
		}
		builtFor = volume;
	}
	return table;
}

/*
 * Stop the sound DMA while nothing is playing.
 *
 * An open DMA clicks about once every 30 seconds on real hardware
 * even looping pure silence with no software running - a property of
 * the machine, documented in stdl_voice.h, and not something any
 * mixing fix reaches. Time spent paused is time without it, and
 * pausing itself was measured on hardware as inaudible.
 *
 * STDL_PauseVoices only asks: the library stops the DMA once the ring
 * has drained to silence, so asking early cannot cut a sound short or
 * park the DAC away from zero. That makes the delay below an
 * optimisation rather than a correctness requirement.
 *
 * Two seconds, and the number matters. Polling the voices every frame
 * across a whole run - intro, both intro cutscenes, title screen, the
 * level's opening cutscene, then walking gameplay - the longest gap
 * between effects during play was 1.16s, with 44 gaps measured and a
 * median of 585ms. So two seconds never fires while the game is
 * running, and fires exactly once per cutscene, title screen or menu,
 * where the device then stays stopped for tens of seconds: the intro
 * alone is one unbroken 61.8s stretch. A shorter delay would ask the
 * library to pause and cancel dozens of times a minute during play
 * for nothing.
 */
enum { kVoiceIdlePauseMs = 2000 };

void ATARIST_mixerTick() {
	if (!STDL_VoicesOpen()) {
		return;
	}
	static uint32_t idleSince;
	static bool asked;
	for (int v = 0; v < 4; ++v) {
		if (STDL_VoiceActive(v)) {
			idleSince = 0;
			asked = false;
			return;
		}
	}
	const uint32_t now = STDL_GetTicks();
	if (idleSince == 0) {
		idleSince = now ? now : 1;   // 0 is the "not idle" marker
	} else if (!asked && now - idleSince >= kVoiceIdlePauseMs) {
		STDL_PauseVoices();
		asked = true;
	}
}

static void ATARIST_playSample(const uint8_t *data, uint32_t len, uint16_t freq, uint8_t volume) {
	// ste_sound=false means no sampled sound at all. Without this the
	// option only skips the voice device, and every effect then falls
	// through to the one-shot path below - which still makes sound,
	// and pays that path's resample per effect (a comment above
	// measures it at ~75ms on an 8MHz machine, more than a frame).
	// So the option would have been quieter, slower and still not
	// silent, which is none of the three things it says.
	if (!g_options.ste_sound) {
		return;
	}
	if (STDL_VoicesOpen()) {
		// Unconditionally, never behind a STDL_VoicesPaused() test:
		// while a pause is draining it is the cancel that matters,
		// and a guarded call leaves the device stopped later with no
		// error and no sound.
		STDL_ResumeVoices();
		STDL_SetVoice(3, (const int8_t *)data, len, 0, 0, freq,
		              (volume > 64) ? 64 : volume);
		return;
	}
	if (!ATARIST_dmaSample()) {
		return;
	}
	const int rate = (freq > 9000) ? 12517 : 6258;
	// 32-bit arithmetic: the effects are a few KB each, far short of
	// the 340KB where len * rate would overflow
	const uint32_t outLen = ((len * (uint32_t)rate) / freq + 1) & ~1;
	if (outLen > _dmaBufSize) {
		::free(_dmaBuf);
		_dmaBuf = (uint8_t *)malloc(outLen);
		_dmaBufSize = _dmaBuf ? outLen : 0;
	}
	if (!_dmaBuf) {
		return;
	}
	uint32_t pos = 0; // 16.16 through the source
	const uint32_t inc = ((uint32_t)freq << 16) / rate;
	const uint8_t *vol = ATARIST_volumeTable(volume);
	for (uint32_t i = 0; i < outLen; ++i) {
		_dmaBuf[i] = vol[data[pos >> 16]];
		pos += inc;
		if ((pos >> 16) >= len) {
			pos = ((uint32_t)len - 1) << 16;
		}
	}
	STDL_PlaySample(_dmaBuf, outLen, rate);
}
#endif

#ifdef ATARIST
/*
 * YM chip music. The Amiga score is sampled and this port has no
 * software mixer, so the modules are converted offline into YM2149
 * register streams (tools/make-music.sh) and replayed by STDL_Music
 * off the 50Hz sound tick. That costs almost nothing and works on a
 * plain ST, where the sampled effects cannot play at all.
 *
 * Everything here fails quiet: the streams are not shipped with the
 * game (they are derived from someone else's copyrighted score), so
 * a player who has not built them simply gets no music, exactly as
 * before. Each track is reported missing once, to RS.LOG, rather
 * than every time the engine asks for it.
 */
static STDL_Music *_ymMusic;
static int _ymTrack = -1;
static uint32_t _ymMissing;          /* one bit per track, log once */

// MUSIC\ is GEMDOS 8.3: strip the underscore, uppercase, and where a
// name is too long keep its last character rather than truncating -
// teleporta and teleport2 differ only there, and would otherwise
// both become TELEPORT. tools/make-music.sh names its output with
// this same rule.
static void ATARIST_musicName(const char *src, char *out) {
	char tmp[16];
	int n = 0;
	for (const char *p = src; *p && n < 15; ++p) {
		if (*p != '_') {
			const char c = *p;
			tmp[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
		}
	}
	tmp[n] = 0;
	if (n > 8) {
		memcpy(out, tmp, 7);
		out[7] = tmp[n - 1];
		n = 8;
	} else {
		memcpy(out, tmp, n);
	}
	out[n] = 0;
}

static bool ATARIST_playMusic(int num) {
	if (!g_options.music || num < 0 || num * 2 >= ModPlayer::_namesCount) {
		return false;
	}
	if (_ymMusic != 0 && num == _ymTrack && STDL_PlayingMusic()) {
		return true;                 // already playing this track
	}
	// the alternate name is the one the module sets carry
	const char *want = ModPlayer::_names[num * 2 + 1];
	if (want == 0) {
		want = ModPlayer::_names[num * 2];
	}
	char stem[16];
	ATARIST_musicName(want, stem);
	// MUSIC\, not DATA\: these streams are derived, optional and
	// unshippable, where DATA\ holds the game's own files. Different
	// provenance, different lifetime, so a data folder can be replaced
	// or compared against a fresh extraction without the music in the
	// way.
	char path[32];
	snprintf(path, sizeof(path), "MUSIC\\%s.STM", stem);

	STDL_Music *m = STDL_LoadMusic(path);
	if (m == 0) {
		if (num < 32 && !(_ymMissing & (1u << num))) {
			_ymMissing |= 1u << num;
			warning("No music for track %d (%s) - see tools/make-music.sh", num, path);
		}
		return false;
	}
	if (_ymMusic != 0) {
		STDL_HaltMusic();
		STDL_FreeMusic(_ymMusic);
	}
	_ymMusic = m;
	_ymTrack = num;
	// STDL scales the YM volume registers on the fly, 0..128 for
	// mute..full; the option is a percentage. Set here rather than
	// once at init so the value is read when the music does play,
	// whichever order the devices came up in.
	STDL_VolumeMusic((g_options.music_volume * 128) / 100);
	STDL_PlayMusic(_ymMusic, -1);    // loop until the scene ends
	return true;
}

static void ATARIST_stopMusic() {
	if (_ymMusic != 0) {
		STDL_HaltMusic();
		STDL_FreeMusic(_ymMusic);
		_ymMusic = 0;
		_ymTrack = -1;
	}
}

#endif

Mixer::Mixer(FileSystem *fs, SystemStub *stub, const PrfMidiDriver *midiDriver, const char *midiSoundFont)
	: _stub(stub), _musicType(MT_NONE), _cpc(this, fs), _mod(this, fs), _ogg(this, fs), _prf(this, fs, midiDriver, midiSoundFont), _sfx(this) {
	_musicTrack = -1;
	_backgroundMusicType = MT_NONE;
}

void Mixer::init() {
	memset(_channels, 0, sizeof(_channels));
	_premixHook = 0;
#ifdef ATARIST
	// the voice mixer carries both music (0-2) and effects (3);
	// fails cleanly on a plain ST and we fall back to one-shots
	if (g_options.ste_sound) {
		STDL_OpenVoices(6258);
	} else {
		info("STE sample sound disabled (ste_sound=false)");
	}
#endif
	_stub->startAudio(Mixer::mixCallback, this);
}

void Mixer::free() {
	setPremixHook(0, 0);
	stopAll();
#ifdef ATARIST
	STDL_CloseVoices();
#endif
	_stub->stopAudio();
}

void Mixer::setPremixHook(PremixHook premixHook, void *userData) {
	debug(DBG_SND, "Mixer::setPremixHook()");
	LockAudioStack las(_stub);
	_premixHook = premixHook;
	_premixHookData = userData;
}

void Mixer::play(const uint8_t *data, uint32_t len, uint16_t freq, uint8_t volume) {
	debug(DBG_SND, "Mixer::play(%d, %d)", freq, volume);
#ifdef ATARIST
	ATARIST_playSample(data, len, freq, volume);
	return;
#endif
	LockAudioStack las(_stub);
	for (int i = 0; i < NUM_CHANNELS; ++i) {
		MixerChannel *ch = &_channels[i];
		if (ch->active && ch->soundData == data) { // repeat sound
			ch->soundPos = 0;
			ch->volume = volume;
			return;
		}
	}
	for (int i = 0; i < NUM_CHANNELS; ++i) {
		MixerChannel *ch = &_channels[i];
		if (!ch->active) { // start sound
			ch->active = true;
			ch->volume = volume;
			ch->soundData = data;
			ch->soundSize = len;
			ch->soundPos = 0;
			ch->soundInc = (freq << FRAC_BITS) / _stub->getOutputSampleRate();
			return;
		}
	}
}

bool Mixer::isPlaying(const uint8_t *data) const {
	debug(DBG_SND, "Mixer::isPlaying");
	LockAudioStack las(_stub);
	for (int i = 0; i < NUM_CHANNELS; ++i) {
		const MixerChannel *ch = &_channels[i];
		if (ch->active && ch->soundData == data) {
			return true;
		}
	}
	return false;
}

uint32_t Mixer::getSampleRate() const {
	return _stub->getOutputSampleRate();
}

void Mixer::stopAll() {
	debug(DBG_SND, "Mixer::stopAll()");
#ifdef ATARIST
	if (STDL_VoicesOpen()) {
		STDL_StopVoice(3);
	} else {
		STDL_StopSample();
	}
#endif
	LockAudioStack las(_stub);
	memset(_channels, 0, sizeof(_channels));
}

static bool isMusicSfx(int num) {
	return (num >= 68 && num <= 75);
}

void Mixer::playMusic(int num, int tempo) {
	debug(DBG_SND, "Mixer::playMusic(%d, %d)", num, tempo);
	// digital soundtracks (.ogg, CD-i .cpc): the ST has no software
	// mixer to play them, and looking for them at every track change
	// is file probing for nothing
#ifndef ATARIST
	int trackNum = -1;
	if (num == 1) { // menu screen
		trackNum = 2;
	} else if (num > MUSIC_TRACK) {
		trackNum = num - MUSIC_TRACK;
	}
	if (trackNum != -1 && trackNum != _musicTrack) {
		if (_ogg.playTrack(trackNum)) {
			_backgroundMusicType = _musicType = MT_OGG;
			_musicTrack = trackNum;
			return;
		}
		if (_cpc.playTrack(trackNum)) {
			_backgroundMusicType = _musicType = MT_CPC;
			_musicTrack = trackNum;
			return;
		}
	}
#endif
	if ((_musicType == MT_OGG || _musicType == MT_CPC) && isMusicSfx(num)) { // do not play level action music with background music
		return;
	}
	if (isMusicSfx(num)) { // level action sequence
		_sfx.play(num);
		if (_sfx._playing) {
			_musicType = MT_SFX;
		}
	} else { // cutscene
#ifdef ATARIST
		// YM chip music stands in for the .mod score, which needs a
		// software mixer this port does not have
		if (ATARIST_playMusic(num)) {
			_musicType = MT_MOD;
		}
		// No fallback to the players below: none of them can make a
		// sound here, and a .mod left in DATA\ would be loaded - a
		// hundred K or more - and never heard.
		return;
#endif
		_mod.play(num, tempo);
		if (_mod._playing) {
			_musicType = MT_MOD;
			return;
		}
		if (g_options.use_prf_music) {
			_prf.play(num);
			if (_prf._playing) {
				_musicType = MT_PRF;
				return;
			}
		}
	}
}

void Mixer::stopMusic() {
	debug(DBG_SND, "Mixer::stopMusic()");
#ifdef ATARIST
	ATARIST_stopMusic();
#endif
	switch (_musicType) {
	case MT_NONE:
		break;
	case MT_MOD:
		_mod.stop();
		break;
	case MT_OGG:
		_ogg.pauseTrack();
		break;
	case MT_PRF:
		_prf.stop();
		break;
	case MT_SFX:
		_sfx.stop();
		break;
	case MT_CPC:
		_cpc.pauseTrack();
		break;
	}
	_musicType = MT_NONE;
	if (_musicTrack > 2) { // do not resume menu music
		switch (_backgroundMusicType) {
		case MT_OGG:
			_ogg.resumeTrack();
			_musicType = MT_OGG;
			break;
		case MT_CPC:
			_cpc.resumeTrack();
			_musicType = MT_CPC;
			break;
		default:
			break;
		}
	} else {
		_musicTrack = -1;
	}
}

void Mixer::mix(int16_t *out, int len) {
	if (_premixHook) {
		if (!_premixHook(_premixHookData, out, len)) {
			_premixHook = 0;
			_premixHookData = 0;
		}
	}
	for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
		MixerChannel *ch = &_channels[i];
		if (ch->active) {
			for (int pos = 0; pos < len; ++pos) {
				const uint32_t sPos = ch->soundPos >> FRAC_BITS;
				if (sPos >= ch->soundSize) {
					ch->active = false;
					break;
				}
				const int8_t s8 = ch->soundData[sPos];
				const int sample = S8_to_S16(s8) * ch->volume / Mixer::MAX_VOLUME;
				out[2 * pos]     = ADDC_S16(out[2 * pos],     sample);
				out[2 * pos + 1] = ADDC_S16(out[2 * pos + 1], sample);
				ch->soundPos += ch->soundInc;
			}
		}
	}
}

void Mixer::mixCallback(void *param, int16_t *buf, int len) {
	((Mixer *)param)->mix(buf, len);
}
