#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if __has_include(<open_hash_kv/kv.h>)
#include <open_hash_kv/kv.h>
#else
#include "kv.h"
#endif

#define FNV_OFFSET_BASIS_32  2166136261U
#define FNV_PRIME_32         16777619U

static struct {
    uint32_t num_slots;
    uint32_t base_addr;
} kv_state = {
    .num_slots = 1000U,
    .base_addr = 0U
};

/* Global structure to track the state of KV slot search */
static struct {
    uint8_t  state;                  // Internal state (enum kv_search_state)
    uint8_t  status;                 // Search status (enum kv_search_status)
    char     key[MAX_KEY_SIZE];      // Key string being searched
    uint8_t  key_len;                // Length of the search key
    uint32_t key_hash;               // 32-bit FNV-1a hash of the key
    uint16_t initial_slot;           // Initial slot derived from key hash
    uint32_t current_slot;           // Current slot index being probed
    uint32_t probe_count;            // Number of slots probed so far
    uint32_t found_slot;             // Slot index where key was located
    struct nvm_kv_slot slot_buf;     // Slot buffer read from NVM
} nvm_kv_search_state;

/* Global structure to track the state of KV write state machine */
static struct {
    uint8_t  state;                      // Internal state (enum kv_write_state)
    uint8_t  status;                     // Write status (enum kv_write_status)
    char     key[MAX_KEY_SIZE];          // Key string being written
    uint8_t  key_len;                    // Length of the key
    uint32_t key_hash;                   // 32-bit FNV-1a hash of the key
    uint8_t  value[VALUE_SIZE];          // Value payload buffer
    uint32_t value_len;                  // Length of the value payload
    uint16_t initial_slot;               // Initial slot derived from key hash
    uint32_t current_slot;               // Current slot index being probed
    uint32_t target_slot;                // Target slot selected for write
    uint32_t first_tombstone_slot;       // First reusable tombstone slot found
    uint32_t probe_count;                // Number of slots probed so far
    struct nvm_kv_slot slot_buf;         // Slot buffer read from NVM
} kv_write_state;

/**
 * @brief Obtains the actual physical NVM slot address from a slot offset (index).
 *
 * Computes the absolute physical byte address in non-volatile memory (NVM) for a given
 * table slot offset. The address is calculated using the configured base address
 * and the fixed aligned size of each NVM slot:
 *
 *   physical_address = base_addr + (slot_offset * sizeof(struct nvm_kv_slot))
 *
 * @param[in] slot_offset Index/offset of the slot in the table [0, num_slots - 1].
 *
 * @return uint32_t Physical byte address of the slot in NVM, or 0xFFFFFFFFU if
 *                  slot_offset is out of bounds (>= num_slots).
 */
uint32_t obtain_nvm_slot_address(uint32_t slot_offset) {
    if (slot_offset >= kv_state.num_slots) {
        return 0xFFFFFFFFU;
    }

    return (kv_state.base_addr + (slot_offset * (uint32_t)sizeof(struct nvm_kv_slot)));
}

void kv_write_reset(void) {
    (void) memset(&kv_write_state, 0, sizeof(kv_write_state));
    kv_write_state.state = KV_WRITE_STATE_IDLE;
    kv_write_state.status = KV_WRITE_STATUS_IDLE;
}

void nvm_kv_search_reset(void) {
    (void) memset(&nvm_kv_search_state, 0, sizeof(nvm_kv_search_state));
    nvm_kv_search_state.state = KV_SEARCH_STATE_IDLE;
    nvm_kv_search_state.status = KV_SEARCH_STATUS_IDLE;
}

/**
 * @brief Initializes the KV store state with the provided base address and slot count.
 *
 * @param[in] base_addr Physical base byte address in NVM where the slot table begins.
 * @param[in] num_slots Total number of slots allocated for the key-value table (must be > 0).
 *
 * @return int32_t 0 on success, -1 if num_slots is 0.
 */
