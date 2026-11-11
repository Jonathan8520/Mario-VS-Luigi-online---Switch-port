#---------------------------------------------------------------------------------
# mvlo_nx -- NSMB Mario vs Luigi Online on Nintendo Switch (homebrew .nro)
#
# Native wrapper that loads the game's Android ARM64 build (libmain / libunity /
# libil2cpp / lib_burst_generated + assets/) built from the official sources by
# .github/workflows/android.yml. Ships NO game code or assets.
#
# Adapted from cloverpit_nx's Makefile (MIT, SoLoader lineage).
#
# Requires devkitA64 + libnx and:
#   dkp-pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-zlib
# (all present in the devkitpro/devkita64 docker image used by the CI)
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "Set DEVKITPRO in your environment. (export DEVKITPRO=/opt/devkitpro)")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := mvlo_nx
APP_TITLE   := Mario vs Luigi Online
APP_AUTHOR  := ipodtouch0218 (game) / Jonathan8520 (Switch port)
APP_VERSION := $(shell sed -n 's/.*MVLO_NX_VERSION "\(.*\)".*/\1/p' $(TOPDIR)/source/version.h)
APP_ICON    ?= $(TOPDIR)/icon.jpg
export APP_TITLE APP_AUTHOR APP_VERSION APP_ICON
BUILD    := build
SOURCES  := source
INCLUDES := source

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS  := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES) \
           -DCRASH_LOG_PRINTF=stallPrintf \
           $(INCLUDE) -D__SWITCH__
# The reserved region where the game .so are loaded (keep in sync with so_util.c).
CFLAGS  += -DLOAD_ADDRESS=0xC0000000
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS  := -g $(ARCH)
# GPU buffers from nouveau go to a dedicated arena (cloverpit_gpuarena.c).
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) \
            -Wl,--wrap,malloc -Wl,--wrap,calloc -Wl,--wrap,realloc \
            -Wl,--wrap,memalign -Wl,--wrap,free \
            $(foreach f,nvInitialize nvMapCreate nvMapClose nvAddressSpaceMap nvAddressSpaceUnmap \
              nvGpuChannelCreate nvGpuChannelClose nvioctlChannel_KickoffPb nvFenceWait \
              nvc0_screen_resize_text_area nvc0_program_translate nvc0_program_upload,-Wl,--wrap,$(f)) \
            $(foreach f,svcSetHeapSize svcSetMemoryPermission svcMapMemory svcUnmapMemory \
              svcMapPhysicalMemory svcUnmapPhysicalMemory svcSetProcessMemoryPermission \
              svcMapProcessMemory svcUnmapProcessMemory svcMapProcessCodeMemory svcUnmapProcessCodeMemory \
              nvIoctl nvIoctl2 nvIoctl3,-Wl,--wrap,$(f)) \
            $(foreach f,bsdSocket bsdClose bsdConnect bsdBind bsdSend bsdSendTo bsdRecv bsdRecvFrom \
              bsdPoll bsdSelect bsdIoctl bsdFcntl bsdSetSockOpt bsdGetSockOpt bsdShutdown \
              bsdGetSockName bsdGetPeerName bsdAccept bsdListen bsdWrite bsdRead,-Wl,--wrap,$(f)) \
            $(foreach f,svcSendSyncRequest svcSendSyncRequestWithUserBuffer svcWaitSynchronization \
              svcArbitrateLock svcWaitProcessWideKeyAtomic svcWaitForAddress,-Wl,--wrap,$(f)) \
            -Wl,--wrap,fsFsGetFileTimeStampRaw

LIBS := -lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau -lz -lnx -lm

LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CXX)
export OFILES  := $(SFILES:.s=.o) $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(PORTLIBS)/include/SDL2 -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean
all: $(BUILD) $(BUILD)/mvlo_ini_default.h
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
$(BUILD):
	@mkdir -p $@
# The default mvlo.ini written on first boot == dist/mvlo.ini.example.
$(BUILD)/mvlo_ini_default.h: dist/mvlo.ini.example | $(BUILD)
	@sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $< > $@
clean:
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
DEPENDS := $(OFILES:.o=.d)
NROFLAGS := --icon=$(APP_ICON) --nacp=$(OUTPUT).nacp
all : $(OUTPUT).nro
$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf : $(OFILES)
-include $(DEPENDS)
endif
