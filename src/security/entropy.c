#include <stdint.h>
uint32_t random(uint64_t* state) {
    uint64_t old_state = *state;
    
    // Advance internal state using a Linear Congruential Generator (LCG)
    *state = old_state * 6364136223846793005ULL + 1442695040888963407ULL;
    
    // Calculate PCG-XSH-RR output transformation
    uint32_t xorshifted = (uint32_t)(((old_state >> 18u) ^ old_state) >> 27u);
    uint32_t rot = (uint32_t)(old_state >> 59u);
    
    // Return rotated value
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}