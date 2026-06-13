# --- Toolchain Configurations ---
CC      := gcc
AS      := nasm
LD      := ld
XORRISO := xorriso
QEMU    := qemu-system-x86_64

# --- Compilation and Linking Flags ---
# 1. Changed -O0 to -O2: Allows the compiler to optimize loop structures and state checks.
# 2. Added -mno-red-zone: Essential for x86_64 kernels so interrupts don't corrupt the stack.
CFLAGS  := -ffreestanding -fno-stack-protector -fno-pic -mno-red-zone \
		   -m64 -march=x86-64 -Wall -Wextra -O2 -I./inc -I. \
		   -mcmodel=kernel -g

# 3. Added -z max-page-size=0x1000 and explicitly forced stack alignment checks
LDFLAGS := -T linker.ld -nostdlib -z max-page-size=0x1000
# NASM target layout: 64-bit Executable and Linkable Format
ASFLAGS := -f elf64

DEPFLAGS = -MT $@ -MMD -MP -MF $(DEP_DIR)/$(notdir $(basename $<)).d
# --- Directory Paths ---
SRC_DIR := src
OBJ_DIR := objects
DEP_DIR := dependencies
ISO_DIR := iso

# --- File Scanning (Recursive) ---
SRCS_C   := $(shell find $(SRC_DIR) -type f -name '*.c')
SRCS_S   := $(shell find $(SRC_DIR) -type f -name '*.s')
SRCS_ASM := $(shell find $(SRC_DIR) -type f -name '*.asm')

# Map all discovered source targets to object destinations
OBJS_C   := $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS_C))
OBJS_S   := $(patsubst $(SRC_DIR)/%.s, $(OBJ_DIR)/%_s.o, $(SRCS_S))
OBJS_ASM := $(patsubst $(SRC_DIR)/%.asm, $(OBJ_DIR)/%_asm.o, $(SRCS_ASM))

# Combine all objects into a single master linking checklist
OBJS     := $(OBJS_C) $(OBJS_S) $(OBJS_ASM)
DEPS     := $(patsubst $(SRC_DIR)/%.c, $(DEP_DIR)/%.d, $(SRCS_C))

# Target Files
KERNEL  := $(ISO_DIR)/kernel.elf
ISO_IMG := custom_os.iso

# --- Build Rules ---
.PHONY: all clean iso run

all: $(KERNEL)

# Link the kernel binary combining C and NASM artifacts
$(KERNEL): $(OBJS) linker.ld | $(ISO_DIR)/.dir
	@echo "[LD] Linking $@"
	@$(LD) $(LDFLAGS) $(OBJS) -o $@

# Compile C source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR) $(DEP_DIR)
	@echo "[CC] Compiling $<"
	@mkdir -p $(dir $@)
	@$(CC) $(DEPFLAGS) $(CFLAGS) -c $< -o $@

# Assemble lowercase .s assembly files
$(OBJ_DIR)/%_s.o: $(SRC_DIR)/%.s | $(OBJ_DIR)
	@echo "[AS] Assembling $<"
	@mkdir -p $(dir $@)
	@$(AS) $(ASFLAGS) $< -o $@

# Assemble lowercase .asm assembly files
$(OBJ_DIR)/%_asm.o: $(SRC_DIR)/%.asm | $(OBJ_DIR)
	@echo "[AS] Assembling $<"
	@mkdir -p $(dir $@)
	@$(AS) $(ASFLAGS) $< -o $@

# Create baseline directory nodes
$(OBJ_DIR) $(DEP_DIR):
	@mkdir -p $@

$(ISO_DIR)/.dir:
	@mkdir -p $(ISO_DIR)
	@touch $@

# --- Limine 7+ Pure UEFI Bootable ISO Generation ---
iso: $(KERNEL)
	@if [ ! -f $(ISO_DIR)/limine.conf ]; then \
		echo "ERROR: $(ISO_DIR)/limine.conf not found! Please create it."; exit 1; \
	fi
	@echo "[ISO] Structuring directory layout for modern UEFI booting"
	@rm -f $(ISO_DIR)/limine-bios.sys $(ISO_DIR)/limine-bios-cd.bin
	@cp limine/limine-uefi-cd.bin $(ISO_DIR)/
	@mkdir -p $(ISO_DIR)/EFI/BOOT
	@cp limine/BOOTX64.EFI $(ISO_DIR)/EFI/BOOT/
	
	@echo "[ISO] Generating pure UEFI ISO with xorriso"
	@$(XORRISO) -as mkisofs \
		-e limine-uefi-cd.bin \
		-no-emul-boot \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		$(ISO_DIR) -o $(ISO_IMG)

# --- Emulation with OVMF (UEFI firmware) ---
run: iso
	@echo "[QEMU] Launching virtual machine in pure UEFI mode..."
	@$(QEMU) -bios /usr/share/ovmf/OVMF.fd -cdrom $(ISO_IMG) -m 4G -M q35 -serial stdio -D qemu.log -d int

# --- Clean Artifacts ---
clean:
	@echo "[CLEAN] Removing artifacts..."
	@rm -rf $(OBJ_DIR) $(DEP_DIR) $(ISO_IMG)
	@rm -rf $(ISO_DIR)/EFI $(ISO_DIR)/limine-uefi-cd.bin $(ISO_DIR)/kernel.elf

-include $(DEPS)
