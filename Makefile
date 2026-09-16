#-------------------------------------------------------------------------------
.SUFFIXES:
#-------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)

include $(DEVKITPRO)/wups/share/wups_rules

WUT_ROOT := $(DEVKITPRO)/wut
# libkernel and libmocha both install under this prefix, and libkernel's own
# linker script lives in its share directory. See its README.
WUMS_ROOT := $(DEVKITPRO)/wums

TARGET		:=	JustGetMiiOnline
BUILD		:=	build
SOURCES		:=	src
DATA		:=
INCLUDES	:=	src

OPT	:=	-Os -fno-exceptions -fno-asynchronous-unwind-tables
CFLAGS	:=	-Wall -Wextra -ffunction-sections -fdata-sections \
			$(MACHDEP) $(OPT)

CFLAGS	+=	$(INCLUDE) -D__WIIU__ -D__WUT__ -D__WUPS__

ifeq ($(DEBUG),1)
	CFLAGS += -DDEBUG -g
endif

CXXFLAGS	:=	$(CFLAGS) -std=c++20

ASFLAGS	:=	-g $(ARCH)

# --discard-locals rather than --discard-all, because Aroma's plugin loader refuses a
# plugin whose symbol table holds as many local symbols as the index of one of its
# loaded sections. WiiUPluginLoaderBackend links each loaded section with the first
# section whose sh_info names it, without checking that it's a relocation table, and a
# symbol table's sh_info is its count of local symbols. On a match the loader happily
# reads the symbol table as relocations, can't find a symbol, and prints
# "Failed to load plugin" before any hook runs.
#
# With --discard-all the symbol table in the finished .wps held the null symbol, five
# section symbols, and each string label the linker keeps for a relocation into merged
# strings. Those labels come from library code rather than this plugin's: the init
# functions of libmocha, libfunctionpatcher and libnotifications, and newlib's dtoa.
# This plugin had two, which put the count at 8, one short of .bss at 9, and the first
# boot toast linked libnotifications and made it 9. Keeping named local symbols puts the
# count in the hundreds, above every section index a .wps has, and the check at the foot
# of this file deletes any build the loader would still refuse.
LDFLAGS	=	-g $(ARCH) $(OPT) $(RPXSPECS) -Wl,-Map,$(notdir $*.map) \
			-Wl,-gc-sections -Wl,--discard-locals \
			-T$(WUMS_ROOT)/share/libkernel.ld

LDFLAGS	+=	$(WUPSSPECS)

# -lfunctionpatcher is for token.cpp installing the token answer inside the Miiverse
# applet at runtime, by resolving nn_act there and patching the address. The static
# WUPS replacement never reaches that process. It sits before -lwut because it calls
# coreinit through wut's stubs and nothing to its left calls back into it.
#
# -lnotifications is the boot toast in notify.cpp and nothing else. It reaches Aroma's
# NotificationModule through OSDynLoad at runtime rather than through an import, so a
# console without the module still loads this plugin and just shows no toast. It sits
# before -lwut for the same reason -lfunctionpatcher does. Linking it is what moved
# the symbol table's local count onto .bss the first time, and the note by LDFLAGS
# has why that no longer matters.
#
# -lz is for the Roseverse import in secret.cpp and nothing else. It comes from the
# ppc-zlib portlib, which the Dockerfile's base image already carries, and sits last
# in the list because everything above may call into it and nothing in it calls back.
LIBS	:=	-lwups -lfunctionpatcher -lnotifications -lwut -lmocha -lkernel -lz

LIBDIRS	:=	$(PORTLIBS) $(WUPS_ROOT) $(WUT_ROOT) $(WUT_ROOT)/usr $(WUMS_ROOT)

#-------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#-------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 		:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@$(shell [ ! -d $(BUILD) ] && mkdir -p $(BUILD))
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).wps $(TARGET).elf

#-------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

# Deleted here when Aroma's plugin loader would refuse it, rather than left to be copied
# to an SD card and found out the hard way at boot. tools/relocation-pairing.awk has what
# it checks, and the note by LDFLAGS has why a build can fail it.
all	:	$(OUTPUT).wps
	@$(PREFIX)readelf -S -W -t $< | awk -f $(TOPDIR)/tools/relocation-pairing.awk || \
		{ rm -f $<; echo "$(notdir $<) deleted: Aroma's plugin loader would refuse it"; exit 1; }

$(OUTPUT).wps	:	$(OUTPUT).elf
$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

-include $(DEPENDS)

#-------------------------------------------------------------------------------
endif
#-------------------------------------------------------------------------------
