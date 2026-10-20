
/*
 * REminiscence - Flashback interpreter
 * Copyright (C) 2005-2019 Gregory Montoir (cyx@users.sourceforge.net)
 * Atari ST port changes Copyright (C) 2026 Neil Rackett
 */

#include "decode_mac.h"
#include "file.h"
#include "fs.h"
#include "resource.h"
#include "unpack.h"
#include "video_st.h"
#include "util.h"
#ifdef ATARIST
#include <mint/osbind.h>
#endif

static const bool kCalculateAudioPeak = false;

Resource::Resource(FileSystem *fs, ResourceType ver, Language lang) {
	memset(this, 0, sizeof(Resource));
	_fs = fs;
	_type = ver;
	_lang = lang;
	_isDemo = false;
	_archive = 0;
	_aba = 0;
	_mac = 0;
	_paq = 0;
	_bigEndianData = g_features->has_bigendian_data;
	_readUint16 = g_features->has_bigendian_data ? READ_BE_UINT16 : READ_LE_UINT16;
	_readUint32 = g_features->has_bigendian_data ? READ_BE_UINT32 : READ_LE_UINT32;
	_scratchBuffer = (uint8_t *)malloc(kScratchBufferSize);
	if (!_scratchBuffer) {
		error("Unable to allocate temporary memory buffer");
	}
#ifdef ATARIST
	// 28K as upstream, unless the machine has RAM to spare. With 4MB
	// the banks of most of a level fit at once, where 28K held about one
	// room's: every room change emptied it and unpacked the tile banks
	// again, half a second of a room entry on an 8MHz ST. Asked of
	// GEMDOS, not read from phystop: this runs before the game is in
	// supervisor mode, where low memory is out of reach. 4MB has about
	// 2.9MB free here and 2.5MB about 1.4MB.
	const int kBankDataSize = ((Malloc(-1) >> 10) >= 2048 && !(g_options.ab & kAbSmallBankArena)) ? 384 * 1024 : 0x7000;
#else
	static const int kBankDataSize = 0x7000;
#endif
	_bankData = (uint8_t *)malloc(kBankDataSize);
	if (!_bankData) {
		error("Unable to allocate bank data buffer");
	}
	_bankDataTail = _bankData + kBankDataSize;
	clearBankData();
#ifdef ATARIST
	info("Bank arena: %dK (%ldK was free)", kBankDataSize >> 10, (long)Malloc(-1) >> 10);
#endif
}

Resource::~Resource() {
	clearLevelRes();
	MAC_unloadLevelData();
	free(_fnt);
	free(_icn);
	free(_tab);
	free(_spc);
	free(_spr1);
	free(_scratchBuffer);
	free(_cmd);
	free(_pol);
	free(_cine_off);
	free(_cine_txt);
	for (int i = 0; i < _numSfx; ++i) {
		free(_sfxList[i].data);
	}
	free(_sfxList);
	free(_bankData);
	free(_credits);
	delete _aba;
	delete _mac;
	delete _paq;
}

static const char *_demoAba = "DEMO_UK.ABA";

static const char *const _joystickAba[] = {
	"GLOB1_FB.ABA", "GLOB2_FB.ABA", "GLOB_FR.ABA", 0
};

void Resource::init() {
	switch (_type) {
	case kResourceTypeAmiga:
		_isDemo = _fs->exists("demo.lev");
		break;
	case kResourceTypeDOS:
		if (_fs->exists(_demoAba)) { // fbdemous
			_aba = new ResourceAba(_fs);
			_aba->readEntries(_demoAba);
			_isDemo = true;
		} else if (_fs->exists(_joystickAba[0])) { // Joystick "Hors Serie" April 1996
			_aba = new ResourceAba(_fs);
			for (int i = 0; _joystickAba[i]; ++i) {
				_aba->readEntries(_joystickAba[i]);
			}
		} else if (!fileExists("LEVEL2.MAP")) { // fbdemofr (no cutscenes)
			_isDemo = true;
		}
		_archive = _aba;
		break;
	case kResourceTypeMac:
		if (_fs->exists(ResourceMac::FILENAME1)) {
			_mac = new ResourceMac(ResourceMac::FILENAME1, _fs);
		} else if (_fs->exists(ResourceMac::FILENAME2)) {
			_mac = new ResourceMac(ResourceMac::FILENAME2, _fs);
		}
		_mac->load();
		MAC_dumpVersion();
		break;
	case kResourceTypePC98:
		_paq = new ResourcePaq(_fs);
		_paq->open();
		_archive = _paq;
		break;
	case kResourceTypeSega:
		break;
	}
}

void Resource::fini() {
}

void Resource::setLanguage(Language lang) {
	if (_lang != lang) {
		_lang = lang;
		// reload global language specific data files
		free_TEXT();
		load_TEXT();
		free_CINE();
		load_CINE();
	}
}

bool Resource::fileExists(const char *filename) {
	if (_fs->exists(filename)) {
		return true;
	} else if (_archive) {
		return _archive->hasEntry(filename);
	}
	return false;
}

void Resource::clearLevelRes() {
#ifdef ATARIST
	ST_freeMonsters();
	free(_stLevU); _stLevU = 0;
	if (_stMbkU) {
		// the banks were the arena's: it is all free again
		_stMbkU = 0;
		_stMbkUCount = 0;
		_stMbkUEnd = 0;
		clearBankData();
	}
#endif
	free(_tbn); _tbn = 0;
	free(_mbk); _mbk = 0;
	free(_pal); _pal = 0;
	free(_map); _map = 0;
	free(_lev); _lev = 0;
	_levNum = -1;
	free(_sgd); _sgd = 0;
	free(_bnq); _bnq = 0;
	if (_type != kResourceTypeSega) {
		free(_ani); _ani = 0;
		free_OBJ();
	}
}

// Everything a level loads, for the way back to the title: the level
// files clearLevelRes frees when the next level replaces them, plus
// the sprite positions and the sound effects, which are the biggest
// of them (up to 152K). The title does not need any of it, and on a
// 2MB machine it could not find room for its picture with a large
// level still loaded. The caller stops the effects playing first.
void Resource::freeLevelData() {
	clearLevelRes();
	free(_spc); _spc = 0;
	_numSpc = 0;
	for (int i = 0; i < _numSfx; ++i) {
		free(_sfxList[i].data);
	}
	free(_sfxList); _sfxList = 0;
	_numSfx = 0;
}

void Resource::load_DEM(const char *filename) {
	free(_dem); _dem = 0;
	_demLen = 0;
	if (_mac) {
		char name[256];
		snprintf(name, sizeof(name), "Demo Level %c", filename[4]);
		_dem = decodeResourceMacData(name, true);
		_demLen = _resourceMacDataSize;
		for (int i = 0; i < _demLen; ++i) {
			uint8_t mask = 0;
			if (_dem[i] & 0x10) {
				mask |= 0x40; // shift
			}
			if (_dem[i] & 0x20) {
				mask |= 0x10; // enter
			}
			if (_dem[i] & 0x40) {
				mask |= 0x20; // space
			}
			if (_dem[i] & 0x80) {
				mask |= 0x80; // backspace
			}
			_dem[i] = mask | (_dem[i] & 0xF);
		}
		if (filename[4] == '5' && _demLen > 1292 && 0) {
			// shorten the demo inputs to workaround game being de-synced
			_demLen = 1292;
		}
		return;
	}
	File f;
	if (f.open(filename, "rb", _fs)) {
		_demLen = f.size();
		_dem = (uint8_t *)malloc(_demLen);
		if (_dem) {
			f.read(_dem, _demLen);
		}
	} else if (_archive) {
		uint32_t size;
		_dem = _archive->loadEntry(filename, &size);
		if (_dem) {
			_demLen = size;
		}
	}
}

static void decodeSfxFibonacci(SoundFx *sfx, File &f) {
	f.seek(sfx->offset);
	const int len = (sfx->len * 2) - 1;
	uint8_t *data = (uint8_t *)malloc(len);
	if (!data) {
		error("Unable to allocate SoundFx data buffer");
	}
	sfx->data = data;

	// Fibonacci-delta decoding
	static const int8_t codeToDelta[16] = { -34, -21, -13, -8, -5, -3, -2, -1, 0, 1, 2, 3, 5, 8, 13, 21 };
	int c = (int8_t)f.readByte();
	*data++ = c;
	sfx->peak = kCalculateAudioPeak ? ABS(c) : 0;

	for (int j = 1; j < sfx->len; ++j) {
		const uint8_t d = f.readByte();

		c += codeToDelta[d >> 4];
		*data++ = CLIP(c, -128, 127);
		if (kCalculateAudioPeak && ABS(c) > sfx->peak) {
			sfx->peak = ABS(c);
		}

		c += codeToDelta[d & 15];
		*data++ = CLIP(c, -128, 127);
		if (kCalculateAudioPeak && ABS(c) > sfx->peak) {
			sfx->peak = ABS(c);
		}
	}
	sfx->len = len;
}

void Resource::load_FIB(const char *fileName) {
	debug(DBG_RES, "Resource::load_FIB('%s')", fileName);
	snprintf(_entryName, sizeof(_entryName), "%s.FIB", fileName);
	File f;
	if (f.open(_entryName, "rb", _fs)) {
		_numSfx = f.readUint16LE();
		_sfxList = (SoundFx *)malloc(_numSfx * sizeof(SoundFx));
		if (!_sfxList) {
			error("Unable to allocate SoundFx table");
		}
		for (int i = 0; i < _numSfx; ++i) {
			SoundFx *sfx = &_sfxList[i];
			sfx->offset = f.readUint32LE();
			sfx->len = f.readUint16LE();
			sfx->freq = 6000;
			sfx->data = 0;
		}
		for (int i = 0; i < _numSfx; ++i) {
			SoundFx *sfx = &_sfxList[i];
			if (sfx->len == 0) {
				continue;
			}
			decodeSfxFibonacci(sfx, f);
		}
		if (f.ioErr()) {
			error("I/O error when reading '%s'", _entryName);
		}
	} else {
		error("Cannot open '%s'", _entryName);
	}
}

