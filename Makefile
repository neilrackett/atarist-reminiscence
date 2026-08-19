
# REminiscence - Atari ST port
#
# Cross-compiles with m68k-atari-mint-g++ via atarist-toolkit-docker:
#   STCMD_NO_TTY=1 stcmd make
#
# The SDL build for desktop platforms lives in Makefile.sdl.
#
# Amiga data files go in dist/DATA (see tools/README.md for how to
# extract them from the original disk images).

CXX    = m68k-atari-mint-g++
STRIP  = m68k-atari-mint-strip

STDL     = stdl
STDL_LIB = $(STDL)/libstdl.a

CXXFLAGS = -O2 -fomit-frame-pointer -fno-exceptions -fno-rtti \
	-fno-strict-aliasing \
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
	staticres.cpp unpack.cpp util.cpp video.cpp \
	main_atari.cpp systemstub_stdl.cpp video_st.cpp

OBJS = $(SRCS:%.cpp=build/%.o)

TARGET = dist/FLASHBAK.TOS

all: $(TARGET)

$(STDL_LIB):
	$(MAKE) -C $(STDL) libstdl.a

$(TARGET): $(OBJS) $(STDL_LIB) | dist
	$(CXX) $(CXXFLAGS) -o build/flashbak.elf $(OBJS) $(LIBS)
	cp build/flashbak.elf $@
	$(STRIP) $@

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build dist:
	mkdir -p $@

clean:
	rm -rf build $(TARGET)

.PHONY: all clean
