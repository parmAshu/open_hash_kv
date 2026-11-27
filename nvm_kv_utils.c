#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if __has_include(<open_hash_kv/kv.h>)
#include <open_hash_kv/kv.h>
#else
#include "kv.h"
#endif

/**
 * @brief Computes a 32-bit FNV-1a hash over arbitrary key bytes.
 * 
 * @param key Pointer to key buffer (binary or string)
 * @param len Length of the key in bytes
 * @return uint32_t Full 32-bit hash value
 */
uint32_t hash32(const void *key, size_t len) {
    const uint8_t *data = (const uint8_t *)key;
    uint32_t hash = FNV_OFFSET_BASIS_32;

    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= FNV_PRIME_32;
    }

    return hash;
}

/**
 * @brief Computes standard IEEE 802.3 CRC32 over arbitrary data bytes.
 *
 * @param crc  Initial/running CRC32 value.
 * @param data Pointer to input data buffer.
 * @param len  Length of data buffer in bytes.
 * @return uint32_t Updated CRC32 value.
 */
uint32_t crc32_update(uint32_t crc, const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (-(crc & 1U)));
        }
    }
    return crc;
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
 * @param[out] out_fingerprint  Pointer to store the 32-bit non-zero hash fingerprint.
 *                              Must not be NULL.
 *
 * @return void
 */
void resolve_key_hash(const void *key, size_t len, uint16_t *out_slot, uint32_t *out_fingerprint) {
    uint32_t h = hash32(key, len);

    // Calculate the initial table slot (0 to NUM_SLOTS - 1)
    *out_slot = (uint16_t)(h % kv_state.num_slots);
    // save the 32 bit fingerprint in the destination variable
    *out_fingerprint = h;
}
