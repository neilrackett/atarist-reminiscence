
/*
 * REminiscence - Flashback interpreter
 * Copyright (C) 2005-2019 Gregory Montoir (cyx@users.sourceforge.net)
 */

#include "file.h"
#include "mixer.h"
#include "mod_player.h"
#include "util.h"

#ifdef ATARIST
extern "C" {
#include <stdl/stdl.h>
}
// a sound effect on voice 3 (mixer.cpp): 0 none, 1 playing, 2 being set
// up - the tick keeps off the voice in both of the last two
extern volatile uint8_t g_stSfxVoice3;
#endif

#ifdef USE_MODPLUG
#include <libmodplug/modplug.h>

struct ModPlayer_impl {

	ModPlugFile *_mf;
	ModPlug_Settings _settings;
	int _songTempo;
	bool _repeatIntro;

	ModPlayer_impl()
		: _mf(0) {
	}

	void init(const int rate) {
		memset(&_settings, 0, sizeof(_settings));
		ModPlug_GetSettings(&_settings);
		_settings.mFlags = MODPLUG_ENABLE_OVERSAMPLING | MODPLUG_ENABLE_NOISE_REDUCTION;
		_settings.mChannels = 2;
		_settings.mBits = 16;
		_settings.mFrequency = rate;
		_settings.mResamplingMode = MODPLUG_RESAMPLE_FIR;
		_settings.mLoopCount = -1;
		ModPlug_SetSettings(&_settings);
	}

	bool load(File *f) {
		const uint32_t size = f->size();
		uint8_t *data = (uint8_t *)malloc(size);
		if (data) {
			f->read(data, size);
			_mf = ModPlug_Load(data, size);
			free(data);
		}
		return _mf != 0;
	}

	void unload() {
		if (_mf) {
			ModPlug_Unload(_mf);
			_mf = 0;
		}
	}

	bool mix(int16_t *buf, int len) {
		if (_mf) {
			const int order = ModPlug_GetCurrentOrder(_mf);
			if (order == 3 && _repeatIntro) {
				ModPlug_SeekOrder(_mf, 1);
				_repeatIntro = false;
			}
			const int count = ModPlug_Read(_mf, buf, len * sizeof(int16_t) * 2);
			// setting mLoopCount to non-zero does not trigger any looping in
			// my test and ModPlug_Read returns 0.
			// looking at the libmodplug-0.8.8 tarball, it seems the variable
			// m_nRepeatCount is commented in sndmix.cpp. Not sure how if this
			// is a known bug, we workaround it here.
			if (count == 0) {
				ModPlug_SeekOrder(_mf, 0);
			}
			return true;
		}
		return false;
	}
};

#else

struct ModPlayer_impl {
	enum {
		NUM_SAMPLES = 31,
		NUM_TRACKS = 4,
		NUM_PATTERNS = 128,
		FRAC_BITS = 12,
		BASE_TEMPO = 125,
		PAULA_FREQ = 3546897
	};

	struct SampleInfo {
		char name[23];
		uint16_t len;
		uint8_t fineTune;
		uint8_t volume;
		uint16_t repeatPos;
		uint16_t repeatLen;
		int8_t *data;

		int8_t getPCM(int offset) const {
			if (offset < 0) {
				offset = 0;
			} else if (offset >= (int)len) {
				offset = len - 1;
			}
			return data[offset];
		}
	};

	struct ModuleInfo {
		char songName[21];
		SampleInfo samples[NUM_SAMPLES];
		uint8_t numPatterns;
		uint8_t patternOrderTable[NUM_PATTERNS];
		uint8_t *patternsTable;
	};

	struct Track {
		SampleInfo *sample;
		uint8_t volume;
		int pos;
		int freq;
		uint16_t period;
		uint16_t periodIndex;
		uint16_t effectData;
		int vibratoSpeed;
		int vibratoAmp;
		int vibratoPos;
		int portamento;
		int portamentoSpeed;
		int retriggerCounter;
		int delayCounter;
		int cutCounter;
#ifdef ATARIST
		// the STDL voice this track plays on: a (re)start is due, and
		// what the voice was last given
		bool trigger;
		bool voiceOn;
		int voiceFreq;
		uint8_t voiceVol;
#endif
	};

