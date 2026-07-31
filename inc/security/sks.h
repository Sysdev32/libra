#pragma once
#include <stdint.h>

void sign_key_with_pid(const uint8_t* key, uint32_t key_len, uint32_t pid, uint8_t out_signature[32]);
uint64_t signature_to_uint64_direct(const uint8_t* signature);