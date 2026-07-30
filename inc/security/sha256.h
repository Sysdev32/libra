#pragma once
#include <stdint.h>

void sha256_transform(uint32_t state[8], const uint8_t data[64]);
void sha256_hash(const uint8_t* data, uint32_t length, uint8_t output[32]);