	bool _playing;
	int _mixingRate;
	ModuleInfo _modInfo;
	uint8_t _currentPatternOrder;
	uint8_t _currentPatternPos;
	uint8_t _currentTick;
	uint8_t _songSpeed;
	uint8_t _songTempo;
	int _patternDelay;
	int _patternLoopPos;
	int _patternLoopCount;
	int _samplesLeft;
	bool _repeatIntro;
	Track _tracks[NUM_TRACKS];

	ModPlayer_impl();

	void init(const int rate);
	uint16_t findPeriod(uint16_t period, uint8_t fineTune) const;
	bool load(File *f);
	void unload();
	void handleNote(int trackNum, uint32_t noteData);
	void handleTick();
	void applyVolumeSlide(int trackNum, int amount);
	void applyVibrato(int trackNum);
	void applyPortamento(int trackNum);
	void handleEffect(int trackNum, bool tick);
	void mixSamples(int16_t *buf, int len);
	bool mix(int16_t *buf, int len);
#ifdef ATARIST
	uint16_t _stTickAcc;
	int _stVolFor;               // the music_volume _stVol is built for
	uint8_t _stVol[65];          // module volume -> voice volume
	void stStart();
	void stStop();
	void stTick();
	void stSyncVoices();
	void stStartVoice(int v, Track *tk, uint8_t vol);
#endif
};

ModPlayer_impl::ModPlayer_impl()
	: _playing(false) {
	memset(&_modInfo, 0, sizeof(_modInfo));
}

uint16_t ModPlayer_impl::findPeriod(uint16_t period, uint8_t fineTune) const {
	for (int p = 0; p < 36; ++p) {
		if (ModPlayer::_periodTable[p] == period) {
			return fineTune * 36 + p;
		}
	}
#ifdef ATARIST
	// This runs in the voice tick, in VBL context, where error() and
	// its log write cannot: take the nearest note instead.
	int best = 0;
	for (int p = 1; p < 36; ++p) {
		if (ABS(ModPlayer::_periodTable[p] - period) < ABS(ModPlayer::_periodTable[best] - period)) {
			best = p;
		}
	}
	return fineTune * 36 + best;
#else
	error("Invalid period=%d", period);
	return 0;
#endif
}

void ModPlayer_impl::init(const int rate) {
	_mixingRate = rate;
}

bool ModPlayer_impl::load(File *f) {
	f->read(_modInfo.songName, 20);
	_modInfo.songName[20] = 0;
	debug(DBG_MOD, "songName = '%s'", _modInfo.songName);

	for (int s = 0; s < NUM_SAMPLES; ++s) {
		SampleInfo *si = &_modInfo.samples[s];
		f->read(si->name, 22);
		si->name[22] = 0;
		si->len = f->readUint16BE() * 2;
		si->fineTune = f->readByte();
		si->volume = f->readByte();
		si->repeatPos = f->readUint16BE() * 2;
		si->repeatLen = f->readUint16BE() * 2;
		si->data = 0;
		assert(si->len == 0 || si->repeatPos + si->repeatLen <= si->len);
		debug(DBG_MOD, "sample=%d name='%s' len=%d vol=%d", s, si->name, si->len, si->volume);
	}
	_modInfo.numPatterns = f->readByte();
	assert(_modInfo.numPatterns < NUM_PATTERNS);
	f->readByte(); // 0x7F
	f->read(_modInfo.patternOrderTable, NUM_PATTERNS);
	f->readUint32BE(); // 'M.K.', Protracker, 4 channels

	uint8_t n = 0;
	for (int i = 0; i < NUM_PATTERNS; ++i) {
		if (_modInfo.patternOrderTable[i] != 0) {
			n = MAX(n, _modInfo.patternOrderTable[i]);
		}
	}
	debug(DBG_MOD, "numPatterns=%d",n + 1);
	const int patternsSize = (n + 1) * 64 * 4 * 4; // 64 lines of 4 notes per channel
	_modInfo.patternsTable = (uint8_t *)malloc(patternsSize);
	if (!_modInfo.patternsTable) {
		warning("Unable to allocate %d bytes for .MOD patterns table", patternsSize);
		return false;
	}
	f->read(_modInfo.patternsTable, patternsSize);

	for (int s = 0; s < NUM_SAMPLES; ++s) {
		SampleInfo *si = &_modInfo.samples[s];
		if (si->len != 0) {
			si->data = (int8_t *)malloc(si->len);
			if (si->data) {
				f->read(si->data, si->len);
#ifdef ATARIST
			} else {
				// Short of memory: reading on would load the rest of
				// the file into the wrong samples, so the module is
				// given up and the caller plays the YM track instead.
				return false;
#endif
			}
		}
	}

	_currentPatternOrder = 0;
	_currentPatternPos = 0;
	_currentTick = 0;
	_patternDelay = 0;
	_songSpeed = 6;
	_songTempo = BASE_TEMPO;
	_patternLoopPos = 0;
	_patternLoopCount = -1;
	_samplesLeft = 0;
	_repeatIntro = false;
	memset(_tracks, 0, sizeof(_tracks));
	_playing = true;

	return true;
}