static void normalizeSPL(SoundFx *sfx) {
	static const int kGain = 2;

	sfx->peak = kCalculateAudioPeak ? ABS(sfx->data[0]) : 0;
#ifdef ATARIST
	// By table, through locals: written as below, gcc 4.6 reloads the
	// pointer and the length after every store (a byte store may alias
	// them) and spent ~115 cycles a sample - 2.1s of an STE's start of
	// level 1, whose effects are 145K.
	if (!kCalculateAudioPeak) {
		static int8_t half[256];
		static bool halfReady;
		if (!halfReady) {
			for (int i = 0; i < 256; ++i) {
				half[i] = (int8_t)((int8_t)i / kGain);
			}
			halfReady = true;
		}
		uint8_t *p = sfx->data + 1;
		for (int n = sfx->len - 1; n > 0; --n, ++p) {
			*p = (uint8_t)half[*p];
		}
		return;
	}
#endif
	for (int i = 1; i < sfx->len; ++i) {
		const int8_t sample = sfx->data[i];
		if (kCalculateAudioPeak && ABS(sample) > sfx->peak) {
			sfx->peak = ABS(sample);
		}
		sfx->data[i] = sample / kGain;
	}
}

void Resource::load_SPL_demo() {
	_numSfx = NUM_SFXS;
	_sfxList = (SoundFx *)calloc(_numSfx, sizeof(SoundFx));
	if (!_sfxList) {
		return;
	}
	for (int i = 0; _splNames[i] && i < NUM_SFXS; ++i) {
		File f;
		if (f.open(_splNames[i], "rb", _fs)) {
			SoundFx *sfx = &_sfxList[i];
			const int size = f.size();
			sfx->data = (uint8_t *)malloc(size);
			if (sfx->data) {
				f.read(sfx->data, size);
				sfx->offset = 0;
				sfx->len = size;
				sfx->freq = kPaulaFreq / 650;
				normalizeSPL(sfx);
			}
		}
	}
}

void Resource::load_MAP_menu(const char *fileName, uint8_t *dstPtr) {
	debug(DBG_RES, "Resource::load_MAP_menu('%s')", fileName);
	static const int kMenuMapSize = 0x3800 * 4;
	snprintf(_entryName, sizeof(_entryName), "%s.MAP", fileName);
	File f;
	if (f.open(_entryName, "rb", _fs)) {
		if (f.read(dstPtr, kMenuMapSize) != kMenuMapSize) {
			error("Failed to read '%s'", _entryName);
		}
		if (f.ioErr()) {
			error("I/O error when reading '%s'", _entryName);
		}
		return;
	} else if (_archive) {
		uint32_t size = 0;
		uint8_t *dat = _archive->loadEntry(_entryName, &size);
		if (dat) {
			if (size != kMenuMapSize) {
				error("Unexpected size %d for '%s'", size, _entryName);
			}
			memcpy(dstPtr, dat, size);
			free(dat);
			return;
		}
	}
	error("Cannot load '%s'", _entryName);
}

void Resource::load_PAL_menu(const char *fileName, uint8_t *dstPtr) {
	debug(DBG_RES, "Resource::load_PAL_menu('%s')", fileName);
	static const int kMenuPalSize = 768;
	snprintf(_entryName, sizeof(_entryName), "%s.PAL", fileName);
	File f;
	if (f.open(_entryName, "rb", _fs)) {
		if (f.read(dstPtr, kMenuPalSize) != kMenuPalSize) {
			error("Failed to read '%s'", _entryName);
		}
		if (f.ioErr()) {
			error("I/O error when reading '%s'", _entryName);
		}
		return;
	} else if (_archive) {
		uint32_t size = 0;
		uint8_t *dat = _archive->loadEntry(_entryName, &size);
		if (dat) {
			if (size != kMenuPalSize) {
				error("Unexpected size %d for '%s'", size, _entryName);
			}
			memcpy(dstPtr, dat, size);
			free(dat);
			return;
		}
	}
	error("Cannot load '%s'", _entryName);
}

void Resource::load_CMP_menu(const char *fileName) {
	File f;
#ifdef ATARIST
	uint32_t unpacked;
	if (_type == kResourceTypeAmiga && ST_openUnpacked(&f, fileName, &unpacked, 1)) {
		if (unpacked <= (uint32_t)kScratchBufferSize) {
			f.read(_scratchBuffer, unpacked);
			if (f.ioErr()) {
				error("I/O error when reading 'UNPACKED\\%s'", fileName);
			}
			return;
		}
		f.close();
	}
#endif
	if (f.open(fileName, "rb", _fs)) {
		const uint32_t size = f.readUint32BE();
		uint8_t *tmp = (uint8_t *)malloc(size);
		if (!tmp) {
			error("Failed to allocate CMP temporary buffer");
		}
		f.read(tmp, size);
		if (!bytekiller_unpack(_scratchBuffer, kScratchBufferSize, tmp, size)) {
			error("Bad CRC for %s", fileName);
		}
                free(tmp);
		return;
	}
	error("Cannot load '%s'", fileName);
}

void Resource::load_SPR_OFF(const char *fileName, uint8_t *sprData, const char *ext) {
	debug(DBG_RES, "Resource::load_SPR_OFF('%s')", fileName);
	snprintf(_entryName, sizeof(_entryName), "%s.%s", fileName, ext);
	uint8_t *offData = 0;
	File f;
	if (f.open(_entryName, "rb", _fs)) {
		const int len = f.size();
		offData = (uint8_t *)malloc(len);
		if (!offData) {
			error("Unable to allocate sprite offsets");
		}
		f.read(offData, len);
		if (f.ioErr()) {
			error("I/O error when reading '%s'", _entryName);
		}
	} else if (_archive) {
		offData = _archive->loadEntry(_entryName);
	}
	if (offData) {
		const uint8_t *p = offData;
		if (_type == kResourceTypeSega) {
			for (int i = 0; i < NUM_SPRITES; ++i, p += 4) {
				const int32_t offset = READ_BE_UINT32(p);
				_sprData[i] = (offset == -1) ? 0 : sprData + offset;
			}
		} else {
			for (int i; (i = READ_LE_UINT16(p)) != 0xFFFF; p += 6) {
				assert(i < NUM_SPRITES);
				const int32_t offset = READ_LE_UINT32(p + 2);
				_sprData[i] = (offset == -1) ? 0 : sprData + offset;
			}
		}
		free(offData);
		return;
	}
	error("Cannot load '%s'", _entryName);
}

static const char *getCineName(Language lang, ResourceType type) {
	switch (lang) {
	case LANG_FR:
		if (type == kResourceTypeAmiga) {
			return "FR";
		}
		return "FR_";
	case LANG_DE:
		return "GER";
	case LANG_SP:
		return "SPA";
	case LANG_IT:
		return "ITA";
	case LANG_EN:
	default:
		return "ENG";
	}
}

void Resource::load_CINE() {
	const char *prefix = getCineName(_lang, _type);
	debug(DBG_RES, "Resource::load_CINE('%s')", prefix);
	switch (_type) {
	case kResourceTypeAmiga:
		if (_cine_txt == 0) {
			snprintf(_entryName, sizeof(_entryName), "%sCINE.TXT", prefix);
			File f;
			if (f.open(_entryName, "rb", _fs)) {
				const int len = f.size();
				_cine_txt = (uint8_t *)malloc(len + 1);
				if (!_cine_txt) {
					error("Unable to allocate cinematics text data");
				}
				f.read(_cine_txt, len);
				if (f.ioErr()) {
					error("I/O error when reading '%s'", _entryName);
				}
				_cine_txt[len] = 0;
				uint8_t *p = _cine_txt;
				for (int i = 0; i < NUM_CUTSCENE_TEXTS; ++i) {
					_cineStrings[i] = p;
					uint8_t *sep = (uint8_t *)memchr(p, '\n', &_cine_txt[len] - p);
					if (!sep) {
						break;
					}
					p = sep + 1;
				}
			} else if (_isDemo) {
				// file not present in demo datafiles
				return;
			}
		}
		if (!_cine_txt) {
			error("Cannot load '%s'", _entryName);
		}
		break;
	case kResourceTypeDOS:
	case kResourceTypeSega:
		if (_cine_off == 0) {
			snprintf(_entryName, sizeof(_entryName), "%sCINE.BIN", prefix);
			if (!fileExists(_entryName)) {
				strcpy(_entryName, "ENGCINE.BIN");
			}
			File f;
			if (f.open(_entryName, "rb", _fs)) {
				int len = f.size();
				_cine_off = (uint8_t *)malloc(len);
				if (!_cine_off) {
					error("Unable to allocate cinematics offsets");
				}
				f.read(_cine_off, len);
				if (f.ioErr()) {
					error("I/O error when reading '%s'", _entryName);
				}
			} else if (_aba) {
				_cine_off = _aba->loadEntry(_entryName);
			} else if (_isDemo) {
				return; // some demos do not have cutscene datafiles
			}
		}
		if (!_cine_off) {
			error("Cannot load '%s'", _entryName);
		}
		if (_cine_txt == 0) {
			snprintf(_entryName, sizeof(_entryName), "%sCINE.TXT", prefix);
			if (!fileExists(_entryName)) {
				strcpy(_entryName, "ENGCINE.TXT");
			}
			File f;
			if (f.open(_entryName, "rb", _fs)) {
				int len = f.size();
				_cine_txt = (uint8_t *)malloc(len);
				if (!_cine_txt) {
					error("Unable to allocate cinematics text data");
				}
				f.read(_cine_txt, len);
				if (f.ioErr()) {
					error("I/O error when reading '%s'", _entryName);
				}
			} else if (_aba) {
				_cine_txt = _aba->loadEntry(_entryName);
			} else if (_isDemo) {
				return; // some demos do not have cutscene datafiles
			}
		}
		if (!_cine_txt) {
			error("Cannot load '%s'", _entryName);
		}
		break;
	case kResourceTypeMac:
		MAC_loadCutsceneText();
		break;
	case kResourceTypePC98:
		break;
	}
}

void Resource::free_CINE() {
	free(_cine_off);
	_cine_off = 0;
	free(_cine_txt);
	_cine_txt = 0;
}

