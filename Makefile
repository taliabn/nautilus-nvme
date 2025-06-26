
export

ifneq ($(MAKELEVEL),0)

# Root Directories to generate a builtin.o from
obj-y += $(SOURCE_DIR)/ $(LIBRARY_DIR)/

else

default:
	@:

ROOT_DIR=$(shell pwd)
ABS_ROOT_DIR=$(ROOT_DIR)

SOURCE_DIR:=$(ROOT_DIR)/src
LIBRARY_DIR:=$(ROOT_DIR)/lib
INCLUDE_DIR:=$(ROOT_DIR)/include
OUTPUT_DIR:=$(ROOT_DIR)
SCRIPTS_DIR:=$(ROOT_DIR)/scripts
SETUPS_DIR:=$(ROOT_DIR)/setups
LINK_DIR:=$(ROOT_DIR)/link

COMMON_FLAGS += -fno-inline

# Config dependency
PYTHON=python3

include $(SCRIPTS_DIR)/include.mk
include $(SCRIPTS_DIR)/kconfig.mk

# This checks to make sure that the .config file exists and has been loaded
ifndef NAUT_CONFIG_CONFIG

# default to x86 if no config exists yet
ARCH ?= x64

default: no_config_message
no_config_message:
	echo "No .config file found!"
	echo "Run 'make defconfig' or 'make menuconfig' to configure the kernel"

else

# Architecture Specific Build Configurations
ifdef NAUT_CONFIG_ARCH_X86
ARCH ?= x64
endif
ifdef NAUT_CONFIG_ARCH_RISCV
ARCH ?= riscv
endif
ifdef NAUT_CONFIG_ARCH_ARM64
ARCH ?= arm64
endif

ARCH_SCRIPTS_DIR:=$(SCRIPTS_DIR)/arch/$(ARCH)
-include $(ARCH_SCRIPTS_DIR)/config.mk

# Toolchain Specific Build Configurations
ifdef NAUT_CONFIG_USE_CLANG
TOOLCHAIN ?= clang
endif
ifdef NAUT_CONFIG_USE_GCC
TOOLCHAIN ?= gcc
endif
ifdef NAUT_CONFIG_USE_WLLVM
TOOLCHAIN ?= wllvm
endif

TOOLCHAIN_SCRIPTS_DIR:=$(SCRIPTS_DIR)/toolchain/$(TOOLCHAIN)
-include $(TOOLCHAIN_SCRIPTS_DIR)/config.mk

# General Build Configuration

LD_SCRIPT_SRC ?= $(LINK_DIR)/nautilus.ld.$(ARCH)
LD_SCRIPT = $(LINK_DIR)/link.ld

COMMON_FLAGS += -fno-omit-frame-pointer \
			   -ffreestanding \
			   -fno-strict-aliasing \
               -fno-strict-overflow \

COMMON_FLAGS += -Wno-int-conversion

NAUT_INCLUDE += -D__NAUTILUS__ -I$(INCLUDE_DIR) \
		     -include $(AUTOCONF)

NAUT_OBJ_NAME := builtin.o
NAUT_BIN_NAME := nautilus.bin
NAUT_ASM_NAME := nautilus.asm

NAUT_OBJ := $(OUTPUT_DIR)/$(NAUT_OBJ_NAME)
NAUT_BIN := $(OUTPUT_DIR)/$(NAUT_BIN_NAME)
NAUT_ASM := $(OUTPUT_DIR)/$(NAUT_ASM_NAME)

CPPFLAGS += $(NAUT_INCLUDE)

SUPRESSED_WARNINGS += \
		-Wno-frame-address \
	        -Wno-unused-function \
	        -Wno-unused-variable \
		-Wno-unused-but-set-variable \
		-Wno-pointer-sign \

CFLAGS += $(COMMON_FLAGS) \
	  -Wall \
	  $(SUPRESSED_WARNINGS) \
	  -fno-common \
	  -Wstrict-overflow=5 \
	  $(NAUT_INCLUDE)

AFLAGS += $(COMMON_FLAGS) \
	  $(NAUT_INCLUDE)

LDFLAGS += 
MAKEFLAGS += --no-print-directory

ifdef NAUT_CONFIG_DEBUG_INFO
CFLAGS		+= -g
endif

ifdef NAUT_CONFIG_CXX_SUPPORT

CXXFLAGS += $(COMMON_FLAGS) \
			$(NAUT_INCLUDE) \
			-fno-exceptions \
			-fno-rtti

#TODO: Theoretically we need to provide libstdc++ here
#      but for now I'm just not going to worry about it -KJH

endif

#
# Build Rules
#

# nautilus.o (Fully compiled but before linker script has been applied)
$(NAUT_OBJ_NAME): $(NAUT_OBJ)
$(NAUT_OBJ): $(AUTOCONF) FORCE
	#$(call quiet-cmd,MAKE,$(call rel-dir, $@, $(ROOT_DIR)))
	$(Q)$(MAKE) -C $(ROOT_DIR) -f $(SCRIPTS_DIR)/build.mk builtin.o

# Kernel Link Rule
$(NAUT_BIN_NAME): $(NAUT_BIN)

# Allow toolchains to override this value in order to link the final binary differently
ifeq ($(OVERRIDE_LINK_RULE),)
$(NAUT_BIN): $(LD_SCRIPT) $(NAUT_OBJ)
	$(call quiet-cmd,LD,$(NAUT_BIN_NAME))
	$(Q)$(LD) $(LDFLAGS) -T$(LD_SCRIPT) \
		$(NAUT_OBJ) -o $(NAUT_BIN)
endif

# Linker Script Generation
$(LD_SCRIPT): $(LD_SCRIPT_SRC) $(AUTOCONF)
	$(call quiet-cmd,CPP,$@)
	$(Q)$(CPP) -CC -P $(CPPFLAGS) $< -o $@

# Architecture specific rules
-include $(ARCH_SCRIPTS_DIR)/rules.mk
# Toolchain specific rules
-include $(TOOLCHAIN_SCRIPTS_DIR)/rules.mk

#
# Utility Rules
#

# Extra rules for generating disassembly
-include $(SCRIPTS_DIR)/extra/objdump.mk
# Extra rules for running QEMU
-include $(SCRIPTS_DIR)/extra/qemu/rules.mk
# Configuration for enabling stack protection with GCC
-include $(SCRIPTS_DIR)/extra/stack_prot.mk

# Include clean.mk files found recursively in the scripts dir
# (Regardless of the Kconfig, we want to clean everything we can on make clean,
#  e.g. clean isoimage even without ARCH_X86)
CLEAN_FILES := $(shell find $(SCRIPTS_DIR) -name "clean.mk")
$(foreach CLEAN_FILE,$(CLEAN_FILES), $(eval -include $(CLEAN_FILE)))

DEFAULT_RULES ?= $(NAUT_BIN)
default: $(DEFAULT_RULES)

mrproper: clean
clean: $(CLEAN_RULES)

FORCE:

.PHONY: default clean $(CLEAN_RULES)

endif
endif
