
ifdef QEMU

QEMU_FLAGS += -smp cpus=2
QEMU_FLAGS += -serial stdio
#QEMU_FLAGS += -display none
QEMU_FLAGS += -m 2G
#QEMU_FLAGS += -device virtio-net-pci -nic socket,udp=1000,localaddr=localhost
QEMU_FLAGS += -drive file=nvm.img,if=none,id=nvm
QEMU_FLAGS += -device nvme,serial=deadbeef,drive=nvm

qemu: $(QEMU_DEPS)
	$(call quiet-cmd,QEMU,)
	$(QEMU) $(QEMU_FLAGS)
qemu-gdb: $(QEMU_DEPS)
	$(call quiet-cmd,QEMU,)
	$(QEMU) $(QEMU_FLAGS) -gdb tcp::1234 -S -no-reboot -no-shutdown
qemu-gdb-%: $(QEMU_DEPS)
	$(call quiet-cmd,QEMU,)
	$(QEMU) $(QEMU_FLAGS) -gdb tcp::$* -S

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

