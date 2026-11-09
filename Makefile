ifeq ($(strip $(DEVKITPRO)),)
$(error Set DEVKITPRO to the devkitPro installation directory)
endif

DEVKITA64 ?= $(DEVKITPRO)/devkitA64
PORTLIBS := $(DEVKITPRO)/portlibs/switch
LIBNX := $(DEVKITPRO)/libnx
CC := $(DEVKITA64)/bin/aarch64-none-elf-gcc

PKG_CONFIG_LIBDIR := $(PORTLIBS)/lib/pkgconfig
export PKG_CONFIG_LIBDIR

TARGET := Lampa-nx
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -fPIE
COMMON := -O2 -g -Wall -Wextra -ffunction-sections -fdata-sections $(ARCH)
CFLAGS := $(COMMON) -D__SWITCH__ -I$(LIBNX)/include -I$(PORTLIBS)/include $(shell pkg-config --cflags sdl2 mpv)
LDFLAGS := -specs=$(LIBNX)/switch.specs $(ARCH) -Wl,--gc-sections
LIBS := $(shell pkg-config --libs mpv sdl2) -llzma -lzstd -L$(LIBNX)/lib -lnx -lm

.PHONY: all clean
all: $(TARGET).nro

main.o: source/main.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: main.o
	$(CC) $(LDFLAGS) $< $(LIBS) -o $@

$(TARGET).nacp:
	nacptool --create "Lampa Native" "Small182" "0.1.0" $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
	elf2nro $< $@ --nacp=$(TARGET).nacp --icon=icon.jpg

clean:
	rm -f main.o $(TARGET).elf $(TARGET).nacp $(TARGET).nro