void ModPlayer_impl::unload() {
	if (_modInfo.songName[0]) {
		free(_modInfo.patternsTable);
		for (int s = 0; s < NUM_SAMPLES; ++s) {
			free(_modInfo.samples[s].data);
		}
		memset(&_modInfo, 0, sizeof(_modInfo));
	}
	_playing = false;
}

void ModPlayer_impl::handleNote(int trackNum, uint32_t noteData) {
	Track *tk = &_tracks[trackNum];
	uint16_t sampleNum = ((noteData >> 24) & 0xF0) | ((noteData >> 12) & 0xF);
	uint16_t samplePeriod = (noteData >> 16) & 0xFFF;
	uint16_t effectData = noteData & 0xFFF;
	debug(DBG_MOD, "ModPlayer::handleNote(%d) p=%d/%d sampleNumber=0x%X samplePeriod=0x%X effectData=0x%X tk->period=%d", trackNum, _currentPatternPos, _currentPatternOrder, sampleNum, samplePeriod, effectData, tk->period);
	if (sampleNum != 0) {
		tk->sample = &_modInfo.samples[sampleNum - 1];
		tk->volume = tk->sample->volume;
		tk->pos = 0;
#ifdef ATARIST
		tk->trigger = true;
#endif
	}
	if (samplePeriod != 0) {
		tk->periodIndex = findPeriod(samplePeriod, tk->sample->fineTune);
		if ((effectData >> 8) != 0x3 && (effectData >> 8) != 0x5) {
			tk->period = ModPlayer::_periodTable[tk->periodIndex];
			tk->freq = PAULA_FREQ / tk->period;
		} else {
			tk->portamento = ModPlayer::_periodTable[tk->periodIndex];
		}
		tk->vibratoAmp = 0;
		tk->vibratoSpeed = 0;
		tk->vibratoPos = 0;
	}
	tk->effectData = effectData;
}

void ModPlayer_impl::applyVolumeSlide(int trackNum, int amount) {
	debug(DBG_MOD, "ModPlayer::applyVolumeSlide(%d, %d)", trackNum, amount);
	Track *tk = &_tracks[trackNum];
	int vol = tk->volume + amount;
	if (vol < 0) {
		vol = 0;
	} else if (vol > 64) {
		vol = 64;
	}
	tk->volume = vol;
}

