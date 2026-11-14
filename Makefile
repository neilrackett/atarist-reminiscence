
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

# Amiga-data engine + ST platform layer. The other versions' loaders
# (Mac, PC98, the DOS demo archives), the SEQ cutscene player and the
# digital music players other than .mod can never run here, so
# st_stubs.cpp stands in for them with the few entry points the engine
# still names, and the upstream sources stay untouched. The .mod player
# is built: on an STE it drives the STDL_Voice mixer (mod_player.cpp).
# Nor are the SDL stub, scalers and MIDI drivers built.
SRCS = entry_atari.cpp collision.cpp cutscene.cpp file.cpp \
	fs.cpp game.cpp graphics.cpp menu.cpp mixer.cpp \
	piege.cpp \
	protection.cpp resource.cpp sfx_player.cpp \
	staticres.cpp splash_data.cpp unpack.cpp util.cpp video.cpp \
	main_atari.cpp systemstub_atari.cpp systemstub_stdl.cpp video_st.cpp pagetrack.cpp st_stubs.cpp \
	palette_st.cpp mod_player.cpp ym_sfx_st.cpp spritecache_st.cpp displaylist_st.cpp

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

# -e __st_entry: the program starts in entry_atari.cpp, which clears the
# BSS whatever loaded it, and then runs mintlib's crt0. It is first in
# SRCS so it sits at the start of the program: on a TT or a Falcon the
# ST build hands over to FBACK030.TOS keeping only that much of itself.
$(TARGET): $(OBJS) $(STDL_LIB) | dist
	$(CXX) $(CXXFLAGS) -Wl,-e,__st_entry -o build/flashbak.elf $(OBJS) $(LIBS)
	cp build/flashbak.elf $@
	$(STRIP) $@
	@# the documented template, but never over a config in use
	@test -f dist/RS.CFG || cp RS.CFG.template dist/RS.CFG

# The 030 build, FBACK030.TOS: the same game for a TT or a Falcon in
# 256 colours (make 030). Upstream's chunky layers stand in for the
# ST's planar ones, so the planar files and the ST stub stay out, and
# the 030 stub shows them (systemstub_030.cpp) through STDL's SDL2
# add-on (stdl/addons/sdl2, its archive libstdl-sdl2.a); STDL's
# keyboard, joystick, sound and clock (systemstub_atari.cpp), RS.CFG and
# the menus are the ST build's. Its objects build apart, for the 68020
# and up with no FPU instructions, and link against the 68000
# libraries: the toolchain's 68020-60 ones assume an FPU, which a stock
# Falcon and some TTs lack, and their mintlib will not start without
# one.
SRCS_030 = $(filter-out systemstub_stdl.cpp video_st.cpp spritecache_st.cpp displaylist_st.cpp palette_st.cpp,$(SRCS)) \
	systemstub_030.cpp
OBJS_030 = $(SRCS_030:%.cpp=build030/%.o)
TARGET_030 = dist/FBACK030.TOS
CXXFLAGS_030 = $(CXXFLAGS) -DATARIST_030 -m68020-60 -msoft-float

# The add-on's headers for the stub alone: its SDL.h is SDL2's, and no
# other file of the port should find it.
STDL_SDL2_LIB = $(STDL)/libstdl-sdl2.a
STDL_SDL2_SRCS = $(wildcard $(STDL)/addons/sdl2/src/*.c) \
                 $(wildcard $(STDL)/addons/sdl2/src/*.h) \
                 $(wildcard $(STDL)/addons/sdl2/include/SDL2/*.h)
build030/systemstub_030.o: CXXFLAGS_030 += -I$(STDL)/addons/sdl2/include/SDL2

$(STDL_SDL2_LIB): $(STDL_SDL2_SRCS) $(STDL_SRCS)
	$(MAKE) -C $(STDL) libstdl-sdl2.a

030: $(TARGET_030)

$(TARGET_030): $(OBJS_030) $(STDL_SDL2_LIB) $(STDL_LIB) | dist
	$(CXX) $(CXXFLAGS) -m68000 -Wl,-e,__st_entry -o build030/fback030.elf $(OBJS_030) $(STDL_SDL2_LIB) $(LIBS)
	cp build030/fback030.elf $@
	$(STRIP) $@
	@test -f dist/RS.CFG || cp RS.CFG.template dist/RS.CFG

build030/main_atari.o: build/version.h

build030/%.o: src/%.cpp | build030 build
	$(CXX) $(CXXFLAGS_030) -c -o $@ $<

build030:
	mkdir -p $@

-include $(OBJS_030:.o=.d)

# Experiments (make experimental): builds to try things with, which
# `make`, `make 030` and the release workflow never build or publish.
#
# FBACKEXP.TOS is the 030 build's game compiled for the 68000 on
# STDL's experimental 68000 build of the SDL2 add-on (stdl/docs/sdl2.md,
# "An experimental build for every machine"): one program for every
# machine. A TT or a Falcon shows what FBACK030.TOS shows, frame for
# frame; an ST, an STE or a Mega STE shows the chunky picture reduced to
# 16 colours, about 5 frames a second at 8MHz, to try the 030 build's
# game where there is no TT or Falcon. ATARIST_030_ST takes the machine
# check and the hand-over out of __st_entry (entry_atari.cpp).
OBJS_EXP = $(SRCS_030:%.cpp=buildexp/%.o)
TARGET_EXP = dist/FBACKEXP.TOS
CXXFLAGS_EXP = $(CXXFLAGS) -DATARIST_030 -DATARIST_030_ST -m68000
STDL_SDL2ST_LIB = $(STDL)/libstdl-sdl2-st.a
buildexp/systemstub_030.o: CXXFLAGS_EXP += -I$(STDL)/addons/sdl2/include/SDL2

$(STDL_SDL2ST_LIB): $(STDL_SDL2_SRCS) $(STDL_SRCS)
	$(MAKE) -C $(STDL) libstdl-sdl2-st.a

experimental: $(TARGET_EXP)

$(TARGET_EXP): $(OBJS_EXP) $(STDL_SDL2ST_LIB) $(STDL_LIB) | dist
	$(CXX) $(CXXFLAGS) -m68000 -Wl,-e,__st_entry -o buildexp/fbackexp.elf $(OBJS_EXP) $(STDL_SDL2ST_LIB) $(LIBS)
	cp buildexp/fbackexp.elf $@
	$(STRIP) $@
	@test -f dist/RS.CFG || cp RS.CFG.template dist/RS.CFG

buildexp/main_atari.o: build/version.h

buildexp/%.o: src/%.cpp | buildexp build
	$(CXX) $(CXXFLAGS_EXP) -c -o $@ $<

buildexp:
	mkdir -p $@

-include $(OBJS_EXP:.o=.d)

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
	rm -rf build build030 buildexp $(TARGET) $(TARGET_030) $(TARGET_EXP)

-include $(DEPS)

.PHONY: all clean 030 experimental
