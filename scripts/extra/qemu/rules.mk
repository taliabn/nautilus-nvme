
ifdef QEMU

QEMU_FLAGS += -smp cpus=$(NAUT_CONFIG_QEMU_NUM_CPUS)
QEMU_FLAGS += -m $(NAUT_CONFIG_QEMU_MEMORY_GB)G

ifdef NAUT_CONFIG_QEMU_NO_DISPLAY
QEMU_FLAGS += -display none
endif

QEMU_FLAGS += -serial stdio

QEMU_GDB_FLAGS += -no-reboot -no-shutdown
ifdef NAUT_CONFIG_QEMU_GDB_BOOT_PAUSE
QEMU_GDB_FLAGS += -S
endif	

# Adding in an NVME drive if NVME is specified
ifdef NAUT_CONFIG_QEMU_NVME_DRIVE
QEMU_FLAGS += -drive file=$(NAUT_CONFIG_QEMU_NVME_DRIVE_FILE_PATH),if=none,id=nvm,format=raw \
              -device nvme,serial=deadbeef,drive=nvm
endif

qemu: $(QEMU_DEPS)
	$(call quiet-cmd,QEMU,)
	$(QEMU) $(QEMU_FLAGS)
qemu-gdb: $(QEMU_DEPS)
	$(call quiet-cmd,QEMU,)
	$(QEMU) $(QEMU_FLAGS) -gdb tcp::$(NAUT_CONFIG_QEMU_GDB_SERVER_PORT) $(QEMU_GDB_FLAGS) 

ifdef NAUT_CONFIG_USE_FDT

QEMU_DTB := $(OUTPUT_DIR)/qemu.dtb
QEMU_DTS := $(OUTPUT_DIR)/qemu.dts

$(QEMU_DTB): $(QEMU_DEPS) FORCE
	$(call quiet-cmd,QEMU,$@)
	$(Q)$(QEMU) $(QEMU_FLAGS) -machine dumpdtb=$(QEMU_DTB) $(QPIPE)

DTC := dtc
%.dts: %.dtb
	$(call quiet-cmd,DTC,$@)
	$(Q)$(DTC) $< -o $@

qemu-dts: $(QEMU_DTS)

device-tree-clean: FORCE
	$(Q)rm -f $(QEMU_DTB)
	$(Q)rm -f $(QEMU_DTS)
CLEAN_RULES += device-tree-clean

endif
endif