void Resource::load_TEXT() {
	_stringsTable = 0;
	switch (_lang) {
	case LANG_FR:
		_stringsTable = LocaleData::_stringsTableFR;
		break;
	case LANG_EN:
		_stringsTable = LocaleData::_stringsTableEN;
		break;
	case LANG_DE:
		_stringsTable = LocaleData::_stringsTableDE;
		break;
	case LANG_SP:
		_stringsTable = LocaleData::_stringsTableSP;
		break;
	case LANG_IT:
		_stringsTable = LocaleData::_stringsTableIT;
		break;
	case LANG_JP:
		_stringsTable = LocaleData::_stringsTableJP;
		break;
	}
	// Load menu strings
	_textsTable = 0;
	switch (_lang) {
	case LANG_FR:
		_textsTable = LocaleData::_textsTableFR;
		break;
	case LANG_EN:
		_textsTable = LocaleData::_textsTableEN;
		break;
	case LANG_DE:
		_textsTable = LocaleData::_textsTableDE;
		break;
	case LANG_SP:
		_textsTable = LocaleData::_textsTableSP;
		break;
	case LANG_IT:
		_textsTable = LocaleData::_textsTableIT;
		break;
	case LANG_JP:
		_textsTable = LocaleData::_textsTableEN;
		break;
	}
}

void Resource::free_TEXT() {
	_stringsTable = 0;
	_textsTable = 0;
}

void Resource::load_CreditsCrd() {
	if (!_credits) {
		static const char *kName = "CREDITS.CRD"; // 25th anniversary edition
		File f;
		if (f.open(kName, "rb", _fs)) {
			const int len = f.size();
			_credits = (uint8_t *)malloc(len);
			if (_credits) {
				f.read(_credits, len);
				if (f.ioErr()) {
					warning("I/O error when reading '%s'", kName);
					free(_credits);
					_credits = 0;
				}
			}
		}
	}
}

void Resource::free_CreditsCrd() {
	free(_credits);
	_credits = 0;
}

static const char *getTextBin(Language lang, ResourceType type) {
	// FB PC-CD version has language specific files
	// .TBN is used as fallback if open fails
	switch (lang) {
	case LANG_FR:
		return "TBF";
	case LANG_DE:
		return "TBG";
	case LANG_SP:
		return "TBS";
	case LANG_IT:
		return "TBI";
	case LANG_EN:
	default:
		return "TBN";
	}
}

void Resource::unload(int objType) {
	switch (objType) {
	case OT_CMD:
		free(_cmd);
		_cmd = 0;
		break;
	case OT_POL:
		free(_pol);
		_pol = 0;
		break;
	case OT_CMP:
		free(_cmd);
		_cmd = 0;
		free(_pol);
		_pol = 0;
		break;
	default:
		error("Unimplemented Resource::unload() type %d", objType);
		break;
	}
}

void Resource::load(const char *objName, int objType, const char *ext) {
	debug(DBG_RES, "Resource::load('%s', %d)", objName, objType);
	LoadProc loadProc = 0;
	switch (objType) {
	case OT_MBK:
		snprintf(_entryName, sizeof(_entryName), "%s.MBK", objName);
		loadProc = &Resource::load_MBK;
		break;
	case OT_PGE:
		snprintf(_entryName, sizeof(_entryName), "%s.PGE", objName);
		loadProc = &Resource::load_PGE;
		break;
	case OT_PAL:
		snprintf(_entryName, sizeof(_entryName), "%s.PAL", objName);
		loadProc = &Resource::load_PAL;
		break;
	case OT_CT:
		snprintf(_entryName, sizeof(_entryName), "%s.CT", objName);
		loadProc = &Resource::load_CT;
		break;
	case OT_MAP:
		snprintf(_entryName, sizeof(_entryName), "%s.MAP", objName);
		loadProc = &Resource::load_MAP;
		break;
	case OT_SPC:
		snprintf(_entryName, sizeof(_entryName), "%s.SPC", objName);
		loadProc = &Resource::load_SPC;
		break;
	case OT_RP:
		snprintf(_entryName, sizeof(_entryName), "%s.RP", objName);
		loadProc = &Resource::load_RP;
		break;
	case OT_RPC:
		snprintf(_entryName, sizeof(_entryName), "%s.RPC", objName);
		loadProc = &Resource::load_RP;
		break;
	case OT_SPR:
		snprintf(_entryName, sizeof(_entryName), "%s.SPR", objName);
		loadProc = &Resource::load_SPR;
		break;
	case OT_SPRM:
		snprintf(_entryName, sizeof(_entryName), "%s.SPR", objName);
		loadProc = &Resource::load_SPRM;
		break;
	case OT_ICN:
		snprintf(_entryName, sizeof(_entryName), "%s.ICN", objName);
		loadProc = &Resource::load_ICN;
		break;
	case OT_FNT:
		snprintf(_entryName, sizeof(_entryName), "%s.FNT", objName);
		loadProc = &Resource::load_FNT;
		break;
	case OT_OBJ:
		snprintf(_entryName, sizeof(_entryName), "%s.OBJ", objName);
		loadProc = &Resource::load_OBJ;
		break;
	case OT_ANI:
		snprintf(_entryName, sizeof(_entryName), "%s.ANI", objName);
		loadProc = &Resource::load_ANI;
		break;
	case OT_TBN:
		snprintf(_entryName, sizeof(_entryName), "%s.%s", objName, getTextBin(_lang, _type));
		if (!_fs->exists(_entryName)) {
			snprintf(_entryName, sizeof(_entryName), "%s.TBN", objName);
		}
		loadProc = &Resource::load_TBN;
		break;
	case OT_CMD:
		snprintf(_entryName, sizeof(_entryName), "%s.CMD", objName);
		loadProc = &Resource::load_CMD;
		break;
	case OT_POL:
		snprintf(_entryName, sizeof(_entryName), "%s.POL", objName);
		loadProc = &Resource::load_POL;
		break;
	case OT_CMP:
		snprintf(_entryName, sizeof(_entryName), "%s.CMP", objName);
		loadProc = &Resource::load_CMP;
		break;
	case OT_OBC:
		snprintf(_entryName, sizeof(_entryName), "%s.OBC", objName);
		loadProc = &Resource::load_OBC;
		break;
	case OT_SPL:
		snprintf(_entryName, sizeof(_entryName), "%s.SPL", objName);
		loadProc = &Resource::load_SPL;
		break;
	case OT_LEV:
		snprintf(_entryName, sizeof(_entryName), "%s.LEV", objName);
		loadProc = &Resource::load_LEV;
		break;
	case OT_SGD:
		snprintf(_entryName, sizeof(_entryName), "%s.SGD", objName);
		loadProc = &Resource::load_SGD;
		break;
	case OT_BNQ:
		snprintf(_entryName, sizeof(_entryName), "%s.BNQ", objName);
		loadProc = &Resource::load_BNQ;
		break;
	case OT_SPM:
		snprintf(_entryName, sizeof(_entryName), "%s.SPM", objName);
		loadProc = &Resource::load_SPM;
		break;
	default:
		error("Unimplemented Resource::load() type %d", objType);
		break;
	}
	if (ext) {
		snprintf(_entryName, sizeof(_entryName), "%s.%s", objName, ext);
	}
#ifdef ATARIST
	if (ST_loadUnpacked(objType)) {
		return;
	}
#endif
	File f;
	if (f.open(_entryName, "rb", _fs)) {
		assert(loadProc);
		(this->*loadProc)(&f);
		if (f.ioErr()) {
			error("I/O error when reading '%s'", _entryName);
		}
#ifdef ATARIST
		if (objType == OT_LEV || objType == OT_MBK) {
			f.close();
			ST_loadUnpackedResident(objType);
		}
#endif
	} else {
		if (_archive) {
			uint32_t size;
			uint8_t *dat = _archive->loadEntry(_entryName, &size);
			if (dat) {
				switch (objType) {
				case OT_MBK:
					_mbk = dat;
					break;
				case OT_PGE:
					decodePGE(dat, size);
					break;
				case OT_PAL:
					_pal = dat;
					break;
				case OT_CT:
					if (!bytekiller_unpack((uint8_t *)_ctData, sizeof(_ctData), dat, size)) {
						error("Bad CRC for '%s'", _entryName);
					}
					free(dat);
					break;
				case OT_SPC:
					free(_spc);
					_spc = dat;
					_numSpc = READ_BE_UINT16(_spc) / 2;
					break;
				case OT_RP:
					if (size != sizeof(_rp)) {
						error("Unexpected size %d for '%s'", size, _entryName);
					}
					memcpy(_rp, dat, size);
					free(dat);
					break;
				case OT_ICN:
					_icn = dat;
					break;
				case OT_FNT:
					_fnt = dat;
					break;
				case OT_OBJ:
					_numObjectNodes = READ_LE_UINT16(dat);
					assert(_numObjectNodes == NUM_OBJECTS);
					decodeOBJ(dat + 2, size - 2);
					break;
				case OT_ANI:
					_ani = dat;
					break;
				case OT_TBN:
					_tbn = dat;
					break;
				case OT_SPR:
					assert(memcmp(dat, "SPP", 3) == 0 && size > kSprHeaderSize);
					_spr1 = (uint8_t *)malloc(size - kSprHeaderSize);
					memcpy(_spr1, dat + kSprHeaderSize, size - kSprHeaderSize);
					break;
				case OT_CMD:
					_cmd = dat;
					break;
				case OT_POL:
					_pol = dat;
					break;
				case OT_SPRM:
					assert(memcmp(dat, "SPP", 3) == 0 && size > kSprHeaderSize);
					memcpy(_sprm, dat + kSprHeaderSize, size - kSprHeaderSize);
					break;
				case OT_SGD:
					_sgd = dat;
					_sgd[0] = 0; // clear number of entries, fix first offset
					break;
				case OT_BNQ:
					_bnq = dat;
					break;
				default:
					error("Cannot load '%s' type %d", _entryName, objType);
				}
				return;
			}
		} else if (_isDemo) {
			// some demos (fbdemofr.zip) do not have the cutscene datafiles included
			switch (objType) {
			case OT_CMD:
			case OT_POL:
			case OT_CMP:
				warning("Unable to load '%s' type %d", _entryName, objType);
				return;
			}
		}
		error("Cannot open '%s'", _entryName);
	}
}

