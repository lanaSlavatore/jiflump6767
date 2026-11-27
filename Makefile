ifeq ($(strip $(DEVKITPRO)),)
$(error "DEVKITPRO غير معرّف")
endif

include $(DEVKITPRO)/libnx/switch_rules

TARGET     := myupdater
APP_TITLE  := AR Updater
APP_AUTHOR := me
APP_VERSION:= 1.0.0

PORTLIBS := $(DEVKITPRO)/portlibs/switch
PKG      := $(PORTLIBS)/bin/aarch64-none-elf-pkg-config --static
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CXXFLAGS := -g -Wall -O2 -ffunction-sections -fdata-sections $(ARCH) -D__SWITCH__ -std=gnu++17 \
            -I$(LIBNX)/include -I$(PORTLIBS)/include $(shell $(PKG) --cflags SDL2_ttf sdl2 libcurl)

LDFLAGS  := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections \
            -L$(LIBNX)/lib -L$(PORTLIBS)/lib

LIBS     := -lminizip $(shell $(PKG) --libs SDL2_ttf sdl2 libcurl) -lnx

SRCS     := source/main.cpp
OBJS     := main.o

.PHONY: all clean

all: $(TARGET).nro

main.o: source/main.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS)
	$(CXX) $(LDFLAGS) $(OBJS) $(LIBS) -o $@

$(TARGET).nacp:
	nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@

$(TARGET).nro: $(TARGET).elf $(TARGET).nacp
	mkdir -p romfs
	elf2nro $< $@ --nacp=$(TARGET).nacp --romfsdir=romfs

clean:
	rm -f $(OBJS) *.d $(TARGET).elf $(TARGET).nacp $(TARGET).nro
