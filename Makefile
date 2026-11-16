ifeq ($(strip $(DEVKITPRO)),)
$(error Set DEVKITPRO to the devkitPro installation directory)
endif

DEVKITA64 ?= $(DEVKITPRO)/devkitA64
PORTLIBS := $(DEVKITPRO)/portlibs/switch
LIBNX := $(DEVKITPRO)/libnx
CC := $(DEVKITA64)/bin/aarch64-none-elf-gcc

PKG_CONFIG_LIBDIR := $(PORTLIBS)/lib/pkgconfig
export PKG_CONFIG_LIBDIR

TARGET := UAPlay
APP_VERSION := $(shell sed -n 's/^#define UAPLAY_VERSION "\([^"]*\)"/\1/p' source/version.h)
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -fPIE
COMMON := -O2 -g -Wall -Wextra -ffunction-sections -fdata-sections $(ARCH)
CFLAGS := $(COMMON) -D__SWITCH__ -I$(LIBNX)/include -I$(PORTLIBS)/include $(shell pkg-config --cflags sdl2 SDL2_ttf SDL2_image mpv libcurl)
LDFLAGS := -specs=$(LIBNX)/switch.specs $(ARCH) -Wl,--gc-sections
LIBS := $(shell pkg-config --libs mpv sdl2 SDL2_ttf SDL2_image libcurl) -lGLESv2 -lharfbuzz -lfreetype -ljpeg -lwebp -llzma -lzstd -L$(LIBNX)/lib -lnx -lm

.PHONY: all clean
all: $(TARGET).nro

main.o: source/main.c source/wallpapers.h source/catalog.h source/posters.h source/library.h source/playback.h source/downloads.h source/platform.h source/i18n.h source/subdl.h source/movy_stream.h source/update.h source/version.h source/iptv.h
	$(CC) $(CFLAGS) -c $< -o $@

catalog.o: source/catalog.c source/catalog.h source/i18n.h source/platform.h source/subdl.h source/update.h source/version.h
	$(CC) $(CFLAGS) -c $< -o $@

posters.o: source/posters.c source/posters.h source/catalog.h
	$(CC) $(CFLAGS) -c $< -o $@

library.o: source/library.c source/library.h source/catalog.h
	$(CC) $(CFLAGS) -c $< -o $@

playback.o: source/playback.c source/playback.h source/i18n.h
	$(CC) $(CFLAGS) -c $< -o $@

downloads.o: source/downloads.c source/downloads.h source/catalog.h source/platform.h source/i18n.h
	$(CC) $(CFLAGS) -c $< -o $@

i18n.o: source/i18n.c source/i18n.h
	$(CC) $(CFLAGS) -c $< -o $@

movy_stream.o: source/movy_stream.c source/movy_stream.h source/catalog.h
	$(CC) $(CFLAGS) -c $< -o $@

update.o: source/update.c source/update.h source/version.h
	$(CC) $(CFLAGS) -c $< -o $@

iptv.o: source/iptv.c source/iptv.h source/iptv_countries.inc source/platform.h source/i18n.h
	$(CC) $(CFLAGS) -c $< -o $@

wallpapers.o: source/wallpapers.c source/wallpapers.h source/i18n.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: main.o catalog.o posters.o library.o playback.o downloads.o i18n.o movy_stream.o update.o iptv.o wallpapers.o
	$(CC) $(LDFLAGS) $^ $(LIBS) -o $@

$(TARGET).nacp: source/version.h
	nacptool --create "UAPlay" "Small182" "$(APP_VERSION)" $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
	elf2nro $< $@ --nacp=$(TARGET).nacp --icon=icon.jpg --romfsdir=romfs

clean:
	rm -f main.o catalog.o posters.o library.o playback.o downloads.o i18n.o movy_stream.o update.o iptv.o wallpapers.o $(TARGET).elf $(TARGET).nacp $(TARGET).nro