void ModPlayer_impl::applyVibrato(int trackNum) {
	static const int8_t sineWaveTable[] = {
	   0,   24,   49,   74,   97,  120, -115,  -95,  -76,  -59,  -44,  -32,  -21,  -12,   -6,   -3,
	  -1,   -3,   -6,  -12,  -21,  -32,  -44,  -59,  -76,  -95, -115,  120,   97,   74,   49,   24,
	   0,  -24,  -49,  -74,  -97, -120,  115,   95,   76,   59,   44,   32,   21,   12,    6,    3,
	   1,    3,    6,   12,   21,   32,   44,   59,   76,   95,  115, -120,  -97,  -74,  -49,  -24
	};
	debug(DBG_MOD, "ModPlayer::applyVibrato(%d)", trackNum);
	Track *tk = &_tracks[trackNum];
	int vib = tk->vibratoAmp * sineWaveTable[tk->vibratoPos] / 128;
	if (tk->period + vib != 0) {
		tk->freq = PAULA_FREQ / (tk->period + vib);
	}
	tk->vibratoPos += tk->vibratoSpeed;
	if (tk->vibratoPos >= 64) {
		tk->vibratoPos = 0;
	}
}

void ModPlayer_impl::applyPortamento(int trackNum) {
	debug(DBG_MOD, "ModPlayer::applyPortamento(%d)", trackNum);
	Track *tk = &_tracks[trackNum];
	if (tk->period < tk->portamento) {
		tk->period = MIN(tk->period + tk->portamentoSpeed, tk->portamento);
	} else if (tk->period > tk->portamento) {
		tk->period = MAX(tk->period - tk->portamentoSpeed, tk->portamento);
	}
	if (tk->period != 0) {
		tk->freq = PAULA_FREQ / tk->period;
	}
}