void Resource::load_CT(File *pf) {
	debug(DBG_RES, "Resource::load_CT()");
	const int len = pf->size();
	uint8_t *tmp = (uint8_t *)malloc(len);
	if (!tmp) {
		error("Unable to allocate CT buffer");
	} else {
		pf->read(tmp, len);
		if (!bytekiller_unpack((uint8_t *)_ctData, sizeof(_ctData), tmp, len)) {
			error("Bad CRC for collision data");
		}
		free(tmp);
	}
}

void Resource::load_FNT(File *f) {
	debug(DBG_RES, "Resource::load_FNT()");
	const int len = f->size();
	_fnt = (uint8_t *)malloc(len);
	if (!_fnt) {
		error("Unable to allocate FNT buffer");
	} else {
		f->read(_fnt, len);
	}
}

void Resource::load_MBK(File *f) {
	debug(DBG_RES, "Resource::load_MBK()");
	const int len = f->size();
	_mbk = (uint8_t *)malloc(len);
	if (!_mbk) {
		error("Unable to allocate MBK buffer");
	} else {
		f->read(_mbk, len);
	}
}

void Resource::load_ICN(File *f) {
	debug(DBG_RES, "Resource::load_ICN()");
	const int len = f->size();
	if (_icnLen == 0) {
		_icn = (uint8_t *)malloc(len);
	} else {
		_icn = (uint8_t *)realloc(_icn, _icnLen + len);
	}
	if (!_icn) {
		error("Unable to allocate ICN buffer");
	} else {
		f->read(_icn + _icnLen, len);
	}
	_icnLen += len;
}

void Resource::load_SPR(File *f) {
	debug(DBG_RES, "Resource::load_SPR()");
	uint32_t len = f->size();
	assert(len > kSprHeaderSize);
	len -= kSprHeaderSize;
	_spr1 = (uint8_t *)malloc(len);
	if (!_spr1) {
		error("Unable to allocate SPR1 buffer");
	} else {
		f->seek(kSprHeaderSize);
		f->read(_spr1, len);
	}
}

void Resource::load_SPRM(File *f) {
	debug(DBG_RES, "Resource::load_SPRM()");
	uint32_t len = f->size();
	assert(len > kSprHeaderSize);
	len -= kSprHeaderSize;
	assert(len <= sizeof(_sprm));
	f->seek(kSprHeaderSize);
	f->read(_sprm, len);
}

void Resource::load_RP(File *f) {
	debug(DBG_RES, "Resource::load_RP()");
	f->read(_rp, sizeof(_rp));
}

void Resource::load_SPC(File *f) {
	debug(DBG_RES, "Resource::load_SPC()");
	const int len = f->size();
	free(_spc);   // the previous level's, which leaked before
	_spc = (uint8_t *)malloc(len);
	if (!_spc) {
		error("Unable to allocate SPC buffer");
	} else {
		f->read(_spc, len);
		_numSpc = READ_BE_UINT16(_spc) / 2;
	}
}

void Resource::load_PAL(File *f) {
	debug(DBG_RES, "Resource::load_PAL()");
	const int len = f->size();
	_pal = (uint8_t *)malloc(len);
	if (!_pal) {
		error("Unable to allocate PAL buffer");
	} else {
		f->read(_pal, len);
	}
}

void Resource::load_MAP(File *f) {
	debug(DBG_RES, "Resource::load_MAP()");
	const int len = f->size();
	_map = (uint8_t *)malloc(len);
	if (!_map) {
		error("Unable to allocate MAP buffer");
	} else {
		f->read(_map, len);
	}
}

void Resource::load_OBJ(File *f) {
	debug(DBG_RES, "Resource::load_OBJ()");
	const int size = f->size();
	uint8_t *buf = (uint8_t *)malloc(size);
	if (!buf) {
		error("Unable to allocate OBJ temporary buffer");
	} else {
		f->read(buf, size);
		if (_type == kResourceTypeAmiga || _type == kResourceTypeSega) {
			decodeOBJ(buf, size);
		} else {
			assert(_readUint16(buf) == NUM_OBJECTS);
			decodeOBJ(buf + 2, size - 2);
		}
		free(buf);
	}
}

void Resource::free_OBJ() {
	debug(DBG_RES, "Resource::free_OBJ()");
	ObjectNode *prevNode = 0;
	for (int i = 0; i < _numObjectNodes; ++i) {
		if (_objectNodesMap[i] != prevNode) {
			ObjectNode *curNode = _objectNodesMap[i];
			prevNode = curNode;
			free(curNode->objects);
			free(curNode);
		}
		_objectNodesMap[i] = 0;
	}
}

void Resource::load_OBC(File *f) {
	const int packedSize = f->readUint32BE();
	uint8_t *packedData = (uint8_t *)malloc(packedSize);
	if (!packedData) {
		error("Unable to allocate OBC temporary buffer 1");
	}
	f->read(packedData, packedSize);
	const int unpackedSize = READ_BE_UINT32(packedData + packedSize - 4);
	uint8_t *tmp = (uint8_t *)malloc(unpackedSize);
	if (!tmp) {
		error("Unable to allocate OBC temporary buffer 2");
	}
	if (!bytekiller_unpack(tmp, unpackedSize, packedData, packedSize)) {
		error("Bad CRC for compressed object data");
	}
	free(packedData);
	decodeOBJ(tmp, unpackedSize);
	free(tmp);
}

void Resource::decodeOBJ(const uint8_t *tmp, int size) {
	uint32_t offsets[256];
	int tmpOffset = 0;
	_numObjectNodes = NUM_OBJECTS;
	if (_type == kResourceTypeMac) {
		_numObjectNodes = _readUint16(tmp);
		/* offsets start from &tmp[0] with Macintosh, &tmp[2] with DOS */
		tmpOffset += 2;
	}
	for (int i = 0; i < _numObjectNodes; ++i) {
		offsets[i] = _readUint32(tmp + tmpOffset); tmpOffset += 4;
	}
	offsets[_numObjectNodes] = size;
	int numObjectsCount = 0;
	uint16_t objectsCount[256];
	for (int i = 0; i < _numObjectNodes; ++i) {
		int diff = offsets[i + 1] - offsets[i];
		if (diff != 0) {
			objectsCount[numObjectsCount] = (diff - 2) / 0x12;
			++numObjectsCount;
		}
	}
	uint32_t prevOffset = 0;
	ObjectNode *prevNode = 0;
	int iObj = 0;
	for (int i = 0; i < _numObjectNodes; ++i) {
		if (prevOffset != offsets[i]) {
			ObjectNode *on = (ObjectNode *)malloc(sizeof(ObjectNode));
			if (!on) {
				error("Unable to allocate ObjectNode num=%d", i);
			}
			const uint8_t *objData = tmp + offsets[i];
			on->num_objects = _readUint16(objData); objData += 2;
			assert(on->num_objects == objectsCount[iObj]);
			on->objects = (Object *)malloc(sizeof(Object) * on->num_objects);
			for (int j = 0; j < on->num_objects; ++j) {
				Object *obj = &on->objects[j];
				obj->type = _readUint16(objData); objData += 2;
				obj->dx = *objData++;
				obj->dy = *objData++;
				obj->init_obj_type = _readUint16(objData); objData += 2;
				obj->opcode2 = *objData++;
				obj->opcode1 = *objData++;
				obj->flags = *objData++;
				obj->opcode3 = *objData++;
				obj->init_obj_number = _readUint16(objData); objData += 2;
				obj->opcode_arg1 = _readUint16(objData); objData += 2;
				obj->opcode_arg2 = _readUint16(objData); objData += 2;
				obj->opcode_arg3 = _readUint16(objData); objData += 2;
				debug(DBG_RES, "obj_node=%d obj=%d op1=0x%X op2=0x%X op3=0x%X arg1=%d arg2=%d arg3=%d", i, j, obj->opcode1, obj->opcode2, obj->opcode3, obj->opcode_arg1, obj->opcode_arg2, obj->opcode_arg3);
			}
			++iObj;
			prevOffset = offsets[i];
			prevNode = on;
		}
		_objectNodesMap[i] = prevNode;
	}
}

void Resource::load_PGE(File *f) {
	debug(DBG_RES, "Resource::load_PGE()");
	const int size = f->size();
	uint8_t *tmp = (uint8_t *)malloc(size);
	if (!tmp) {
		error("Unable to allocate PGE temporary buffer");
	}
	f->read(tmp, size);
	decodePGE(tmp, size);
	free(tmp);
}

void Resource::decodePGE(const uint8_t *p, int size) {
	_pgeNum = _readUint16(p); p += 2;
	memset(_pgeInit, 0, sizeof(_pgeInit));
	debug(DBG_RES, "len=%d _pgeNum=%d", size, _pgeNum);
	assert(_pgeNum <= ARRAYSIZE(_pgeInit));
	for (uint16_t i = 0; i < _pgeNum; ++i) {
		InitPGE *pge = &_pgeInit[i];
		pge->type = _readUint16(p); p += 2;
		pge->pos_x = _readUint16(p); p += 2;
		pge->pos_y = _readUint16(p); p += 2;
		pge->obj_node_number = _readUint16(p); p += 2;
		pge->life = _readUint16(p); p += 2;
		for (int lc = 0; lc < 4; ++lc) {
			pge->data[lc] = _readUint16(p); p += 2;
		}
		pge->object_type = *p++;
		pge->init_room = *p++;
		pge->room_location = *p++;
		pge->init_flags = *p++;
		pge->colliding_icon_num = *p++;
		pge->icon_num = *p++;
		pge->object_id = *p++;
		pge->skill = *p++;
		pge->mirror_x = *p++;
		pge->flags = *p++;
		pge->collision_data_len = *p++;
		++p;
		pge->text_num = _readUint16(p); p += 2;
	}
}

void Resource::load_ANI(File *f) {
	debug(DBG_RES, "Resource::load_ANI()");
	const int size = f->size();
	_ani = (uint8_t *)malloc(size);
	if (!_ani) {
		error("Unable to allocate ANI buffer");
	} else {
		f->read(_ani, size);
	}
}

void Resource::load_TBN(File *f) {
	debug(DBG_RES, "Resource::load_TBN()");
	const int len = f->size();
	_tbn = (uint8_t *)malloc(len);
	if (!_tbn) {
		error("Unable to allocate TBN buffer");
	} else {
		f->read(_tbn, len);
	}
}

