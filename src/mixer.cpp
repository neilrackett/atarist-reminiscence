
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

static void ATARIST_playSample(const uint8_t *data, uint32_t len, uint16_t freq, uint8_t volume) {
	if (STDL_VoicesOpen()) {
		STDL_SetVoice(3, (const int8_t *)data, len, 0, 0, freq,
		              (volume > 64) ? 64 : volume);
		return;
	}
	const int rate = (freq > 9000) ? 12517 : 6258;
	const uint32_t outLen = ((uint32_t)((uint64_t)len * rate / freq) + 1) & ~1;
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
	for (uint32_t i = 0; i < outLen; ++i) {
		const int8_t s = (int8_t)data[pos >> 16];
		_dmaBuf[i] = (uint8_t)((s * volume) >> 6);
		pos += inc;
		if ((pos >> 16) >= len) {
			pos = ((uint32_t)len - 1) << 16;
		}
	}
	STDL_PlaySample(_dmaBuf, outLen, rate);
}
#endif

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

// DATA\ is GEMDOS 8.3: strip the underscore, uppercase, and where a
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
	char path[32];
	snprintf(path, sizeof(path), "DATA\\%s.STM", stem);

	STDL_Music *m = STDL_LoadMusic(path);
	if (m == 0) {
		if (num < 32 && !(_ymMissing & (1u << num))) {
			_ymMissing |= 1u << num;
			warning("No music for track %d (%s) - run tools/make-music.sh", num, path);
		}
		return false;
	}
	if (_ymMusic != 0) {
		STDL_HaltMusic();
		STDL_FreeMusic(_ymMusic);
	}
	_ymMusic = m;
	_ymTrack = num;
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
	STDL_OpenVoices(6258);
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
			return;
		}
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
