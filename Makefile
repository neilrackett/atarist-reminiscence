
# REminiscence - Atari ST port
# Copyright (C) 2026 Neil Rackett
#
# Cross-compiles with m68k-atari-mint-g++ via atarist-toolkit-docker:
#   STCMD_NO_TTY=1 stcmd make
#
# The SDL build for desktop platforms lives in Makefile.sdl.
#
# Amiga data files go in dist/DATA and the optional chip music in
# dist/MUSIC (see tools/README.md for how to extract both from the
# original disk images).

CXX    = m68k-atari-mint-g++
STRIP  = m68k-atari-mint-strip

STDL     = stdl
STDL_LIB = $(STDL)/libstdl.a

# -MMD -MP: without header dependencies, editing a header rebuilds
# only the .cpp files touched in the same pass. Game holds a Cutscene
# by value, so a stale object compiled against a different cutscene.h
# puts every member after it at the wrong offset - silent memory
# corruption that surfaces as bus errors far from the edit.
CXXFLAGS = -O2 -fomit-frame-pointer -fno-exceptions -fno-rtti \
	-fno-strict-aliasing -MMD -MP \
	-Wall -Wno-unused-parameter \
	-I$(STDL)/include -DATARIST -DNDEBUG

LIBS = $(STDL_LIB) -lm

# Amiga-data engine + ST platform layer. DOS/Mac/PC98/Sega loaders are
# still compiled (they are small and keep the diff against upstream
# minimal); the SDL stub, scalers and MIDI drivers are not.
SRCS = collision.cpp cpc_player.cpp cutscene.cpp decode_mac.cpp file.cpp \
	fs.cpp game.cpp graphics.cpp menu.cpp midi_parser.cpp mixer.cpp \
	mod_player.cpp ogg_player.cpp piege.cpp prf_player.cpp \
	protection.cpp resource.cpp resource_aba.cpp resource_mac.cpp \
	resource_paq.cpp screenshot.cpp seq_player.cpp sfx_player.cpp \
	staticres.cpp splash_data.cpp unpack.cpp util.cpp video.cpp \
	main_atari.cpp systemstub_stdl.cpp video_st.cpp

OBJS = $(SRCS:%.cpp=build/%.o)
DEPS = $(OBJS:.o=.d)

TARGET = dist/FLASHBAK.TOS

# The version the binary announces on the console and in RS.LOG: the
# nearest release tag, then the commit's hash when the build is not
# that exact commit, then + when the tree had uncommitted changes.
# v0.5.6-atarist.123-56da7ca+ is the longest that can come out, and
# with the name in front it still fits the ST's 40-column console.
# The release workflow passes in the tag it is about to create, so a
# release reads as the tag alone. It lands in a generated header that
# is rewritten only when the string changes, so an incremental build
# cannot carry a stale one and an unchanged one rebuilds nothing.
# -c safe.directory: the build runs inside the toolkit container, where
# the checkout belongs to another user and git otherwise refuses it.
PORT_VERSION ?= $(shell git -c safe.directory='*' describe --tags --match 'v*-atarist.*' --always --dirty=+ 2>/dev/null \
	| sed -E 's/-[0-9]+-g([0-9a-f]+)/-\1/')
ifeq ($(PORT_VERSION),)
PORT_VERSION = unknown
endif
# What the console shows at startup: the release number alone, with the
# hash and + carried over - r13, r13-6992ccc+. The tag's v0.5.6-atarist.
# prefix is upstream's version and the tag's namespace, neither of which
# a tester needs to read off a photograph. RS.LOG keeps the full form.
PORT_RELEASE := $(shell echo '$(PORT_VERSION)' | sed -E 's/^v[0-9.]+-atarist\.([0-9]+)/r\1/')
CXXFLAGS += -Ibuild

all: $(TARGET)

# Depend on the library's own sources: without this the archive is
# only ever built when missing, so a submodule update leaves a stale
# libstdl.a linked against fresh headers.
# STDL's own sources, so editing the library rebuilds the archive.
# xpad arrives through STDL's submodule at lib/xpad rather than its
# src/, so it needs naming separately or a bump there goes unnoticed.
STDL_SRCS = $(wildcard $(STDL)/src/*.c) $(wildcard $(STDL)/include/stdl/*.h) \
            $(wildcard $(STDL)/lib/xpad/src/*.c)

$(STDL_LIB): $(STDL_SRCS)
	$(MAKE) -C $(STDL) libstdl.a

$(TARGET): $(OBJS) $(STDL_LIB) | dist
	$(CXX) $(CXXFLAGS) -o build/flashbak.elf $(OBJS) $(LIBS)
	cp build/flashbak.elf $@
	$(STRIP) $@
	@# the documented template, but never over a config in use
	@test -f dist/RS.CFG || cp RS.CFG.template dist/RS.CFG

build/version.h: FORCE | build
	@printf '#define PORT_VERSION "%s"\n#define PORT_RELEASE "%s"\n' '$(PORT_VERSION)' '$(PORT_RELEASE)' > $@.tmp; \
	cmp -s $@.tmp $@ || mv $@.tmp $@; rm -f $@.tmp
build/main_atari.o: build/version.h
.PHONY: FORCE
FORCE:

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build dist:
	mkdir -p $@

clean:
	rm -rf build $(TARGET)

-include $(DEPS)

.PHONY: all clean
