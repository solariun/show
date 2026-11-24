# show - pictures, video and sound in the terminal (FFmpeg + Graphicx), C++11.
#
#   ./env.sh        install everything needed, fetch Graphicx, build (once)
#   make            build ./show
#   make install    copy ./show to $(PREFIX)/bin (default /usr/local)
#   make clean
#
# Needs the FFmpeg development libraries (libavformat, libavcodec, libavutil,
# libswscale, libswresample, libavdevice) and the Graphicx submodule in
# external/Graphicx.  Works on macOS and Linux.

GRAPHICX := external/Graphicx
ifeq ($(wildcard $(GRAPHICX)/src/graphicx.cpp),)
  $(error Graphicx is missing: run ./env.sh (or: git submodule update --init))
endif

UNAME_S    := $(shell uname -s)
PKG_CONFIG ?= pkg-config
FFMPEG_PKGS := libavformat libavcodec libavutil libswscale libswresample libavdevice
HAVE_PKG   := $(shell command -v $(PKG_CONFIG) >/dev/null 2>&1 && $(PKG_CONFIG) --exists $(FFMPEG_PKGS) && echo yes)

ifeq ($(HAVE_PKG),yes)
  FFMPEG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(FFMPEG_PKGS))
  FFMPEG_LIBS   := $(shell $(PKG_CONFIG) --libs $(FFMPEG_PKGS))
else ifeq ($(UNAME_S),Darwin)
  # No pkg-config: use Homebrew's FFmpeg directly.
  BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
  ifeq ($(wildcard $(BREW_PREFIX)/include/libavformat/avformat.h),)
    $(error FFmpeg development files not found: run ./env.sh (or: brew install ffmpeg))
  endif
  FFMPEG_CFLAGS := -I$(BREW_PREFIX)/include
  FFMPEG_LIBS   := -L$(BREW_PREFIX)/lib -lavdevice -lavformat -lavcodec -lswscale -lswresample -lavutil
else
  $(error FFmpeg development files not found: run ./env.sh)
endif

# FFmpeg headers as system headers: their code is not checked by our warnings.
FFMPEG_CFLAGS := $(patsubst -I%,-isystem %,$(FFMPEG_CFLAGS))

CXX      ?= c++
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++11 -Wall -Wextra -Wpedantic -MMD -MP -I$(GRAPHICX)/include $(FFMPEG_CFLAGS)
LDLIBS   += $(FFMPEG_LIBS) -pthread
PREFIX   ?= /usr/local

TARGET := show
SRCS   := $(wildcard src/*.cpp)
OBJS   := $(SRCS:src/%.cpp=build/%.o) build/graphicx/graphicx.o build/graphicx/terminal.o
DEPS   := $(OBJS:.o=.d)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/graphicx/%.o: $(GRAPHICX)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -rf build $(TARGET)

-include $(DEPS)