void Resource::load_CMD(File *pf) {
	debug(DBG_RES, "Resource::load_CMD()");
	free(_cmd);
	const int len = pf->size();
	_cmd = (uint8_t *)malloc(len);
	if (!_cmd) {
		error("Unable to allocate CMD buffer");
	} else {
		pf->read(_cmd, len);
	}
}

void Resource::load_POL(File *pf) {
	debug(DBG_RES, "Resource::load_POL()");
	free(_pol);
	const int len = pf->size();
	_pol = (uint8_t *)malloc(len);
	if (!_pol) {
		error("Unable to allocate POL buffer");
	} else {
		pf->read(_pol, len);
	}
}

void Resource::load_CMP(File *pf) {
	free(_pol);
	free(_cmd);
	const int len = pf->size();
	uint8_t *tmp = (uint8_t *)malloc(len);
	if (!tmp) {
		error("Unable to allocate CMP buffer");
	}
	pf->read(tmp, len);
	struct {
		int offset, packedSize, size;
	} data[2];
	int offset = 0;
	for (int i = 0; i < 2; ++i) {
		int packedSize = READ_BE_UINT32(tmp + offset); offset += 4;
		assert((packedSize & 1) == 0);
		if (packedSize < 0) {
			data[i].size = packedSize = -packedSize;
		} else {
			data[i].size = READ_BE_UINT32(tmp + offset + packedSize - 4);
		}
		data[i].offset = offset;
		data[i].packedSize = packedSize;
		offset += packedSize;
	}
	_pol = (uint8_t *)malloc(data[0].size);
	if (!_pol) {
		error("Unable to allocate POL buffer");
	}
	if (data[0].packedSize == data[0].size) {
		memcpy(_pol, tmp + data[0].offset, data[0].packedSize);
	} else if (!bytekiller_unpack(_pol, data[0].size, tmp + data[0].offset, data[0].packedSize)) {
		error("Bad CRC for cutscene polygon data");
	}
	_cmd = (uint8_t *)malloc(data[1].size);
	if (!_cmd) {
		error("Unable to allocate CMD buffer");
	}
	if (data[1].packedSize == data[1].size) {
		memcpy(_cmd, tmp + data[1].offset, data[1].packedSize);
	} else if (!bytekiller_unpack(_cmd, data[1].size, tmp + data[1].offset, data[1].packedSize)) {
		error("Bad CRC for cutscene command data");
	}
	free(tmp);
}

void Resource::load_VCE(int num, int segment, uint8_t **buf, uint32_t *bufSize) {
	*buf = 0;
	int offset = _voicesOffsetsTable[num];
	if (offset != 0xFFFF) {
		const uint16_t *p = _voicesOffsetsTable + offset / 2;
		offset = (*p++) * 2048;
		int count = *p++;
		if (segment < count) {
			File f;
			if (f.open("VOICE.VCE", "rb", _fs)) {
				int voiceSize = p[segment] * 2048 / 5;
				uint8_t *voiceBuf = (uint8_t *)malloc(voiceSize);
				if (voiceBuf) {
					uint8_t *dst = voiceBuf;
					offset += 0x2000;
					for (int s = 0; s < count; ++s) {
						int len = p[s] * 2048;
						for (int i = 0; i < len / (0x2000 + 2048); ++i) {
							if (s == segment) {
								f.seek(offset);
								int n = 2048;
								while (n--) {
									int v = f.readByte();
									if (v & 0x80) {
										v = -(v & 0x7F);
									}
									*dst++ = (uint8_t)(v & 0xFF);
								}
							}
							offset += 0x2000 + 2048;
						}
						if (s == segment) {
							break;
						}
					}
					*buf = voiceBuf;
					*bufSize = voiceSize;
				}
			}
		}
	}
}

void Resource::load_SPL(File *f) {
	for (int i = 0; i < _numSfx; ++i) {
		free(_sfxList[i].data);
	}
	free(_sfxList);
	_numSfx = NUM_SFXS;
	_sfxList = (SoundFx *)calloc(_numSfx, sizeof(SoundFx));
	if (!_sfxList) {
		error("Unable to allocate SoundFx table");
	}
	int offset = 0;
	for (int i = 0; i < _numSfx; ++i) {
		const int size = f->readUint16BE(); offset += 2;
		if ((size & 0x8000) != 0) {
			continue;
		}
		debug(DBG_RES, "sfx=%d size=%d", i, size);
		assert(size != 0 && (size & 1) == 0);
		if (i == 64) {
			warning("Skipping sound #%d (%s) size %d", i, _splNames[i], size);
			f->seek(offset + size);
		}  else {
			_sfxList[i].offset = offset;
			_sfxList[i].freq = kPaulaFreq / 650;
			_sfxList[i].data = (uint8_t *)malloc(size);
			if (_sfxList[i].data) {
				f->read(_sfxList[i].data, size);
				_sfxList[i].len = size;
				normalizeSPL(&_sfxList[i]);
			}
		}
		offset += size;
	}
}

void Resource::load_LEV(File *f) {
	const int len = f->size();
	// level 2 loads its other half over the first, and the first was
	// never given back: 33-39K lost each time Conrad crossed between them
	free(_lev);
	_lev = (uint8_t *)malloc(len);
	if (!_lev) {
		error("Unable to allocate LEV buffer");
	} else {
		f->read(_lev, len);
	}
}

void Resource::load_SGD(File *f) {
	const int len = f->size();
	if (_type == kResourceTypeDOS || _type == kResourceTypeSega) {
		_sgd = (uint8_t *)malloc(len);
		if (!_sgd) {
			error("Unable to allocate SGD buffer");
		} else {
			f->read(_sgd, len);
			// first byte == number of entries, clear to fix up 32 bits offset
			_sgd[0] = 0;
		}
		return;
	}
	f->seek(len - 4);
	int size = f->readUint32BE();
	f->seek(0);
	uint8_t *tmp = (uint8_t *)malloc(len);
	if (!tmp) {
		error("Unable to allocate SGD temporary buffer");
	}
	f->read(tmp, len);
	_sgd = (uint8_t *)malloc(size);
	if (!_sgd) {
		error("Unable to allocate SGD buffer");
	}
	if (!bytekiller_unpack(_sgd, size, tmp, len)) {
		error("Bad CRC for SGD data");
	}
	free(tmp);
}

void Resource::load_BNQ(File *f) {
	const int len = f->size();
	_bnq = (uint8_t *)malloc(len);
	if (!_bnq) {
		error("Unable to allocate BNQ buffer");
	} else {
		f->read(_bnq, len);
	}
}

static const uint32_t kPersoDatSize = 178647;

// The sprite table: Conrad's from PERSO's own buffer, the monsters'
// from the shared one
void Resource::setSprData() {
	for (int i = 0; i < NUM_SPRITES; ++i) {
		const uint32_t offset = _spmOffsetsTable[i];
		if (offset >= kPersoDatSize) {
			_sprData[i] = _sprm + offset - kPersoDatSize;
		} else {
			_sprData[i] = _spr1 + offset;
		}
	}
}

void Resource::load_SPM(File *f) {
	const int len = f->size();
	f->seek(len - 4);
	const uint32_t size = f->readUint32BE();
	f->seek(0);
	// The sprites before the packed copy, so that freeing the copy
	// leaves its hole at the top of the heap rather than under the
	// sprites: loaded behind the Atari's start-up splash, the hole
	// under them cost the title 40K of its largest free block.
	if (size == kPersoDatSize) {
		_spr1 = (uint8_t *)malloc(size);
		if (!_spr1) {
			error("Unable to allocate SPR1 buffer");
		}
	}
	uint8_t *tmp = (uint8_t *)malloc(len);
	if (!tmp) {
		error("Unable to allocate SPM temporary buffer");
	}
	f->read(tmp, len);
	if (size == kPersoDatSize) {
		if (!bytekiller_unpack(_spr1, size, tmp, len)) {
			error("Bad CRC for SPM data");
		}
	} else {
		assert(size <= sizeof(_sprm));
		if (!bytekiller_unpack(_sprm, sizeof(_sprm), tmp, len)) {
			error("Bad CRC for SPM data");
		}
#ifdef ATARIST
		_stSprmSize = size;
		++_stMonsterEpoch;
#endif
	}
	setSprData();
	free(tmp);
}

#ifdef ATARIST
/*
 * UNPACKED\, beside DATA\: the data's bytekiller streams already
 * unpacked, as RExtract and tools/make-unpacked.py write them. They are
 * read straight into place - no unpacking, which cost an 8MHz ST 3.6s
 * for Conrad's sprites, 1.2s for the intro and 2.3s for level 1, and
 * no packed copy held beside the result. Optional and derived, so
 * kept out of DATA\ like MUSIC\; anything missing or odd here falls
 * back to DATA\ as before.
 *
 * A file keeps its DATA\ name and holds "RXU1", a BE32 stream count,
 * a BE32 size per stream, then the streams as the unpacker would
 * write them: two for a cutscene (polygons, then commands), one for
 * everything else.
 */
static int g_stUnpacked = -1;        // the folder is there: -1 not looked yet

static bool ST_haveUnpacked() {
	if (g_stUnpacked < 0) {
		_DTA dta;
		_DTA *saved = Fgetdta();
		Fsetdta(&dta);
		g_stUnpacked = (Fsfirst("UNPACKED\\*.*", 0) == 0) ? 1 : 0;
		Fsetdta(saved);
		if (g_stUnpacked) {
			info("Unpacked data in UNPACKED\\");
		}
	}
	return g_stUnpacked != 0;
}

bool Resource::ST_openUnpacked(File *f, const char *name, uint32_t *sizes, int count) {
	if (!ST_haveUnpacked() || !f->open(name, "rb", "UNPACKED")) {
		return false;
	}
	const uint32_t len = f->size();
	uint32_t total = 8 + 4 * count;
	if (len >= total && f->readUint32BE() == 0x52585531 && f->readUint32BE() == (uint32_t)count) {
		for (int i = 0; i < count; ++i) {
			sizes[i] = f->readUint32BE();
			total += sizes[i];
		}
		if (total == len && !f->ioErr()) {
			return true;
		}
	}
	warning("UNPACKED\\%s is not what it should be, so DATA\\%s is unpacked instead", name, name);
	f->close();
	return false;
}