void ModPlayer_impl::handleEffect(int trackNum, bool tick) {
	Track *tk = &_tracks[trackNum];
	uint8_t effectNum = tk->effectData >> 8;
	uint8_t effectXY = tk->effectData & 0xFF;
	uint8_t effectX = effectXY >> 4;
	uint8_t effectY = effectXY & 0xF;
	debug(DBG_MOD, "ModPlayer::handleEffect(%d) effectNum=0x%X effectXY=0x%X", trackNum, effectNum, effectXY);
	switch (effectNum) {
	case 0x0: // arpeggio
		if (tick && effectXY != 0) {
			uint16_t period = tk->period;
			switch (_currentTick & 3) {
			case 1:
				period = ModPlayer::_periodTable[tk->periodIndex + effectX];
				break;
			case 2:
				period = ModPlayer::_periodTable[tk->periodIndex + effectY];
				break;
			}
			tk->freq = PAULA_FREQ / period;
		}
		break;
	case 0x1: // portamento up
		if (tick) {
			tk->period -= effectXY;
			if (tk->period < 113) { // note B-3
				tk->period = 113;
			}
			tk->freq = PAULA_FREQ / tk->period;
		}
		break;
	case 0x2: // portamento down
		if (tick) {
			tk->period += effectXY;
			if (tk->period > 856) { // note C-1
				tk->period = 856;
			}
			tk->freq = PAULA_FREQ / tk->period;
		}
		break;
	case 0x3: // tone portamento
		if (!tick) {
        	if (effectXY != 0) {
        		tk->portamentoSpeed = effectXY;
        	}
		} else {
			applyPortamento(trackNum);
		}
		break;
	case 0x4: // vibrato
		if (!tick) {
			if (effectX != 0) {
				tk->vibratoSpeed = effectX;
			}
			if (effectY != 0) {
				tk->vibratoAmp = effectY;
			}
		} else {
			applyVibrato(trackNum);
		}
		break;
	case 0x5: // tone portamento + volume slide
		if (tick) {
			applyPortamento(trackNum);
			applyVolumeSlide(trackNum, effectX - effectY);
		}
		break;
	case 0x6: // vibrato + volume slide
		if (tick) {
			applyVibrato(trackNum);
			applyVolumeSlide(trackNum, effectX - effectY);
		}
		break;
	case 0x9: // set sample offset
		if (!tick) {
			tk->pos = effectXY << (8 + FRAC_BITS);
#ifdef ATARIST
			tk->trigger = true;
#endif
		}
		break;
	case 0xA: // volume slide
		if (tick) {
			applyVolumeSlide(trackNum, effectX - effectY);
		}
		break;
	case 0xB: // position jump
		if (!tick) {
			_currentPatternOrder = effectXY;
			_currentPatternPos = 0;
			assert(_currentPatternOrder < _modInfo.numPatterns);
		}
		break;
	case 0xC: // set volume
		if (!tick) {
			assert(effectXY <= 64);
			tk->volume = effectXY;
		}
		break;
	case 0xD: // pattern break
		if (!tick) {
			_currentPatternPos = effectX * 10 + effectY;
			assert(_currentPatternPos < 64);
			++_currentPatternOrder;
			debug(DBG_MOD, "_currentPatternPos = %d _currentPatternOrder = %d", _currentPatternPos, _currentPatternOrder);
		}
		break;
	case 0xE: // extended effects
		switch (effectX) {
		case 0x0: // set filter, ignored
			break;
		case 0x1: // fineslide up
			if (!tick) {
				tk->period -= effectY;
				if (tk->period < 113) { // B-3 note
					tk->period = 113;
				}
				tk->freq = PAULA_FREQ / tk->period;
			}
			break;
		case 0x2: // fineslide down
			if (!tick) {
				tk->period += effectY;
				if (tk->period > 856) { // C-1 note
					tk->period = 856;
				}
				tk->freq = PAULA_FREQ / tk->period;
			}
			break;
		case 0x6: // loop pattern
			if (!tick) {
				if (effectY == 0) {
					_patternLoopPos = _currentPatternPos | (_currentPatternOrder << 8);
					debug(DBG_MOD, "_patternLoopPos=%d/%d", _currentPatternPos, _currentPatternOrder);
				} else {
					if (_patternLoopCount == -1) {
						_patternLoopCount = effectY;
						_currentPatternPos = _patternLoopPos & 0xFF;
						_currentPatternOrder = _patternLoopPos >> 8;
					} else {
						--_patternLoopCount;
						if (_patternLoopCount != 0) {
							_currentPatternPos = _patternLoopPos & 0xFF;
							_currentPatternOrder = _patternLoopPos >> 8;
						} else {
							_patternLoopCount = -1;
						}
					}
					debug(DBG_MOD, "_patternLoopCount=%d", _patternLoopCount);
				}
			}
			break;
		case 0x9: // retrigger sample
			if (tick) {
				tk->retriggerCounter = effectY;
			} else {
				if (tk->retriggerCounter == 0) {
					tk->pos = 0;
#ifdef ATARIST
					tk->trigger = true;
#endif
					tk->retriggerCounter = effectY;
					debug(DBG_MOD, "retrigger sample=%d _songSpeed=%d", effectY, _songSpeed);
				}
				--tk->retriggerCounter;
			}
			break;
		case 0xA: // fine volume slide up
			if (!tick) {
				applyVolumeSlide(trackNum, effectY);
			}
			break;
		case 0xB: // fine volume slide down
			if (!tick) {
				applyVolumeSlide(trackNum, -effectY);
			}
			break;
		case 0xC: // cut sample
			if (!tick) {
				tk->cutCounter = effectY;
			} else {
				--tk->cutCounter;
				if (tk->cutCounter == 0) {
					tk->volume = 0;
				}
			}
			break;
		case 0xD: // delay sample
			if (!tick) {
				tk->delayCounter = effectY;
			} else {
				if (tk->delayCounter != 0) {
					--tk->delayCounter;
				}
			}
			break;
		case 0xE: // delay pattern
			if (!tick) {
				debug(DBG_MOD, "ModPlayer::handleEffect() _currentTick=%d delay pattern=%d", _currentTick, effectY);
				_patternDelay = effectY;
			}
			break;
		default:
#ifndef ATARIST
			warning("Unhandled extended effect 0x%X params=0x%X", effectX, effectY);
#endif
			break;
		}
		break;
	case 0xF: // set speed
		if (!tick) {
			if (effectXY < 0x20) {
				_songSpeed = effectXY;
			} else {
				_songTempo = effectXY;
			}
		}
		break;
	default:
#ifndef ATARIST
		warning("Unhandled effect 0x%X params=0x%X", effectNum, effectXY);
#endif
		break;
	}
}

