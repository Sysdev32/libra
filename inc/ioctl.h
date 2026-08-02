#pragma once
#define IOBLKSZ 0 // io block size
#define IOBLKSC 1 // io block sectors
#define FBSZXY 2 // FB Size X/Y
#define FBCRES 3 // set fb res
#define ETHMAC 4 // mac address
int ioctl(int fd, unsigned long request, void* arg);