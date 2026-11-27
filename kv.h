#ifndef KV_H_
#define KV_H_

#include <stdint.h>
#include <stddef.h>

#define MAX_KEY_SIZE 32u
#define VALUE_SIZE   128u

// Slot states for crash-consistent state transitions
enum slot_state {
    SLOT_EMPTY    = 0x0, // Available for allocation
    SLOT_WRITING  = 0x1, // Torn-write protection: currently being updated
    SLOT_VALID    = 0x2, // Committed and readable
    SLOT_TOMBSTONE= 0x3  // Deleted (prevents breaking linear/quadratic probe chains)
};

// Search status codes returned by nvm_kv_search
enum kv_search_status {
    KV_SEARCH_STATUS_IDLE        = 0x0,
    KV_SEARCH_STATUS_IN_PROGRESS = 0x1,
    KV_SEARCH_STATUS_FOUND       = 0x2,
    KV_SEARCH_STATUS_NOT_FOUND   = 0x3,
    KV_SEARCH_STATUS_STOPPED     = 0x4,
    KV_SEARCH_STATUS_ERROR       = 0x5
};

// Internal states for search state machine
enum kv_search_state {
    KV_SEARCH_STATE_IDLE    = 0x0,
    KV_SEARCH_STATE_PROBING = 0x1
};

// Write status codes returned by kv_write
enum kv_write_status {
    KV_WRITE_STATUS_IDLE        = 0x0,
    KV_WRITE_STATUS_IN_PROGRESS = 0x1,
    KV_WRITE_STATUS_SUCCESS     = 0x2,
    KV_WRITE_STATUS_FULL        = 0x3,
    KV_WRITE_STATUS_STOPPED     = 0x4,
    KV_WRITE_STATUS_ERROR       = 0x5
};

// Internal states for write state machine
enum kv_write_state {
    KV_WRITE_STATE_IDLE      = 0x0,
    KV_WRITE_STATE_SEARCHING = 0x1,
    KV_WRITE_STATE_WRITING   = 0x2
};

// Align to 64 bytes (CPU cache line size) to prevent false sharing and optimize flushes
struct nvm_kv_slot {
    /* --- Cache Line 0: Metadata & Key (64 Bytes) --- */
    
    // 8-byte atomic commit header:
    // Updating this via a single 8-byte atomic store + clwb/sfence commits the slot.
    uint8_t  state;          // 1 byte:  enum slot_state
    uint8_t  key_len;        // 1 byte:  Actual length of the key (0 to MAX_KEY_SIZE)
    uint32_t checksum;       // 4 bytes: CRC32 of (hash + key + value) to detect torn writes/bit-rot
    
    uint64_t key_hash;       // 8 bytes: 32-bit hash FNV-1a (remaining 32 bits are reserved for future usage)
    
    char     key[MAX_KEY_SIZE]; // 32 bytes: Inline fixed-size key
    
    uint8_t  reserved[18];   // 18 bytes: Explicit padding to align 'value' to Cache Line 1
                             // (Can also be used for TTL timestamps or extra key bytes)

    /* --- Cache Lines 1 & 2: Payload (128 Bytes) --- */
    uint8_t  value[VALUE_SIZE]; // 128 bytes: Actual user data
} __attribute__((__packed__));

/* Wrappers to read and write to NVM storage */
int32_t nvm_kv_read(uint32_t address, uint8_t *buff, uint32_t len);
int32_t nvm_kv_write(uint32_t address, const uint8_t *buff, uint32_t len);

uint32_t obtain_nvm_slot_address(uint32_t slot_offset);

int32_t nvm_kv_init(uint32_t base_addr, uint32_t num_slots);

int32_t nvm_kv_search(const char *key, uint8_t force_stop, uint8_t *search_status, uint32_t *out_slot);
uint32_t nvm_kv_search_get_found_slot(void);
void nvm_kv_search_reset(void);
int32_t nvm_kv_delete_slot(uint32_t slot_offset);

int32_t kv_insert_slot(uint32_t slot_offset, const char *key, const void *value, uint32_t value_len);

int32_t kv_write(const char *key, const void *value, uint32_t value_len, uint8_t force_stop, uint8_t *write_status, uint32_t *out_slot);
int32_t kv_write_entry(const char *key, const void *value, uint32_t value_len, uint8_t force_stop, uint8_t *write_status, uint32_t *out_slot);
int32_t kv_put(const char *key, const void *value, uint32_t value_len, uint8_t force_stop, uint8_t *write_status, uint32_t *out_slot);
uint32_t kv_write_get_written_slot(void);
void kv_write_reset(void);

#endif /* KV_H_ */