void ModPlayer_impl::handleTick() {
	if (!_playing) {
		return;
	}
//	if (_patternDelay != 0) {
//		--_patternDelay;
//		return;
//	}
	if (_currentTick == 0) {
		debug(DBG_MOD, "_currentPatternOrder=%d _currentPatternPos=%d", _currentPatternOrder, _currentPatternPos);
		uint8_t currentPattern = _modInfo.patternOrderTable[_currentPatternOrder];
		const uint8_t *p = _modInfo.patternsTable + (currentPattern * 64 + _currentPatternPos) * 16;
		for (int i = 0; i < NUM_TRACKS; ++i) {
			uint32_t noteData = READ_BE_UINT32(p);
			handleNote(i, noteData);
			p += 4;
		}
		++_currentPatternPos;
		if (_currentPatternPos == 64) {
			++_currentPatternOrder;
			_currentPatternPos = 0;
			debug(DBG_MOD, "ModPlayer::handleTick() _currentPatternOrder = %d/%d", _currentPatternOrder, _modInfo.numPatterns);
			// On the amiga version, the introduction cutscene is shorter than the PC version ;
			// so the music module doesn't synchronize at all with the PC datafiles, here we
			// add a hack to let the music play longer
			if (_currentPatternOrder == 3 && _repeatIntro) {
				_currentPatternOrder = 1;
				_repeatIntro = false;
//				warning("Introduction module synchronization hack");
			}
		}
	}
	for (int i = 0; i < NUM_TRACKS; ++i) {
		handleEffect(i, (_currentTick != 0));
	}
	++_currentTick;
	if (_currentTick == _songSpeed) {
		_currentTick = 0;
	}
	if (_currentPatternOrder == _modInfo.numPatterns) {
		debug(DBG_MOD, "ModPlayer::handleEffect() _currentPatternOrder == _modInfo.numPatterns");
//		_playing = false;
		_currentPatternOrder = 0;
	}
}

#ifndef ATARIST
void ModPlayer_impl::mixSamples(int16_t *buf, int samplesLen) {
	for (int i = 0; i < NUM_TRACKS; ++i) {
		Track *tk = &_tracks[i];
		if (tk->sample != 0 && tk->delayCounter == 0) {
			int16_t *mixbuf = buf;
			SampleInfo *si = tk->sample;
			int len = si->len << FRAC_BITS;
			int loopLen = si->repeatLen << FRAC_BITS;
			int loopPos = si->repeatPos << FRAC_BITS;
			int deltaPos = (tk->freq << FRAC_BITS) / _mixingRate;
			int curLen = samplesLen;
			int pos = tk->pos;
			while (curLen != 0) {
				int count;
				if (loopLen > (2 << FRAC_BITS)) {
					if (pos >= loopPos + loopLen) {
						pos -= loopLen;
					}
					count = MIN(curLen, (loopPos + loopLen - pos - 1) / deltaPos + 1);
					curLen -= count;
				} else {
					if (pos >= len) {
						count = 0;
					} else {
						count = MIN(curLen, (len - pos - 1) / deltaPos + 1);
					}
					curLen = 0;
				}
				while (count--) {
					const int sample = S8_to_S16(si->getPCM(pos >> FRAC_BITS)) * tk->volume / 64;
					*mixbuf = ADDC_S16(*mixbuf, sample);
					++mixbuf;
					*mixbuf = ADDC_S16(*mixbuf, sample);
					++mixbuf;
					pos += deltaPos;
				}
			}
			tk->pos = pos;
		}
	}
}