int32_t nvm_kv_init((uint32_t base_addr, uint32_t num_slots) {
    if (num_slots == 0U) {
        return -1;
    }

    kv_state.base_addr = base_addr;
    kv_state.num_slots = num_slots;

    nvm_kv_search_reset();
    kv_write_reset();

    return 0;
}

/**
 * @brief Performs a single probing step in the NVM table.
 *
 * Reads the slot at current_slot via nvm_kv_read and evaluates its slot state
 * using a switch-case block.
 *
 * @param[out] out_slot Optional pointer to store the slot index if found.
 * @return int32_t 0 on success/in-progress, negative error code on failure.
 */
static int32_t nvm_kv_search_probe_single_slot(uint8_t * probe_status) {
    int32_t ret = 0;
    uint32_t slot_addr = 0xFFFFFFFFU;

    /* Obtain the slot address */
    slot_addr = obtain_nvm_slot_address(nvm_kv_search_state.current_slot);
    if (slot_addr == 0xFFFFFFFFU) {
        ret = -1;
    }

    /* Read from the slot */
    ret = nvm_kv_read(slot_addr, (uint8_t *)&nvm_kv_search_state.slot_buf, sizeof(struct nvm_kv_slot));

    if (ret == 0) {
        // Switch case block for slot state specific tasks
        switch (nvm_kv_search_state.slot_buf.state) {
            case SLOT_EMPTY:
            {
                *probe_status = KV_SEARCH_STATUS_NOT_FOUND;
                break;
            }

            case SLOT_VALID:
            {
                if ((nvm_kv_search_state.slot_buf.key_len == nvm_kv_search_state.key_len) &&
                    ((uint32_t)nvm_kv_search_state.slot_buf.key_hash == nvm_kv_search_state.key_hash) &&
                    (memcmp(nvm_kv_search_state.slot_buf.key, nvm_kv_search_state.key, nvm_kv_search_state.key_len) == 0)) {
                    // Key found
                    nvm_kv_search_state.found_slot = nvm_kv_search_state.current_slot;
                    *probe_status = KV_SEARCH_STATUS_FOUND;
                    break;
                }
            }
            /* Mismatch: collision, fall through to advance probe */
            /* fallthrough */

            case SLOT_TOMBSTONE:
            case SLOT_WRITING:
            default:
            {
                /* No valid state found */
                nvm_kv_search_state.probe_count++;
                if (nvm_kv_search_state.probe_count >= kv_state.num_slots) {
                    // All slots probed: key not found
                    *probe_status = KV_SEARCH_STATUS_NOT_FOUND
                } else {
                    nvm_kv_search_state.current_slot = (nvm_kv_search_state.current_slot + 1U) % kv_state.num_slots;
                    *probe_status = KV_SEARCH_STATUS_IN_PROGRESS;
                }
                break;
            }
        }
    }

    return ret;
}

/**
 * @brief Non-blocking state machine function to search for a slot matching a key string.
 *
 * Performs one probe step per invocation without blocking the system loop.
 * Called repeatedly by the system loop until the search completes.
 * Uses switch-case blocks for state-specific tasks.
 *
 * @param[in]  key           Null-terminated key string to search for (used to initiate or continue search).
 * @param[in]  force_stop    Non-zero value to immediately abort any active search and reset to IDLE.
 * @param[out] search_status Pointer to receive the current search status:
 *                           - KV_SEARCH_STATUS_IDLE: No search active
 *                           - KV_SEARCH_STATUS_IN_PROGRESS: Search underway, continue calling
 *                           - KV_SEARCH_STATUS_FOUND: Matching key found
 *                           - KV_SEARCH_STATUS_NOT_FOUND: Key does not exist in table
 *                           - KV_SEARCH_STATUS_STOPPED: Search was aborted via force_stop
 *                           - KV_SEARCH_STATUS_ERROR: Invalid parameter or NVM read failure
 * @param[out] out_slot      Optional pointer to store the slot index if found (can be NULL).
 *
 * @return int32_t 0 on success/in-progress, negative error code on failure.
 */
int32_t nvm_kv_search(const char *key, uint8_t key_len, uint8_t *search_status) {
    uint8_t probe_status;
    uint16_t slot = 0;
    uint32_t fp = 0;
    int32_t ret = 0;

    do {
        /* Validate the parameters */
        if ((key == NULL) ||
            (key_len == 0u) ||
            (key_len > MAX_KEY_SIZE) ||
            (search_status == NULL)) {
            ret = -1;
            break;
        }

        /* Validate the table state */
        if (kv_state.num_slots == 0u) {
            ret = -2;
            break;
        }

        // Switch-case block for search state specific tasks
        switch (nvm_kv_search_state.state) {
            case KV_SEARCH_STATE_IDLE: {
                resolve_key_hash(key, key_len, &slot, &fp);
                /* Clear the state for fresh start */
                (void) memset(&nvm_kv_search_state, 0, sizeof(nvm_kv_search_state));
                /* Copy the key and key length */
                (void) memcpy(nvm_kv_search_state.key, key, key_len);
                nvm_kv_search_state.key_len = (uint8_t)key_len;
                /* Copy the fingerprint */
                nvm_kv_search_state.key_hash = fp;
                /* Initailize the slot counters */
                nvm_kv_search_state.initial_slot = slot;
                nvm_kv_search_state.current_slot = slot;
                nvm_kv_search_state.probe_count = 0;
                /* Intialize the status*/
                nvm_kv_search_state.status = KV_SEARCH_STATUS_IN_PROGRESS;
                /* State change */
                nvm_kv_search_state.state = KV_SEARCH_STATE_PROBING;
                break;
            }

            case KV_SEARCH_STATE_PROBING: {
                ret = nvm_kv_search_probe_single_slot(&probe_status);
                if (probe_status == KV_SEARCH_STATUS_FOUND ||
                    probe_status == KV_SEARCH_STATUS_NOT_FOUND) {
                    nvm_kv_search_state.state = KV_SEARCH_STATE_IDLE;
                    nvm_kv_search_state.status = probe_status;
                }
                break;
            }

            default: {
                nvm_kv_search_state.state = KV_SEARCH_STATE_IDLE;
                nvm_kv_search_state.status = KV_SEARCH_STATUS_ERROR;
                ret = -1;
                break;
            }
        }
    } while(0);

    if (search_status != NULL) {
        *search_status = nvm_kv_search_state.status;
    }

    return ret;
}

uint32_t nvm_kv_search_get_found_slot(void) {
    return nvm_kv_search_state.found_slot;
}

void nvm_kv_search_get_found_value(uint8_t ** buff, uint8_t * buf_len) {
}

/**
 * @brief Deletes an entry at the specified NVM slot offset.
 *
 * Marks the slot as a tombstone (SLOT_TOMBSTONE) and increments the version counter.
 * Using a tombstone ensures open-addressing probe chains are preserved for lookups
 * while allowing future write operations to reuse the slot.
 *
 * If the slot is already SLOT_EMPTY or SLOT_TOMBSTONE, no modification is needed.
 *
 * @param[in] slot_offset Slot index in the table [0, num_slots - 1].
 *
 * @return int32_t 0 on success, negative error code on failure.
 */
int32_t nvm_kv_delete_slot(uint32_t slot_offset) {
    uint32_t slot_addr;
    if (kv_state.num_slots == 0 || slot_offset >= kv_state.num_slots) {
        return -1;
    }

    uint32_t slot_addr = obtain_nvm_slot_address(slot_offset);
    if (slot_addr == 0xFFFFFFFFU) {
        return -1;
    }

    struct nvm_kv_slot slot;
    int32_t ret = nvm_kv_read(slot_addr, (uint8_t *)&slot, sizeof(struct nvm_kv_slot));
    if (ret != 0) {
        return ret;
    }

    // If slot is empty or already a tombstone, no action needed
    if (slot.state == SLOT_EMPTY || slot.state == SLOT_TOMBSTONE) {
        return 0;
    }

    // Clear the slot
    (void) memset(&slot, 0x0, sizeof(struct nvm_kv_slot));
    // Transition slot to tombstone and advance version to prevent ABA
    slot.state = SLOT_TOMBSTONE;

    ret = nvm_kv_write(slot_addr, (const uint8_t *)&slot, sizeof(struct nvm_kv_slot));
    if (ret != 0) {
        return ret;
    }

    return 0;
}

/**
 * @brief Commits the prepared key-value pair to the target NVM slot.
 *
 * @param[out] out_slot Optional pointer to store the slot index if written.
 * @return int32_t 0 on success, negative error code on failure.
 */
static int32_t kv_write_commit_step(uint32_t *out_slot) {
    uint32_t slot_addr = obtain_nvm_slot_address(kv_write_state.target_slot);
    if (slot_addr == 0xFFFFFFFFU) {
        kv_write_state.state = KV_WRITE_STATE_IDLE;
        kv_write_state.status = KV_WRITE_STATUS_ERROR;
        return -1;
    }

    struct nvm_kv_slot old_slot;
    if (nvm_kv_read(slot_addr, (uint8_t *)&old_slot, sizeof(old_slot)) == 0) {
        if (old_slot.state == SLOT_VALID || old_slot.state == SLOT_TOMBSTONE) {
        }
    }

    // Prepare new slot in memory
    struct nvm_kv_slot new_slot;
    (void) memset(&new_slot, 0, sizeof(new_slot));

    new_slot.state = SLOT_WRITING;
    new_slot.version = version;
    new_slot.key_len = kv_write_state.key_len;
    new_slot.key_hash = (uint64_t)kv_write_state.key_hash;

    (void) memcpy(new_slot.key, kv_write_state.key, kv_write_state.key_len);

    if (kv_write_state.value_len > 0) {
        (void) memcpy(new_slot.value, kv_write_state.value, kv_write_state.value_len);
    }

    // Compute CRC32 checksum over (hash + key + value)
    uint32_t crc = 0xFFFFFFFFU;
    crc = crc32_update(crc, &new_slot.key_hash, sizeof(new_slot.key_hash));
    crc = crc32_update(crc, new_slot.key, sizeof(new_slot.key));
    crc = crc32_update(crc, new_slot.value, sizeof(new_slot.value));
    new_slot.checksum = ~crc;

    // Step 1: Write slot content with state = SLOT_WRITING (torn-write protection)
    int32_t ret = nvm_kv_write(slot_addr, (const uint8_t *)&new_slot, sizeof(struct nvm_kv_slot));
    if (ret != 0) {
        kv_write_state.state = KV_WRITE_STATE_IDLE;
        kv_write_state.status = KV_WRITE_STATUS_ERROR;
        return ret;
    }

    // Step 2: Commit slot by atomically setting state = SLOT_VALID
    uint8_t valid_state = SLOT_VALID;
    ret = nvm_kv_write(slot_addr, &valid_state, sizeof(valid_state));
    if (ret != 0) {
        kv_write_state.state = KV_WRITE_STATE_IDLE;
        kv_write_state.status = KV_WRITE_STATUS_ERROR;
        return ret;
    }

    if (out_slot != NULL) {
        *out_slot = kv_write_state.target_slot;
    }

    kv_write_state.state = KV_WRITE_STATE_IDLE;
    kv_write_state.status = KV_WRITE_STATUS_SUCCESS;
    return 0;
}

/**
 * @brief Performs a single probing step for the write state machine.
 *
 * Reads candidate slot at current_slot and evaluates it via switch-case block.
 *
 * @param[out] out_slot Optional pointer to store the slot index if written.
 * @return int32_t 0 on success/in-progress, negative error code on failure.
 */
static int32_t kv_write_probe_step(uint32_t *out_slot) {
    uint32_t slot_addr = obtain_nvm_slot_address(kv_write_state.current_slot);
    if (slot_addr == 0xFFFFFFFFU) {
        kv_write_state.state = KV_WRITE_STATE_IDLE;
        kv_write_state.status = KV_WRITE_STATUS_ERROR;
        return -1;
    }

    int32_t read_ret = nvm_kv_read(slot_addr, (uint8_t *)&kv_write_state.slot_buf, sizeof(struct nvm_kv_slot));
    if (read_ret != 0) {
        kv_write_state.state = KV_WRITE_STATE_IDLE;
        kv_write_state.status = KV_WRITE_STATUS_ERROR;
        return read_ret;
    }

    switch (kv_write_state.slot_buf.state) {
        case SLOT_EMPTY:
            // Key is not in table. Use first seen tombstone if available, otherwise current slot
            if (kv_write_state.first_tombstone_slot != 0xFFFFFFFFU) {
                kv_write_state.target_slot = kv_write_state.first_tombstone_slot;
            } else {
                kv_write_state.target_slot = kv_write_state.current_slot;
            }
            kv_write_state.state = KV_WRITE_STATE_WRITING;
            return kv_write_commit_step(out_slot);

        case SLOT_VALID:
            // Check if key matches an existing entry (update case)
            if (kv_write_state.slot_buf.key_len == kv_write_state.key_len &&
                (uint32_t)kv_write_state.slot_buf.key_hash == kv_write_state.key_hash &&
                memcmp(kv_write_state.slot_buf.key, kv_write_state.key, kv_write_state.key_len) == 0) {
                // Key exists: update this slot
                kv_write_state.target_slot = kv_write_state.current_slot;
                kv_write_state.state = KV_WRITE_STATE_WRITING;
                return kv_write_commit_step(out_slot);
            }
            /* Collision with another key: fall through to advance probe */
            /* fallthrough */

        case SLOT_TOMBSTONE:
        case SLOT_WRITING:
        default:
            // Remember first available tombstone slot
            if (kv_write_state.slot_buf.state == SLOT_TOMBSTONE &&
                kv_write_state.first_tombstone_slot == 0xFFFFFFFFU) {
                kv_write_state.first_tombstone_slot = kv_write_state.current_slot;
            }

            kv_write_state.probe_count++;
            if (kv_write_state.probe_count >= kv_state.num_slots) {
                // Entire table searched
                if (kv_write_state.first_tombstone_slot != 0xFFFFFFFFU) {
                    kv_write_state.target_slot = kv_write_state.first_tombstone_slot;
                    kv_write_state.state = KV_WRITE_STATE_WRITING;
                    return kv_write_commit_step(out_slot);
                } else {
                    // No empty or tombstone slot available
                    kv_write_state.state = KV_WRITE_STATE_IDLE;
                    kv_write_state.status = KV_WRITE_STATUS_FULL;
                }
            } else {
                kv_write_state.current_slot = (kv_write_state.current_slot + 1U) % kv_state.num_slots;
                kv_write_state.status = KV_WRITE_STATUS_IN_PROGRESS;
            }
            break;
    }

    return 0;
}

/**
 * @brief Non-blocking state machine function to insert/update a key-value pair.
 *
 * Derives the slot index internally based on the key value (hashing and probing).
 * Performs one probe/write step per invocation without blocking the system loop.
 *
 * @param[in]  key          Null-terminated key string (max MAX_KEY_SIZE bytes).
 * @param[in]  value        Pointer to value payload buffer (can be NULL if value_len is 0).
 * @param[in]  value_len    Length of value payload in bytes (must be <= VALUE_SIZE).
 * @param[in]  force_stop   Non-zero value to immediately abort any active write and reset to IDLE.
 * @param[out] write_status Pointer to receive the current write status:
 *                          - KV_WRITE_STATUS_IDLE: No write active
 *                          - KV_WRITE_STATUS_IN_PROGRESS: Searching/writing underway, continue calling
 *                          - KV_WRITE_STATUS_SUCCESS: Key-value pair committed successfully
 *                          - KV_WRITE_STATUS_FULL: Table is full, no slot available
 *                          - KV_WRITE_STATUS_STOPPED: Write was aborted via force_stop
 *                          - KV_WRITE_STATUS_ERROR: Invalid parameter or NVM I/O failure
 * @param[out] out_slot     Optional pointer to store the slot index where data was written.
 *
 * @return int32_t 0 on success/in-progress, negative error code on failure.
 */
int32_t kv_write(const char *key, const void *value, uint32_t value_len, uint8_t force_stop, uint8_t *write_status, uint32_t *out_slot) {
    int32_t ret = 0;

    // 1. Force stop check
    if (force_stop != 0) {
        kv_write_state.state = KV_WRITE_STATE_IDLE;
        kv_write_state.status = KV_WRITE_STATUS_STOPPED;
        if (write_status != NULL) {
            *write_status = KV_WRITE_STATUS_STOPPED;
        }
        return 0;
    }

    // 2. Validate table configuration
    if (kv_state.num_slots == 0) {
        kv_write_state.state = KV_WRITE_STATE_IDLE;
        kv_write_state.status = KV_WRITE_STATUS_ERROR;
        if (write_status != NULL) {
            *write_status = KV_WRITE_STATUS_ERROR;
        }
        return -1;
    }

    // 3. State machine using switch-case block
    switch (kv_write_state.state) {
        case KV_WRITE_STATE_IDLE: {
            if (key == NULL) {
                if (write_status != NULL) {
                    *write_status = KV_WRITE_STATUS_IDLE;
                }
                return 0;
            }

            size_t klen = strlen(key);
            if (klen == 0 || klen > MAX_KEY_SIZE) {
                kv_write_state.status = KV_WRITE_STATUS_ERROR;
                if (write_status != NULL) {
                    *write_status = KV_WRITE_STATUS_ERROR;
                }
                return -1;
            }

            if (value_len > VALUE_SIZE || (value == NULL && value_len > 0)) {
                kv_write_state.status = KV_WRITE_STATUS_ERROR;
                if (write_status != NULL) {
                    *write_status = KV_WRITE_STATUS_ERROR;
                }
                return -1;
            }

            // Initialize new write request
            memset(&kv_write_state, 0, sizeof(kv_write_state));
            memcpy(kv_write_state.key, key, klen);
            kv_write_state.key_len = (uint8_t)klen;
            kv_write_state.key_hash = hash32(key, klen);

            if (value != NULL && value_len > 0) {
                memcpy(kv_write_state.value, value, value_len);
                kv_write_state.value_len = value_len;
            }

            // Derive initial slot index internally from key
            uint16_t slot = 0;
            uint32_t fp = 0;
            resolve_key_hash(key, klen, &slot, &fp);
            kv_write_state.initial_slot = slot;
            kv_write_state.current_slot = slot;
            kv_write_state.target_slot = 0xFFFFFFFFU;
            kv_write_state.first_tombstone_slot = 0xFFFFFFFFU;
            kv_write_state.probe_count = 0;
            kv_write_state.state = KV_WRITE_STATE_SEARCHING;
            kv_write_state.status = KV_WRITE_STATUS_IN_PROGRESS;

            ret = kv_write_probe_step(out_slot);
            break;
        }

        case KV_WRITE_STATE_SEARCHING:
            ret = kv_write_probe_step(out_slot);
            break;

        case KV_WRITE_STATE_WRITING:
            ret = kv_write_commit_step(out_slot);
            break;

        default:
            kv_write_state.state = KV_WRITE_STATE_IDLE;
            kv_write_state.status = KV_WRITE_STATUS_ERROR;
            ret = -1;
            break;
    }

    if (write_status != NULL) {
        *write_status = kv_write_state.status;
    }

    return ret;
}

int32_t kv_put(const char *key, const void *value, uint32_t value_len, uint8_t force_stop, uint8_t *write_status, uint32_t *out_slot) {
    return kv_write(key, value, value_len, force_stop, write_status, out_slot);
}

uint32_t kv_write_get_written_slot(void) {
    return kv_write_state.target_slot;
}

