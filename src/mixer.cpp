
/*
 * REminiscence - Flashback interpreter
 * Copyright (C) 2005-2019 Gregory Montoir (cyx@users.sourceforge.net)
 */

#include "mixer.h"
#include "systemstub.h"
#include "util.h"

#ifdef ATARIST
extern "C" {
#include <stdl/stdl.h>
}

// STE DMA one-shot playback: no per-frame CPU cost, monophonic.
// Samples are signed 8-bit; resample to the nearest DMA rate and
// apply the volume while copying.
static uint8_t *_dmaBuf;
static uint32_t _dmaBufSize;

static void ATARIST_playSample(const uint8_t *data, uint32_t len, uint16_t freq, uint8_t volume) {
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

Mixer::Mixer(FileSystem *fs, SystemStub *stub, const PrfMidiDriver *midiDriver, const char *midiSoundFont)
	: _stub(stub), _musicType(MT_NONE), _cpc(this, fs), _mod(this, fs), _ogg(this, fs), _prf(this, fs, midiDriver, midiSoundFont), _sfx(this) {
	_musicTrack = -1;
	_backgroundMusicType = MT_NONE;
}

void Mixer::init() {
	memset(_channels, 0, sizeof(_channels));
	_premixHook = 0;
	_stub->startAudio(Mixer::mixCallback, this);
}

void Mixer::free() {
	setPremixHook(0, 0);
	stopAll();
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