bool ModPlayer_impl::mix(int16_t *buf, int len) {
	memset(buf, 0, sizeof(int16_t) * len * 2); // stereo
	if (_playing) {
		const int samplesPerTick = _mixingRate / (50 * _songTempo / BASE_TEMPO);
		while (len != 0) {
			if (_samplesLeft == 0) {
				handleTick();
				_samplesLeft = samplesPerTick;
			}
			int count = _samplesLeft;
			if (count > len) {
				count = len;
			}
			_samplesLeft -= count;
			len -= count;
			mixSamples(buf, count);
			buf += count * 2; // stereo
		}
	}
	return _playing;
}
#else
// On the ST the software mixer above is replaced by the STDL_Voice
// device on an STE: four hardware-mixed voices, Paula-style, which is
// what a four-channel module is written for. The sequencer runs as
// ever - notes, effects, speed - but from the device's voice tick, and
// after each tick the voices are told what changed: a note (re)started,
// a new pitch (vibrato, portamento, arpeggio) or a new volume.

static void stModTick(void *ud) {
	((ModPlayer_impl *)ud)->stTick();
}

void ModPlayer_impl::stStart() {
	_stTickAcc = 0;
	_stVolFor = -1;              // built on the first tick
	STDL_ResumeVoices();         // unconditional, see mixer.cpp
	ST_setVoiceTick(stModTick, this);
}

void ModPlayer_impl::stStop() {
	// the tick first - it reads the module - then the voices, which
	// read the samples, and only then may unload free either
	ST_setVoiceTick(0, 0);
	for (int i = 0; i < NUM_TRACKS; ++i) {
		if (i == 3 && g_stSfxVoice3 != 0) {
			continue;            // a sound effect's, not ours
		}
		STDL_StopVoice(i);
	}
}

// The voice tick comes 50 times a second, which is tempo 125; the
// sequencer runs tempo/125 ticks for each, so the few scenes at 107 to
// 155 land their ticks on the same 20ms grid rather than between.
void ModPlayer_impl::stTick() {
	const uint16_t tempo = (_songTempo >= 32) ? _songTempo : BASE_TEMPO;
	_stTickAcc = (uint16_t)(_stTickAcc + tempo);
	while (_stTickAcc >= BASE_TEMPO && _playing) {
		_stTickAcc = (uint16_t)(_stTickAcc - BASE_TEMPO);
		handleTick();
		stSyncVoices();
	}
}

// start a voice where the track's sample starts, the sample offset
// effect (9xx) included
void ModPlayer_impl::stStartVoice(int v, Track *tk, uint8_t vol) {
	const SampleInfo *si = tk->sample;
	const bool looping = (si->repeatLen > 2);
	// the software mixer wraps at the loop's end on the first pass too
	const uint32_t end = looping ? (uint32_t)si->repeatPos + si->repeatLen : si->len;
	const uint32_t off = (uint32_t)tk->pos >> FRAC_BITS;
	if (off >= end || tk->freq <= 0) {
		STDL_StopVoice(v);
		tk->voiceOn = false;
		return;
	}
	uint32_t loopOff = 0, loopLen = 0;
	if (looping) {
		if (off <= si->repeatPos) {
			loopOff = si->repeatPos - off;
			loopLen = si->repeatLen;
		} else {
			loopLen = end - off;   // started inside the loop: loop the rest of it
		}
	}
	STDL_SetVoice(v, si->data + off, end - off, loopOff, loopLen, (uint32_t)tk->freq, vol);
	tk->voiceOn = true;
	tk->voiceFreq = tk->freq;
	tk->voiceVol = vol;
}

