#---------------------------------------------------------------------------------
# Wii GX graphics demo (libogc)
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPPC)),)
$(error "Set DEVKITPPC / source /opt/devkitpro/devkit-env or use the docker build script")
endif

include $(DEVKITPPC)/wii_rules

# wii_rules links with $(LD) and passes MACHDEP (-mhard-float). That only works when
# LD is the gcc driver, not raw ld.
export CC  := powerpc-eabi-gcc
export CXX := powerpc-eabi-g++
export LD  := powerpc-eabi-gcc

TARGET   := boot
BUILD    := build
SOURCES  := .
INCLUDES :=

CFLAGS   := -g -O2 -Wall -Wextra $(MACHDEP) $(INCLUDE)
CXXFLAGS := $(CFLAGS)
LDFLAGS  := -g $(MACHDEP) -Wl,-Map,$(notdir $@).map
LIBPATHS := -L$(LIBOGC_LIB)

LIBS := -lwiiuse -lbte -logc -lm

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export VPATH   := $(CURDIR)/$(SOURCES)
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES := main.c
export OFILES_SOURCES := $(CFILES:.c=.o)
export OFILES := $(OFILES_SOURCES)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  -I$(CURDIR)/$(BUILD) \
                  -I$(LIBOGC_INC)

.PHONY: all clean

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@rm -rf $(BUILD) $(OUTPUT).elf $(OUTPUT).dol

else

$(OUTPUT).dol: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
