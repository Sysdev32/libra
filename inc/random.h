#pragma once
#include <stdint.h>

void rng_init();
uint64_t rng_next();
uint64_t rng_range(uint64_t min, uint64_t max);