### How to build a program using this SDK

1. Create a toolchain with the sysroot provided in the SDK
2. Compile statically with the sysroot
3. Add the program to the rootfs
4. Edit launchd configuration in order for it to load your program as a service on boot
5. Enjoy, and have fun!