// Room data and banks unpacked ahead of time. The game keeps both whole
// for a level and unpacks a room's data, and every bank its tiles and
// objects use, as Conrad enters it: a third of a second or more on an
// 8MHz ST, every first visit. Read from UNPACKED\ instead, beside
// DATA\'s packed copies (still read for which rooms exist and how big
// each bank is):
//  - the banks into the bank arena itself, where it is big enough for
//    all of a level's (34-125K in the 384K a 4MB machine has; never in
//    a 2.5MB machine's 28K), so they cost no RAM at all;
//  - the room data, 71-143K a level, on a machine with 3.5MB or more,
//    where 1MB stays free after it.
// A file holds a stream per room or per bank; anything missing, odd or
// out of room and the game unpacks as before. With dst, the streams go
// there if they fit in dstSize; without, into a block of their own.
bool Resource::ST_readUnpackedTable(const char *name, int maxCount, uint8_t *dst, uint32_t dstSize, uint8_t **data, uint32_t *offs, uint32_t *sizes, int *count) {
	enum { kMinRamK = 3584, kSpareK = 1024 };
	extern int ATARIST_ramK();
	if ((!dst && ATARIST_ramK() < kMinRamK) || !ST_haveUnpacked()) {
		return false;
	}
	File f;
	if (!f.open(name, "rb", "UNPACKED")) {
		return false;
	}
	const uint32_t len = f.size();
	if (len < 8 || f.readUint32BE() != 0x52585531) {
		warning("UNPACKED\\%s is not what it should be, so DATA\\%s is unpacked instead", name, name);
		return false;
	}
	const uint32_t n = f.readUint32BE();
	if (n == 0 || n > (uint32_t)maxCount) {
		warning("UNPACKED\\%s is not what it should be, so DATA\\%s is unpacked instead", name, name);
		return false;
	}
	uint32_t total = 0;
	for (uint32_t i = 0; i < n; ++i) {
		sizes[i] = f.readUint32BE();
		offs[i] = total;
		total += sizes[i];
	}
	if (f.ioErr() || 8 + 4 * n + total != len) {
		warning("UNPACKED\\%s is not what it should be, so DATA\\%s is unpacked instead", name, name);
		return false;
	}
	uint8_t *buf = dst;
	if (dst) {
		if (total > dstSize) {
			return false;
		}
	} else {
		if (((long)Malloc(-1) >> 10) < (long)(total >> 10) + kSpareK) {
			return false;                    // a machine without the RAM unpacks as it goes
		}
		buf = (uint8_t *)malloc(total + 1);
		if (!buf) {
			return false;
		}
	}
	f.read(buf, total);
	if (f.ioErr()) {
		if (!dst) {
			free(buf);
		}
		return false;
	}
	*data = buf;
	*count = (int)n;
	return true;
}

void Resource::ST_loadUnpackedResident(int objType) {
	if (_type != kResourceTypeAmiga) {
		return;
	}
	if (objType == OT_LEV) {
		free(_stLevU);
		_stLevU = 0;
		int n;
		if (ST_readUnpackedTable(_entryName, 64, 0, 0, &_stLevU, _stLevUOff, _stLevUSize, &n) && n != 64) {
			free(_stLevU);
			_stLevU = 0;
		}
	} else {
		// the arena is about to be written over from its start
		_stMbkU = 0;
		_stMbkUCount = 0;
		_stMbkUEnd = 0;
		clearBankData();
		int n;
		uint8_t *banks;
		if (!ST_readUnpackedTable(_entryName, 256, _bankData, _bankDataTail - _bankData, &banks, _stMbkUOff, _stMbkUSize, &n)) {
			return;
		}
		// every bank as big as the packed table says, empty ones included,
		// or none of them: then no bank ever needs the arena beside them
		for (int i = 0; i < n; ++i) {
			if ((int)_stMbkUSize[i] != getBankDataSize(i)) {
				warning("UNPACKED\\%s does not match DATA\\%s, so the banks are unpacked as needed", _entryName, _entryName);
				return;
			}
		}
		_stMbkU = banks;
		_stMbkUCount = n;
		_stMbkUEnd = _stMbkUOff[n - 1] + _stMbkUSize[n - 1];
		clearBankData();                     // anything else goes after them
	}
}

// The sprite table's four monster ranges, one per SPM file in
// Game::_monsterNames order: runs of offsets past PERSO's that start
// again from the beginning of the shared buffer.
static int16_t g_monsterRange[4][2];

static void findMonsterRanges(const uint32_t *offsets) {
	if (g_monsterRange[0][1] != 0) {
		return;
	}
	int m = -1;
	bool inRun = false;
	uint32_t prev = 0;
	for (int i = 0; i < Resource::NUM_SPRITES; ++i) {
		const uint32_t offset = offsets[i];
		if (offset < kPersoDatSize) {
			inRun = false;
			continue;
		}
		if (!inRun || offset < prev) {           // the next file's sprites
			if (m == 3) {
				break;
			}
			++m;
			g_monsterRange[m][0] = i;
		}
		g_monsterRange[m][1] = i;
		inRun = true;
		prev = offset;
	}
}

// Copy the monster just loaded into _sprm to a buffer of its own, if
// the machine has the room: up to 61K a file, three at most on a
// level. A machine without it keeps sharing _sprm, as before.
bool Resource::ST_keepMonster(int num) {
	enum { kSpareK = 320 };     // left for music, cutscenes and the rest
	if (_stSprmSize == 0 || (Malloc(-1) >> 10) < (long)(_stSprmSize >> 10) + kSpareK) {
		return false;
	}
	uint8_t *buf = (uint8_t *)malloc(_stSprmSize);
	if (!buf) {
		return false;
	}
	memcpy(buf, _sprm, _stSprmSize);
	_stMonsterBuf[num] = buf;
	return true;
}

// Point every kept monster's sprites at its own buffer: a load into
// _sprm points all four ranges there.
void Resource::ST_pointMonsters() {
	findMonsterRanges(_spmOffsetsTable);
	for (int m = 0; m < 4; ++m) {
		uint8_t *buf = _stMonsterBuf[m];
		if (!buf) {
			continue;
		}
		for (int i = g_monsterRange[m][0]; i <= g_monsterRange[m][1]; ++i) {
			_sprData[i] = buf + _spmOffsetsTable[i] - kPersoDatSize;
		}
	}
}

void Resource::ST_freeMonsters() {
	for (int m = 0; m < 4; ++m) {
		free(_stMonsterBuf[m]);
		_stMonsterBuf[m] = 0;
	}
	++_stMonsterEpoch;
}

// The types that have a copy in UNPACKED\, loaded from it as their
// load_* functions would leave them; false to load DATA\ instead
bool Resource::ST_loadUnpacked(int objType) {
	if (_type != kResourceTypeAmiga
	    || (objType != OT_SPM && objType != OT_SGD && objType != OT_OBC && objType != OT_CT && objType != OT_CMP)) {
		return false;
	}
	File f;
	uint32_t sizes[2];
	if (!ST_openUnpacked(&f, _entryName, sizes, (objType == OT_CMP) ? 2 : 1)) {
		return false;
	}
	switch (objType) {
	case OT_SPM:
		if (sizes[0] == kPersoDatSize) {
			_spr1 = (uint8_t *)malloc(sizes[0]);
			if (!_spr1) {
				error("Unable to allocate SPR1 buffer");
			}
			f.read(_spr1, sizes[0]);
		} else {
			if (sizes[0] > sizeof(_sprm)) {
				return false;
			}
			f.read(_sprm, sizes[0]);
			_stSprmSize = sizes[0];
			++_stMonsterEpoch;
		}
		setSprData();
		break;
	case OT_SGD:
		_sgd = (uint8_t *)malloc(sizes[0]);
		if (!_sgd) {
			error("Unable to allocate SGD buffer");
		}
		f.read(_sgd, sizes[0]);
		break;
	case OT_OBC: {
			uint8_t *tmp = (uint8_t *)malloc(sizes[0]);
			if (!tmp) {
				error("Unable to allocate OBC temporary buffer 2");
			}
			f.read(tmp, sizes[0]);
			decodeOBJ(tmp, sizes[0]);
			free(tmp);
		}
		break;
	case OT_CT:
		if (sizes[0] > sizeof(_ctData)) {
			return false;
		}
		f.read(_ctData, sizes[0]);
		break;
	case OT_CMP:
		free(_pol);
		free(_cmd);
		_pol = (uint8_t *)malloc(sizes[0]);
		_cmd = (uint8_t *)malloc(sizes[1]);
		if (!_pol || !_cmd) {
			error("Unable to allocate cutscene buffers");
		}
		f.read(_pol, sizes[0]);
		f.read(_cmd, sizes[1]);
		break;
	}
	if (f.ioErr()) {
		error("I/O error when reading 'UNPACKED\\%s'", _entryName);
	}
	return true;
}
#endif

void Resource::clearBankData() {
#ifdef ATARIST
	// a frame's display list names pieces in the arena by address: what
	// it has written down is drawn now, while those bytes are still theirs
	ST_dlFlushNow();
	// every pointer into the arena is about to mean something else, and
	// the sprite bakes key on such pointers (spritecache_st.cpp)
	++_stBankEpoch;
	++g_stPerf.bankClears;
#endif
	_bankBuffersCount = 0;
	_bankDataHead = _bankData;
#ifdef ATARIST
	_bankDataHead += _stMbkUEnd;              // the level's banks, unpacked ahead, stay
#endif
	memset(_bankPtr, 0, sizeof(_bankPtr));
}

int Resource::getBankDataSize(uint16_t num) const {
	int len = READ_BE_UINT16(_mbk + num * 6 + 4);
	switch (_type) {
	case kResourceTypeAmiga:
		if (len & 0x8000) {
			len = -(int16_t)len;
		}
		break;
	case kResourceTypeDOS:
	case kResourceTypePC98:
		if (len & 0x8000) {
			if (_mbk == _bnq) { // demo .bnq use signed int
				len = -(int16_t)len;
				break;
			}
			len &= 0x7FFF;
		}
		break;
	case kResourceTypeMac:
		assert(0); // different graphics format
		break;
	case kResourceTypeSega:
		assert((len & 0x8000) == 0);
		break;
	}
	return len * 32;
}

