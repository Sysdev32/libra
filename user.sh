#!/bin/sh
nasm -f elf64 crt0.asm -o crt0.o
x86_64-elf-gcc -mno-red-zone -I./include -O2 -c stubs.c -o stubs.o -g -mno-sse
x86_64-elf-gcc -mno-red-zone -I./include -O2 -c user.c -o user.o -g -mno-sse
x86_64-elf-gcc -nostdlib -T user.ld crt0.o stubs.o user.o prebuilt/libc.a -o user_app.elf -g -mno-sse
x86_64-elf-objcopy -O binary user_app.elf rootfs/user.bin
