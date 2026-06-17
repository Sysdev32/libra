#include <stdint.h>

#define POOL_SIZE 16

static uint64_t pool[POOL_SIZE];
static uint64_t pool_pos = 0;
static uint32_t s[16];
static int init = 0;
static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}
void rng_add_entropy(uint64_t x) {
    x ^= rdtsc();
    x ^= (uint64_t)&x;
    x = mix64(x);

    pool[pool_pos++ % POOL_SIZE] ^= x;
}
#define ROTL(a,b) (((a) << (b)) | ((a) >> (32 - (b))))

static inline void quarter_round(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    *a += *b; *d ^= *a; *d = ROTL(*d, 16);
    *c += *d; *b ^= *c; *b = ROTL(*b, 12);
    *a += *b; *d ^= *a; *d = ROTL(*d, 8);
    *c += *d; *b ^= *c; *b = ROTL(*b, 7);
}
void rng_init() {
    rng_add_entropy(rdtsc());
    rng_add_entropy((uint64_t)&pool);
    rng_add_entropy(0xdeadbeef ^ rdtsc());
    for (int i = 0; i < 16; i++) {
        uint64_t e = pool[i] ^ rdtsc();
        s[i] = (uint32_t)mix64(e);
    }

    init = 1;
}
uint64_t rng_next() {
    if (!init) rng_init();

    for (int i = 0; i < 16; i += 4) {
        quarter_round(&s[i], &s[i+1], &s[i+2], &s[i+3]);
    }

    return ((uint64_t)s[0] << 32) | s[1];
}
uint64_t rng_range(uint64_t min, uint64_t max) {
    return min + (rng_next() % (max - min + 1));
}