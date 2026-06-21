#!/bin/sh
set -e # Exit immediately if any compilation command fails
# 1. Assemble the User Space Entry Point
nasm -f elf64 crt0.asm -o crt0.o

# 2. Compile System Call Stubs and C Code Source Files
x86_64-elf-gcc -ffreestanding -mno-red-zone -I./include -O2 -c stubs.c -o stubs.o -g -mno-sse
x86_64-elf-gcc -ffreestanding -mno-red-zone -I./include -O2 -c user.c -o user.o -g -mno-sse

# 3. Link Executable forcing 4KB Page Alignments (-z)
# FIXED: Added --start-group and --end-group to resolve cross-archive dependencies.
x86_64-elf-ld -nostdlib \
    -z max-page-size=0x1000 \
    -z common-page-size=0x1000 \
    -T user.ld \
    crt0.o stubs.o user.o \
    --start-group \
    micropython.a \
    prebuilt/libc.a \
    prebuilt/libm.a \
    --end-group \
    -o user_app.elf

# 4. Export Outputs for Kernel VFS Environment Ingestion
cp user_app.elf rootfs/user_app.elf
x86_64-elf-objcopy -O binary user_app.elf rootfs/user.bin

echo "[SUCCESS] User application compiled tightly for 4KB runtime mapping layouts."
