#include <stdio.h>
#include <open_hash_kv/kv.h>

#define FNV_OFFSET_BASIS_32  2166136261U
#define FNV_PRIME_32         16777619U

static uint32_t kv_num_slots = 1000U;

/**
 * @brief Computes a 32-bit FNV-1a hash over arbitrary key bytes.
 * 
 * @param key Pointer to key buffer (binary or string)
 * @param len Length of the key in bytes
 * @return uint32_t Full 32-bit hash value
 */
static uint32_t hash32(const void *key, size_t len) {
    const uint8_t *data = (const uint8_t *)key;
    uint32_t hash = FNV_OFFSET_BASIS_32;

    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= FNV_PRIME_32;
    }

    return hash;
}

/**
 * @brief Resolves a key into its initial table slot index and a fast-comparison fingerprint.
 *
 * Computes a 32-bit FNV-1a hash over the provided key bytes and derives two
 * independent values:
 * 1. The primary slot index for open-addressing table probing (via modulo arithmetic).
 * 2. A 16-bit fingerprint extracted from the upper half of the 32-bit hash, used
 *    in slot headers to quickly filter out non-matching records over the serial bus
 *    before reading the full key string.
 *
 * @note The fingerprint is clamped to ensure it never evaluates to 0x0000, preventing
 *       accidental matches with zeroed or unprogrammed memory cells.
 *
 * @param[in]  key              Pointer to the contiguous byte buffer containing the key.
 *                              Must not be NULL if len > 0.
 * @param[in]  len              Length of the key in bytes (binary data or string excluding
 *                              null-terminator unless intended as part of the key).
 * @param[out] out_slot         Pointer to store the calculated initial slot index 
 *                              [0, NUM_SLOTS - 1]. Must not be NULL.
 * @param[out] out_fingerprint  Pointer to store the 16-bit non-zero hash fingerprint.
 *                              Must not be NULL.
 *
 * @return void
 */
static void resolve_key_hash(const void *key, size_t len, uint16_t *out_slot, uint16_t *out_fingerprint) {
    uint32_t h = hash32(key, len);

    // 1. Calculate the initial table slot (0 to NUM_SLOTS - 1)
    *out_slot = (uint16_t)(h % kv_num_slots);

    // 2. Extract upper 16 bits as the fast-verification fingerprint
    //    (Using upper bits avoids correlation with the lower-bit modulo index)
    uint16_t fp = (uint16_t)(h >> 16);
    
    // Ensure fingerprint is non-zero so it never collides with cleared/uninitialized bytes
    if (fp == 0) {
        fp = 1;
    }
    *out_fingerprint = fp;
}
