#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#if __has_include(<open_hash_kv/kv.h>)
#include <open_hash_kv/kv.h>
#else
#include "kv.h"
#endif

/**
 * @brief Weak stub function to read bytes from Non-Volatile Memory (NVM).
 *
 * This function should be overridden by the platform/hardware adapter to perform
 * actual read operations from the underlying NVM storage (e.g. FRAM, Flash, PMEM).
 *
 * @param[in]  address Physical byte address in NVM to read from.
 * @param[out] buff    Destination buffer to store the read bytes.
 * @param[in]  len     Number of bytes to read.
 *
 * @return int32_t 0 on success, negative error code on failure.
 */
int32_t __attribute__((weak)) nvm_kv_read(uint32_t address, uint8_t *buff, uint32_t len) {
    (void)address;
    (void)buff;
    (void)len;
    return 0;
}

/**
 * @brief Weak stub function to write bytes to Non-Volatile Memory (NVM).
 *
 * This function should be overridden by the platform/hardware adapter to perform
 * actual write operations to the underlying NVM storage (e.g. FRAM, Flash, PMEM).
 *
 * @param[in] address Physical byte address in NVM to write to.
 * @param[in] buff    Source buffer containing the data to write.
 * @param[in] len     Number of bytes to write.
 *
 * @return int32_t 0 on success, negative error code on failure.
 */
int32_t __attribute__((weak)) nvm_kv_write(uint32_t address, const uint8_t *buff, uint32_t len) {
    (void)address;
    (void)buff;
    (void)len;
    return 0;
}