void ModPlayer_impl::stSyncVoices() {
	// The module's volume scaled by music_volume. A voice at 64 is
	// half the output, so four channels there would be twice as loud
	// as an effect; at the default 50 a channel peaks at 16, and all
	// four together where one effect does. Measured on the title
	// track that is close to the YM stream's level at the same
	// setting (at 32 a channel it was 9dB over it). A table, rebuilt
	// when the setting moves: worked out per channel per tick it was
	// a __mulsi3 and a divide each, 1.6% of an STE.
	if (_stVolFor != g_options.music_volume) {
		_stVolFor = g_options.music_volume;
		for (int v = 0; v <= 64; ++v) {
			_stVol[v] = (uint8_t)(v * _stVolFor / 200);
		}
	}
	for (int i = 0; i < NUM_TRACKS; ++i) {
		Track *tk = &_tracks[i];
		if (i == 3 && g_stSfxVoice3 != 0) {
			// a sound effect has the voice: the channel sits out until
			// it ends, and comes back with its next note
			if (g_stSfxVoice3 == 2 || STDL_VoiceActive(3)) {
				tk->voiceOn = false;
				continue;
			}
			g_stSfxVoice3 = 0;
		}
		if (!tk->sample || !tk->sample->data || tk->delayCounter != 0) {
			// no sample yet, or a note held back (EDx): silent, as the
			// software mixer leaves a delayed track; the note starts
			// when the delay runs out
			if (tk->voiceOn) {
				STDL_StopVoice(i);
				tk->voiceOn = false;
			}
			continue;
		}
		const uint8_t vol = _stVol[(tk->volume > 64) ? 64 : tk->volume];
		if (tk->trigger) {
			tk->trigger = false;
			stStartVoice(i, tk, vol);
		} else if (tk->voiceOn) {
			if (tk->freq != tk->voiceFreq && tk->freq > 0) {
				STDL_SetVoiceFreq(i, (uint32_t)tk->freq);
				tk->voiceFreq = tk->freq;
			}
			if (vol != tk->voiceVol) {
				STDL_SetVoiceVolume(i, vol);
				tk->voiceVol = vol;
			}
		}
	}
}
#endif
#endif

ModPlayer::ModPlayer(Mixer *mixer, FileSystem *fs)
	: _playing(false), _mix(mixer), _fs(fs) {
	_impl = new ModPlayer_impl;
}

ModPlayer::~ModPlayer() {
	delete _impl;
}

void ModPlayer::play(int num, int tempo) {
#ifdef ATARIST
	// An STE plays MUSIC\<name>.MOD, named as the YM tracks are
	// (ST_musicName); without the voice device (a plain ST, or
	// ste_sound=false) there is nothing to play it on.
	if (num * 2 >= _namesCount || !STDL_VoicesOpen()) {
		return;
	}
	const char *want = _names[num * 2 + 1] ? _names[num * 2 + 1] : _names[num * 2];
	char name[16];
	ST_musicName(want, name);
	strcat(name, ".MOD");
	File f;
	if (!f.open(name, "rb", "MUSIC")) {
		return;
	}
	stop();
	if (_impl->load(&f)) {
		// 0, the title's call, means no tempo of the scene's own
		_impl->_songTempo = (tempo >= 32) ? tempo : (int)ModPlayer_impl::BASE_TEMPO;
		_impl->_repeatIntro = false;   // a PC-data fix; these are the Amiga's modules
		_impl->stStart();
		_playing = true;
		info("Music: %s at tempo %d", name, _impl->_songTempo);
	} else {
		// not playing, so stop() would never free what it did get
		_impl->unload();
		info("Music: %s does not fit in memory", name);
	}
#else
	if (num * 2 < _namesCount) {
		File f;
		if (!f.open(_names[num * 2], "rb", _fs)) {
			const char *p = _names[num * 2 + 1];
			char name[32];
			snprintf(name, sizeof(name), "mod.flashback-%s", p ? p : _names[num * 2]);
			if (!f.open(name, "rb", _fs)) {
				return;
			}
		}
		_impl->init(_mix->getSampleRate());
		if (_impl->load(&f)) {
			_impl->_songTempo = tempo;
			_impl->_repeatIntro = (num == 0) && !_isAmiga;
			_mix->setPremixHook(mixCallback, _impl);
			_playing = true;
		}
	}
#endif
}

void ModPlayer::stop() {
	if (_playing) {
#ifdef ATARIST
		_impl->stStop();
#else
		_mix->setPremixHook(0, 0);
#endif
		_impl->unload();
		_playing = false;
	}
}

#ifndef ATARIST
bool ModPlayer::mixCallback(void *param, int16_t *buf, int len) {
	return ((ModPlayer_impl *)param)->mix(buf, len);
}
#endif
