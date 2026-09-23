/*
 * REminiscence - Flashback interpreter
 * Atari ST port Copyright (C) 2026 Neil Rackett
 *
 * Empty stand-ins for the parts of the engine this port never runs.
 *
 * The ST plays the Amiga data only, and has no software mixer. So the
 * other versions' loaders (Mac, PC98, the DOS demo archives and their
 * decompressors), the DOS SEQ cutscene player and the digital music
 * players (.mod, .ogg, CD-i, .prf and its MIDI parser) can never run -
 * yet the engine still names them, from resource-type switches and as
 * members of Mixer and Game, so their code was linked in and loaded
 * into memory with the rest of the program. On a 2MB machine that is
 * memory the game needs.
 *
 * This file replaces those translation units in the ST build with the
 * few entry points the engine links against. Nothing here is reached
 * with Amiga data: the constructors leave each player idle, and the
 * loaders are behind resource types the ST never detects. Keeping the
 * stand-ins here, rather than fencing the calls with #ifdefs, leaves
 * the upstream sources as they are.
 */

#ifdef ATARIST

#include "cpc_player.h"
#include "decode_mac.h"
#include "midi_parser.h"
#include "mod_player.h"
#include "ogg_player.h"
#include "prf_player.h"
#include "resource_aba.h"
#include "resource_mac.h"
#include "resource_paq.h"
#include "seq_player.h"

// music players: idle from construction

ModPlayer::ModPlayer(Mixer *mixer, FileSystem *fs)
	: _isAmiga(false), _playing(false), _fs(fs) {
}
ModPlayer::~ModPlayer() {}
void ModPlayer::stop() {}

OggPlayer::OggPlayer(Mixer *mixer, FileSystem *fs)
	: _fs(fs), _impl(0) {
}
OggPlayer::~OggPlayer() {}
void OggPlayer::pauseTrack() {}
void OggPlayer::resumeTrack() {}

CpcPlayer::CpcPlayer(Mixer *mixer, FileSystem *fs)
	: _fs(fs) {
}
CpcPlayer::~CpcPlayer() {}
void CpcPlayer::pauseTrack() {}
void CpcPlayer::resumeTrack() {}

MidiParser::MidiParser() {}

PrfPlayer::PrfPlayer(Mixer *mix, FileSystem *fs, const PrfMidiDriver *midiDriver, const char *midiSoundFont)
	: _playing(false) {
}
PrfPlayer::~PrfPlayer() {}
void PrfPlayer::stop() {}

// DOS SEQ cutscenes: only played when INTRO.SEQ exists on DOS data

SeqPlayer::SeqPlayer(SystemStub *stub, Mixer *mixer) {}
SeqPlayer::~SeqPlayer() {}
void SeqPlayer::play(File *f) {}

// other versions' data: only constructed for Mac, PC98 and DOS demos

const char *ResourceMac::FILENAME1 = "";
const char *ResourceMac::FILENAME2 = "";
ResourceMac::ResourceMac(const char *filePath, FileSystem *) {}
ResourceMac::~ResourceMac() {}
void ResourceMac::load() {}
const ResourceMacEntry *ResourceMac::findEntry(const char *name, int type) const { return 0; }
const ResourceMacEntry *ResourceMac::getEntry(int type, int num) const { return 0; }

ResourceAba::ResourceAba(FileSystem *fs) {}
ResourceAba::~ResourceAba() {}
void ResourceAba::readEntries(const char *aba) {}
const ResourceAbaEntry *ResourceAba::findEntry(const char *name) const { return 0; }
uint8_t *ResourceAba::loadEntry(const char *name, uint32_t *size) { return 0; }

ResourcePaq::ResourcePaq(FileSystem *fs) {}
ResourcePaq::~ResourcePaq() {}
bool ResourcePaq::open() { return false; }
const ResourcePaqEntry *ResourcePaq::findEntry(const char *name) const { return 0; }
uint8_t *ResourcePaq::loadEntry(const char *name, uint32_t *size) { return 0; }

uint8_t *decodeLzss(File &f, uint32_t &decodedSize) { decodedSize = 0; return 0; }
void decodeC103(const uint8_t *a3, int w, int h, DecodeBuffer *buf) {}
void decodeC211(const uint8_t *a3, int w, int h, DecodeBuffer *buf) {}

#endif // ATARIST
