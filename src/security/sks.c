#include <stdint.h>
// Signs a data key combined with a specific process identifier (PID)
void sign_key_with_pid(const uint8_t* key, uint32_t key_len, uint32_t pid, uint8_t out_signature[32]) {
    uint8_t k_ipad[64] = {0};
    uint8_t k_opad[64] = {0};
    
    // Handle keys larger than block size (64 bytes)
    uint8_t prepared_key[64] = {0};
    if (key_len > 64) {
        sha256_hash(key, key_len, prepared_key);
    } else {
        for (uint32_t i = 0; i < key_len; i++) prepared_key[i] = key[i];
    }

    // XOR key with inner and outer padding constants
    for (int i = 0; i < 64; i++) {
        k_ipad[i] = prepared_key[i] ^ 0x36;
        k_opad[i] = prepared_key[i] ^ 0x5C;
    }

    // Combine inner padding with the 4-byte PID payload
    uint8_t inner_buffer[64 + 4];
    for (int i = 0; i < 64; i++) inner_buffer[i] = k_ipad[i];
    inner_buffer[64] = (pid >> 24) & 0xFF;
    inner_buffer[65] = (pid >> 16) & 0xFF;
    inner_buffer[66] = (pid >> 8) & 0xFF;
    inner_buffer[67] = pid & 0xFF;

    // First hash pass
    uint8_t inner_hash[32];
    sha256_hash(inner_buffer, 68, inner_hash);

    // Combine outer padding with inner hash result
    uint8_t outer_buffer[64 + 32];
    for (int i = 0; i < 64; i++) outer_buffer[i] = k_opad[i];
    for (int i = 0; i < 32; i++) outer_buffer[64 + i] = inner_hash[i];

    // Second hash pass to yield the final signature
    sha256_hash(outer_buffer, 96, out_signature);
}
uint64_t signature_to_uint64_direct(const uint8_t* signature) {
    uint64_t result;
    // Copies the first 8 bytes of the signature directly into the uint64_t
    // Works safely across alignment boundaries
    for (int i = 0; i < 8; i++) {
        ((uint8_t*)&result)[i] = signature[i];
    }
    return result;
}