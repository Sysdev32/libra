# =========================================================
#               Libra Kernel Build System
# =========================================================

# -------------------------
# Toolchain (CI + Local)
# -------------------------

ACTIONS ?= 0

ifeq ($(ACTIONS),1)
    TOOLCHAIN_DIR := $(shell pwd)/toolchain
    CROSS_COMPILE := $(TOOLCHAIN_DIR)/
else
    CROSS_COMPILE ?= toolchain/
endif

CC := x86_64-elf-gcc
LD := x86_64-elf-ld
AR := x86_64-elf-ar
HCC = gcc
HCFLAGS      := -Wall -Wextra -O2
NCURSES_LIBS:= -lncursesw   # Required to link the terminal interface

AS := nasm
QEMU := qemu-system-x86_64
XORRISO := xorriso
export srctree := .
export SRCARCH := x86

# Directory references
SRC_DIR     := scripts/kbuild-standalone
BIN_DIR     := tools/bin

# Build targets for compiled configuration utilities
CONF_BIN    := $(BIN_DIR)/conf
MCONF_BIN   := $(BIN_DIR)/mconf

# -------------------------
# Flags
# -------------------------

CFLAGS := \
	-ffreestanding \
	-fno-stack-protector \
	-fno-pic \
	-mno-red-zone \
	-m64 \
	-march=x86-64 \
	-mcmodel=kernel \
	-O0 \
	-Wall -Wextra \
	-g \
	-I./inc -I. \
	-mno-sse -mno-mmx -mno-3dnow -fpermissive -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration

LDFLAGS := \
	-T linker.ld \
	-nostdlib \
	-z max-page-size=0x1000

ASFLAGS := -f elf64

# -------------------------
# Directories
# -------------------------

SRC_DIR := src
OBJ_DIR := objects
DEP_DIR := dependencies
ISO_DIR := iso

# -------------------------
# Sources
# -------------------------

SRCS_C   := $(shell find $(SRC_DIR) -type f -name '*.c')
SRCS_S   := $(shell find $(SRC_DIR) -type f -name '*.s')
SRCS_ASM := $(shell find $(SRC_DIR) -type f -name '*.asm')

OBJS_C   := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS_C))
OBJS_S   := $(patsubst $(SRC_DIR)/%.s, $(OBJ_DIR)/%_s.o, $(SRCS_S))
OBJS_ASM := $(patsubst $(SRC_DIR)/%.asm, $(OBJ_DIR)/%_asm.o, $(SRCS_ASM))

OBJS := $(OBJS_C) $(OBJS_S) $(OBJS_ASM)

DEPS := $(patsubst $(SRC_DIR)/%.c, $(DEP_DIR)/%.d, $(SRCS_C))

# -------------------------
# Output
# -------------------------

KERNEL := $(ISO_DIR)/kernel.elf
ISO    := custom_os.iso

# -------------------------
# Targets
# -------------------------

.PHONY: all clean iso run dirs

all: $(KERNEL)

# -------------------------
# Link Kernel
# -------------------------

$(KERNEL): $(OBJS) linker.ld | dirs
	@echo "[LD] Linking kernel"
	@mkdir -p $(dir $@)
	@$(LD) $(LDFLAGS) $(OBJS) -o $@

# -------------------------
# Compile C
# -------------------------

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | dirs
	@echo "[CC] $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

# -------------------------
# Assemble .s
# -------------------------

$(OBJ_DIR)/%_s.o: $(SRC_DIR)/%.s | dirs
	@echo "[AS] $<"
	@mkdir -p $(dir $@)
	@$(AS) $(ASFLAGS) $< -o $@

# -------------------------
# Assemble .asm
# -------------------------

$(OBJ_DIR)/%_asm.o: $(SRC_DIR)/%.asm | dirs
	@echo "[AS] $<"
	@mkdir -p $(dir $@)
	@$(AS) $(ASFLAGS) $< -o $@

# -------------------------
# Directories
# -------------------------

dirs:
	@mkdir -p $(OBJ_DIR) $(DEP_DIR) $(ISO_DIR)

# -------------------------
# ISO Build (UEFI)
# -------------------------

iso: $(KERNEL)
	@echo "[ISO] building..."

	@if [ ! -f $(ISO_DIR)/limine.conf ]; then \
		echo "ERROR: missing limine.conf"; exit 1; \
	fi

	@cp limine/limine-uefi-cd.bin $(ISO_DIR)/
	@mkdir -p $(ISO_DIR)/EFI/BOOT
	@cp limine/BOOTX64.EFI $(ISO_DIR)/EFI/BOOT/

	@$(XORRISO) -as mkisofs \
		-e limine-uefi-cd.bin \
		-no-emul-boot \
		-efi-boot-part \
		--efi-boot-image \
		--protective-msdos-label \
		$(ISO_DIR) -o $(ISO)

# -------------------------
# Run in QEMU
# -------------------------

run: iso
	@echo "[QEMU] booting..."
	@$(QEMU) \
		-bios /usr/share/ovmf/OVMF.fd \
		-cdrom $(ISO) \
		-m 4G \
		-M q35 \
		-serial stdio \
		-D qemu.log \
		-d int

# -------------------------
# Clean
# -------------------------
compile_tools:
	mkdir -p $(BIN_DIR) && cd $(BIN_DIR) && make -C ../../scripts/kbuild-standalone -f Makefile.sample O=`pwd` -j$(shell nproc)
menuconfig: compile_tools
	./$(BIN_DIR)/kconfig/mconf Kconfig
genconfig: compile_tools
	@mkdir -p inc
	export KCONFIG_AUTOHEADER=inc/config.h; \
	export srctree=.; \
	export SRCARCH=x86; \
	./$(BIN_DIR)/kconfig/conf --syncconfig Kconfig

clean:
	@echo "[CLEAN]"
	@rm -rf $(OBJ_DIR) $(DEP_DIR) $(ISO) $(ISO_DIR)/kernel.elf
	@rm -rf inc/config.h
	@rm -rf $(BIN_DIR)/*
	@rm -rf qemu.log .config .config.old

-include $(DEPS)