uint8_t *Resource::findBankData(uint16_t num) {
	// A table by entry number: this runs for every object piece drawn,
	// and the scan of the loaded banks it replaces cost 1.1ms a frame on
	// an 8MHz ST in a room full of objects, where several banks
	// alternate and a one-entry memo kept missing.
	if (num < ARRAYSIZE(_bankPtr)) {
		return _bankPtr[num];
	}
	for (int i = 0; i < _bankBuffersCount; ++i) {
		if (_bankBuffers[i].entryNum == num) {
			return _bankBuffers[i].ptr;
		}
	}
	return 0;
}

uint8_t *Resource::loadBankData(uint16_t num) {
#ifdef ATARIST
	if (num < _stMbkUCount && _stMbkUSize[num] != 0) {
		// unpacked ahead of time and kept for the level at the start of
		// the arena: no unpacking, and the same address all level long
		uint8_t *bank = _stMbkU + _stMbkUOff[num];
		if (num < ARRAYSIZE(_bankPtr)) {
			_bankPtr[num] = bank;
		}
		return bank;
	}
#endif
	const uint8_t *ptr = _mbk + num * 6;
	int dataOffset = READ_BE_UINT32(ptr);
	if (_type == kResourceTypeDOS) {
		// first byte of the data buffer corresponds
		// to the total count of entries
		dataOffset &= 0xFFFF;
	}
	const int size = getBankDataSize(num);
	if (size == 0) {
		warning("Invalid bank data %d", num);
#ifdef ATARIST
		// upstream hands back the arena's free bytes, which the next load
		// fills: whatever keyed on this address (tile cells, sprite
		// bakes) must not survive that
		++_stBankEpoch;
#endif
		return _bankDataHead;
	}
	const int avail = _bankDataTail - _bankDataHead;
	if (avail < size) {
		clearBankData();
	}
	assert(_bankDataHead + size <= _bankDataTail);
	if (num < ARRAYSIZE(_bankPtr)) {
		// the table holds it whether or not the list below has room: a
		// bank the list dropped was unpacked again on every draw
		_bankPtr[num] = _bankDataHead;
	} else if (_bankBuffersCount < (int)ARRAYSIZE(_bankBuffers)) {
		_bankBuffers[_bankBuffersCount].entryNum = num;
		_bankBuffers[_bankBuffersCount].ptr = _bankDataHead;
		++_bankBuffersCount;
	}
	const uint8_t *data = _mbk + dataOffset;
	const int count = READ_BE_UINT16(ptr + 4);
#ifdef ATARIST
	++g_stPerf.bankLoads;
	g_stPerf.bankBytes += size;
#endif
	if (count & 0x8000) {
		memcpy(_bankDataHead, data, size);
	} else {
		assert(dataOffset > 4);
		assert(size == (int)READ_BE_UINT32(data - 4));
		if (!bytekiller_unpack(_bankDataHead, _bankDataTail - _bankDataHead, data, 0)) {
			error("Bad CRC for bank data %d", num);
		} else if (count * 32 != size) {
			warning("Unexpected decompressed size %d for bank data, count %d", size, count);
		}
	}
	uint8_t *bankData = _bankDataHead;
	_bankDataHead += size;
	return bankData;
}

void Resource::PC98_loadLevelMap(int level) {
	char name[32];
	snprintf(name, sizeof(name), "LEVEL%d.PAQ", level + 1);
	File f;
	if (f.open(name, "rb", _fs)) {
		const int size = f.size();
		_map = (uint8_t *)malloc(size);
		if (_map) {
			f.read(_map, size);
			debug(DBG_RES, "Loaded MAP file for PC98, %d bytes", size);
		}
	}
}

void Resource::PC98_loadSounds() {
	File f;
	if (f.open("SFX.PAQ", "rb", _fs)) {
		_numSfx = NUM_SFXS;
		_sfxList = (SoundFx *)calloc(_numSfx, sizeof(SoundFx));
		if (!_sfxList) {
			error("Unable to allocate SoundFx table");
		}
		int current = 0;
		int count = 0;
		do {
			const uint32_t offset = f.readUint32LE();
			if (count == 0) {
				assert((count % 12) == 0);
				count = offset / 12;
				assert(count == NUM_SFXS);
			}
			const uint32_t compressed   = f.readUint32LE();
			const uint32_t uncompressed = f.readUint32LE();
			assert(compressed == uncompressed);
			SoundFx *sfx = &_sfxList[current++];
			sfx->offset = offset;
			sfx->len = uncompressed;
			sfx->freq = 6000;
		} while (--count != 0);
		for (int i = 0; i < _numSfx; ++i) {
			SoundFx *sfx = &_sfxList[i];
			if (sfx->len == 0) {
				continue;
			}
			decodeSfxFibonacci(sfx, f);
		}
	}
}

uint8_t *Resource::decodeResourceMacText(const char *name, const char *suffix) {
	char buf[256];
	snprintf(buf, sizeof(buf), "%s %s", name, suffix);
	const ResourceMacEntry *entry = _mac->findEntry(buf, _mac->_strsIndex);
	if (entry) {
		return decodeResourceMacData(entry, false);
	} else { // CD version
		if (strcmp(name, "Flashback") == 0) {
			name = "Game";
		}
		const char *language = (_lang == LANG_FR) ? "French" : "English";
		snprintf(buf, sizeof(buf), "%s %s %s", name, suffix, language);
		return decodeResourceMacData(buf, false, _mac->_strsIndex);
	}
}

uint8_t *Resource::decodeResourceMacData(const char *name, bool decompressLzss, int type) {
	uint8_t *data = 0;
	const ResourceMacEntry *entry = _mac->findEntry(name, type);
	if (entry) {
		data = decodeResourceMacData(entry, decompressLzss);
	} else {
		_resourceMacDataSize = 0;
		error("Resource '%s' not found", name);
	}
	return data;
}

uint8_t *Resource::decodeResourceMacData(const ResourceMacEntry *entry, bool decompressLzss) {
	assert(entry);
	_mac->_f.seek(_mac->_dataOffset + entry->dataOffset);
	_resourceMacDataSize = _mac->_f.readUint32BE();
	uint8_t *data = 0;
	if (decompressLzss) {
		data = decodeLzss(_mac->_f, _resourceMacDataSize);
		if (!data) {
			error("Failed to decompress '%s'", entry->name);
		}
	} else {
		data = (uint8_t *)malloc(_resourceMacDataSize);
		if (!data) {
			error("Failed to allocate %d bytes for '%s'", _resourceMacDataSize, entry->name);
		} else {
			_mac->_f.read(data, _resourceMacDataSize);
		}
	}
	return data;
}

void Resource::MAC_decodeImageData(const uint8_t *ptr, int i, DecodeBuffer *dst) {
	const uint8_t *basePtr = ptr;
	const uint16_t sig = READ_BE_UINT16(ptr); ptr += 2;
	assert(sig == 0xC211 || sig == 0xC103);
	const int count = READ_BE_UINT16(ptr); ptr += 2;
	assert(i < count);
	ptr += 4;
	const uint32_t offset = READ_BE_UINT32(ptr + i * 4);
	if (offset != 0) {
		ptr = basePtr + offset;
		const int w = READ_BE_UINT16(ptr); ptr += 2;
		const int h = READ_BE_UINT16(ptr); ptr += 2;

		dst->orig_w = w;
		dst->orig_h = h;
		dst->clip_x = 0;
		dst->clip_y = 0;
		dst->clip_w = w;
		dst->clip_h = h;

		bool clipping = false;
		if (dst->dst_x < 0) {
			dst->clip_w += dst->dst_x;
			dst->clip_x = -dst->dst_x;
			dst->dst_x = 0;
			clipping = true;
		}
		if (dst->dst_y < 0) {
			dst->clip_h += dst->dst_y;
			dst->clip_y = -dst->dst_y;
			dst->dst_y = 0;
			clipping = true;
		}
		if (dst->dst_x + dst->clip_w > dst->dst_w) {
			dst->clip_w = dst->dst_w - dst->dst_x;
			clipping = true;
		}
		if (dst->dst_y + dst->clip_h > dst->dst_h) {
			dst->clip_h = dst->dst_h - dst->dst_y;
			clipping = true;
		}

		if (clipping || !dst->ptr) {
			const int size = w * h;
			if (size > kScratchBufferSize) {
				warning("Need %d bytes (current %d) for clipping image buffer", size, kScratchBufferSize);
				return;
			}
			dst->clip_buf = _scratchBuffer;
			memset(dst->clip_buf, 0, size);
		}
		switch (sig) {
		case 0xC211:
			decodeC211(ptr + 4, w, h, dst);
			break;
		case 0xC103:
			decodeC103(ptr, w, h, dst);
			break;
		}
	}
}

void Resource::MAC_decodeDataCLUT(const uint8_t *ptr) {
	ptr += 6; // seed+flags
	_clutSize = READ_BE_UINT16(ptr); ptr += 2;
	assert(_clutSize < kClutSize);
	for (int i = 0; i < _clutSize; ++i) {
		const int index = READ_BE_UINT16(ptr); ptr += 2;
		assert(i == index);
		// ignore lower bits
		_clut[i].r = ptr[0]; ptr += 2;
		_clut[i].g = ptr[0]; ptr += 2;
		_clut[i].b = ptr[0]; ptr += 2;
	}
}

void Resource::MAC_loadClutData() {
	uint8_t *ptr = decodeResourceMacData("Flashback colors", false);
	MAC_decodeDataCLUT(ptr);
	free(ptr);
}

void Resource::MAC_loadFontData() {
	_fnt = decodeResourceMacData("Font", true, _mac->_ppssIndex);
}

void Resource::MAC_loadIconData() {
	_icn = decodeResourceMacData("Icons", true, _mac->_ppssIndex);
}

void Resource::MAC_loadPersoData() {
	_perso = decodeResourceMacData("Person", true, _mac->_ppssIndex);
}

