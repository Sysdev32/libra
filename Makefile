# =========================
# --- Toolchain Config ---
# =========================

CROSS_COMPILE ?= toolchain/
CC      ?= $(CROSS_COMPILE)x86_64-elf-gcc
LD      ?= $(CROSS_COMPILE)x86_64-elf-ld

AS      := nasm
XORRISO := xorriso
QEMU    := qemu-system-x86_64

# =========================
# --- CI MODE SWITCH ---
# =========================
# Set ACTIONS=1 in GitHub Actions
# Enables CI-safe behavior
ACTIONS ?= 0

ifeq ($(ACTIONS),1)
    TOOLCHAIN_PATH := $(shell pwd)/toolchain
    CROSS_COMPILE := $(TOOLCHAIN_PATH)/
endif

# =========================
# --- Flags ---
# =========================

CFLAGS := -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone \
          -m64 -march=x86-64 -Wall -Wextra -O2 -I./inc -I. \
          -mcmodel=kernel -g

LDFLAGS := -T linker.ld -nostdlib -z max-page-size=0x1000
ASFLAGS := -f elf64

DEPFLAGS = -MT $@ -MMD -MP -MF $(DEP_DIR)/$(notdir $(basename $<)).d

# =========================
# --- Directories ---
# =========================

SRC_DIR := src
OBJ_DIR := objects
DEP_DIR := dependencies
ISO_DIR := iso

# =========================
# --- Sources ---
# =========================

SRCS_C   := $(shell find $(SRC_DIR) -type f -name '*.c')
SRCS_S   := $(shell find $(SRC_DIR) -type f -name '*.s')
SRCS_ASM := $(shell find $(SRC_DIR) -type f -name '*.asm')

OBJS_C   := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS_C))
OBJS_S   := $(patsubst $(SRC_DIR)/%.s, $(OBJ_DIR)/%_s.o, $(SRCS_S))
OBJS_ASM := $(patsubst $(SRC_DIR)/%.asm, $(OBJ_DIR)/%_asm.o, $(SRCS_ASM))

OBJS     := $(OBJS_C) $(OBJS_S) $(OBJS_ASM)
DEPS     := $(patsubst $(SRC_DIR)/%.c, $(DEP_DIR)/%.d, $(SRCS_C))

# =========================
# --- Targets ---
# =========================

KERNEL  := $(ISO_DIR)/kernel.elf
ISO_IMG := custom_os.iso

.PHONY: all clean iso run

all: $(KERNEL)
	ls
# =========================
# --- Link ---
# =========================

$(KERNEL): $(OBJS) linker.ld | $(ISO_DIR)/.dir
	@echo "[LD] Linking kernel"
	@$(LD) $(LDFLAGS) $(OBJS) -o $@

# =========================
# --- Compile C ---
# =========================

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR) $(DEP_DIR)
	@echo "[CC] $<"
	@mkdir -p $(dir $@)
	@$(CC) $(DEPFLAGS) $(CFLAGS) -c $< -o $@

# =========================
# --- ASM ---
# =========================

$(OBJ_DIR)/%_s.o: $(SRC_DIR)/%.s | $(OBJ_DIR)
	@echo "[AS] $<"
	@mkdir -p $(dir $@)
	@$(AS) $(ASFLAGS) $< -o $@

$(OBJ_DIR)/%_asm.o: $(SRC_DIR)/%.asm | $(OBJ_DIR)
	@echo "[AS] $<"
	@mkdir -p $(dir $@)
	@$(AS) $(ASFLAGS) $< -o $@

# =========================
# --- dirs ---
# =========================

$(OBJ_DIR) $(DEP_DIR):
	@mkdir -p $@

$(ISO_DIR)/.dir:
	@mkdir -p $(ISO_DIR)
	@touch $@

# =========================
# --- ISO ---
# =========================

iso: $(KERNEL)
	@echo "[ISO] building"

	@if [ ! -f $(ISO_DIR)/limine.conf ]; then \
		echo "missing limine.conf"; exit 1; \
	fi

	@rm -f $(ISO_DIR)/limine-bios.sys $(ISO_DIR)/limine-bios-cd.bin
	@cp limine/limine-uefi-cd.bin $(ISO_DIR)/
	@mkdir -p $(ISO_DIR)/EFI/BOOT
	@cp limine/BOOTX64.EFI $(ISO_DIR)/EFI/BOOT/

	@$(XORRISO) -as mkisofs \
		-e limine-uefi-cd.bin \
		-no-emul-boot \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ISO_DIR) -o $(ISO_IMG)

# =========================
# --- RUN ---
# =========================

run: iso
	ls
	@$(QEMU) -bios /usr/share/ovmf/OVMF.fd -cdrom $(ISO_IMG) -m 4G -M q35 -serial stdio -D qemu.log -d int

# =========================
# --- CLEAN ---
# =========================

clean:
	@rm -rf $(OBJ_DIR) $(DEP_DIR) $(ISO_IMG)
	@rm -rf $(ISO_DIR)/EFI $(ISO_DIR)/limine-uefi-cd.bin $(ISO_DIR)/kernel.elf

-include $(DEPS)