void Resource::MAC_loadMonsterData(const char *name, Color *clut, int level) {
	static const struct {
		const char *id;
		const char *name;
		uint8_t pal_index;
	} data[] = {
		{ "junky", "Junky", 0x32 },
		{ "mercenai", "Mercenary", 0x34 },
		{ "replican", "Replicant", 0x35 },
		{ "glue", "Alien", 0x36 },
		{ 0, 0, 0 }
	};
	free(_monster);
	_monster = 0;
	for (int i = 0; data[i].id; ++i) {
		if (strcmp(data[i].id, name) == 0) {
			_monster = decodeResourceMacData(data[i].name, true, _mac->_ppssIndex);
			assert(_monster);
			MAC_copyClut16(clut, 0, (i == 0 && level == 1) ? 0x33 : data[i].pal_index);
			break;
		}
	}
}

void Resource::MAC_loadTitleImage(int i, DecodeBuffer *buf) {
	char name[64];
	snprintf(name, sizeof(name), "Title %d", i);
	uint8_t *ptr = decodeResourceMacData(name, (i == 6), _mac->_ppssIndex);
	if (ptr) {
		MAC_decodeImageData(ptr, 0, buf);
		free(ptr);
	}
}

void Resource::MAC_unloadLevelData() {
	free(_ani);
	_ani = 0;
	ObjectNode *prevNode = 0;
	for (int i = 0; i < _numObjectNodes; ++i) {
		if (prevNode != _objectNodesMap[i]) {
			free(_objectNodesMap[i]);
			prevNode = _objectNodesMap[i];
		}
	}
	_numObjectNodes = 0;
	free(_tbn);
	_tbn = 0;
	free(_str);
	_str = 0;
}

static const uint8_t _macLevelColorOffsets[] = { 24, 28, 36, 40, 44 }; // red palette: 32
static const char *const _macLevelNumbers[] = { "1", "2", "3", "4-1", "4-2", "5-1", "5-2" };

void Resource::MAC_loadLevelData(int level) {
	char name[64];

	// .PGE
	snprintf(name, sizeof(name), "Level %s objects", _macLevelNumbers[level]);
	uint8_t *ptr = decodeResourceMacData(name, true, _mac->_objdIndex);
	decodePGE(ptr, _resourceMacDataSize);
	free(ptr);

	// .ANI
	snprintf(name, sizeof(name), "Level %s sequences", _macLevelNumbers[level]);
	_ani = decodeResourceMacData(name, true, _mac->_animIndex);
	assert(READ_BE_UINT16(_ani) == 0x48D);

	// .OBJ
	snprintf(name, sizeof(name), "Level %s conditions", _macLevelNumbers[level]);
	ptr = decodeResourceMacData(name, true, _mac->_condIndex);
	assert(READ_BE_UINT16(ptr) == NUM_OBJECTS);
	decodeOBJ(ptr, _resourceMacDataSize);
	free(ptr);

	// .CT
	snprintf(name, sizeof(name), "Level %c map", _macLevelNumbers[level][0]);
	ptr = decodeResourceMacData(name, true, _mac->_lmapIndex);
	assert(_resourceMacDataSize == 0x1D00);
	memcpy(_ctData, ptr, _resourceMacDataSize);
	free(ptr);

	// .SPC
	snprintf(name, sizeof(name), "Objects %c", _macLevelNumbers[level][0]);
	_spc = decodeResourceMacData(name, true, _mac->_ppssIndex);

	// .TBN
	snprintf(name, sizeof(name), "Level %s", _macLevelNumbers[level]);
	_tbn = decodeResourceMacText(name, "names");

	_str = decodeResourceMacText("Flashback", "strings");
}

void Resource::MAC_loadLevelRoom(int level, int i, DecodeBuffer *dst) {
	char name[64];
	snprintf(name, sizeof(name), "Level %c Room %d", _macLevelNumbers[level][0], i);
	uint8_t *ptr = decodeResourceMacData(name, true,  _mac->_ppssIndex);
	MAC_decodeImageData(ptr, 0, dst);
	free(ptr);
}

void Resource::MAC_clearClut16(Color *clut, uint8_t dest) {
	memset(&clut[dest * 16], 0, 16 * sizeof(Color));
}

void Resource::MAC_copyClut16(Color *clut, uint8_t dest, uint8_t src) {
	assert(src * 16 < _clutSize);
	memcpy(&clut[dest * 16], &_clut[src * 16], 16 * sizeof(Color));
}

void Resource::MAC_setupRoomClut(int level, int room, Color *clut) {
	const int num = _macLevelNumbers[level][0] - '1';
	int offset = _macLevelColorOffsets[num];
	int conradPal = 0x30;
	if (level == 1) {
		switch (room) {
		case 27:
		case 28:
		case 29:
		case 30:
		case 35:
		case 36:
		case 37:
		case 45:
		case 46:
			offset = 32;
			conradPal = 0x31;
			break;
		}
	}
	for (int i = 0; i < 4; ++i) {
		MAC_copyClut16(clut, i, offset + i);
		MAC_copyClut16(clut, 8 + i, offset + i);
	}
	MAC_copyClut16(clut, 4, conradPal);
	// 5 is monster palette
	MAC_clearClut16(clut, 6);
	MAC_copyClut16(clut, 0xA, _macLevelColorOffsets[0] + 2);
	MAC_copyClut16(clut, 0xC, 0x37);
	MAC_copyClut16(clut, 0xD, 0x38);
}

const uint8_t *Resource::MAC_getImageData(const uint8_t *ptr, int i) {
	const uint8_t *basePtr = ptr;
	const uint16_t sig = READ_BE_UINT16(ptr); ptr += 2;
	assert(sig == 0xC211);
	const int count = READ_BE_UINT16(ptr); ptr += 2;
	assert(i < count);
	ptr += 4;
	const uint32_t offset = READ_BE_UINT32(ptr + i * 4);
	return (offset != 0) ? basePtr + offset : 0;
}

bool Resource::MAC_hasLevelMap(int level, int room) const {
	char name[64];
	snprintf(name, sizeof(name), "Level %c Room %d", _macLevelNumbers[level][0], room);
	return _mac->findEntry(name, _mac->_ppssIndex) != 0;
}

static void stringLowerCase(char *p) {
	while (*p) {
		if (*p >= 'A' && *p <= 'Z') {
			*p += 'a' - 'A';
		}
		++p;
	}
}

void Resource::MAC_unloadCutscene() {
	free(_cmd);
	_cmd = 0;
	free(_pol);
	_pol = 0;
}

void Resource::MAC_loadCutscene(const char *cutscene) {
	MAC_unloadCutscene();
	char name[32];

	snprintf(name, sizeof(name), "%s movie", cutscene);
	stringLowerCase(name);
	const ResourceMacEntry *cmdEntry = _mac->findEntry(name, _mac->_pmovIndex);
	if (!cmdEntry) {
		return;
	}
	_cmd = decodeResourceMacData(cmdEntry, true);

	snprintf(name, sizeof(name), "%s polygons", cutscene);
	stringLowerCase(name);
	const ResourceMacEntry *polEntry = _mac->findEntry(name, _mac->_polyIndex);
	if (!polEntry) {
		return;
	}
	_pol = decodeResourceMacData(polEntry, true);
}

void Resource::MAC_loadCutsceneText() {
	_cine_txt = decodeResourceMacText("Movie", "strings");
	_cine_off = 0; // offsets are prepended to _cine_txt
}

void Resource::MAC_loadCreditsText() {
	if (!_credits) {
		_credits = decodeResourceMacText("Credit", "strings");
	}
}

void Resource::MAC_loadSounds() {
	static const int8_t table[NUM_SFXS] = {
		 0, -1,  1,  2,  3,  4, -1,  5,  6,  7,  8,  9, 10, 11, -1, 12,
		13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, 26, 27,
		28, -1, 29, -1, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
		42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, -1, 53, 54, 55, 56,
		-1, 57
	};
	_numSfx = NUM_SFXS;
	_sfxList = (SoundFx *)calloc(_numSfx, sizeof(SoundFx));
	if (!_sfxList) {
		return;
	}
	static const int kHeaderSize = 0x24;
	const int soundType = _mac->_sndIndex;
	assert(soundType != -1);
	static const int kGain = 2;
	for (int i = 0; i < NUM_SFXS; ++i) {
		const int num = table[i];
		if (num != -1) {
			const ResourceMacEntry *entry = _mac->getEntry(soundType, num);
			_mac->_f.seek(_mac->_dataOffset + entry->dataOffset);
			int dataSize = _mac->_f.readUint32BE();
			assert(dataSize > kHeaderSize);
			uint8_t buf[kHeaderSize];
			_mac->_f.read(buf, kHeaderSize);
			dataSize -= kHeaderSize;
			uint8_t *p = (uint8_t *)malloc(dataSize);
			if (p) {
				_mac->_f.read(p, dataSize);
				for (int j = 0; j < dataSize; ++j) {
					p[j] = ((int8_t)(p[j] ^ 0x80)) / kGain;
				}
				_sfxList[i].len = READ_BE_UINT32(buf + 0x12);
				_sfxList[i].freq = READ_BE_UINT16(buf + 0x16);
				_sfxList[i].data = p;
				debug(DBG_RES, "sfx #%d len %d datasize %d freq %d", i, _sfxList[i].len, dataSize, _sfxList[i].freq);
			}
		}
	}
}

static int dumpPascalString(const uint8_t *s, const char *text) {
	fputs(text, stdout);
	const int len = *s++;
	for (int i = 0; i < len; ++i, ++s) {
		if (*s == 0xA9) {
			fprintf(stdout, "(c)");
		} else if (*s < 0x80) {
			fputc(*s, stdout);
		}
	}
	fputc('\n', stdout);
	return len + 1;
}

void Resource::MAC_dumpVersion() {
	const int versionType = _mac->_versIndex;
	if (versionType != -1) {
		const ResourceMacEntry *entry = _mac->getEntry(versionType, 0);
		_mac->_f.seek(_mac->_dataOffset + entry->dataOffset);
		int dataSize = _mac->_f.readUint32BE();
		static const int kVersionSize = 0x80;
		assert(dataSize <= kVersionSize);
		uint8_t buf[kVersionSize];
		_mac->_f.read(buf, kVersionSize);
		int offset = 0;
		++offset; // major
		++offset; // minor
		++offset; // development stage
		++offset; // prerelease version
		offset += 2; // region
		const int len = buf[offset++];
		offset += len;
		// offset += dumpPascalString(buf + offset);
		offset += dumpPascalString(buf + offset, "Macintosh version: ");
	}
}
