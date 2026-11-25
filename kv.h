#ifndef KV_H_
#define KV_H_

#define MAX_KEY_SIZE 32
#define VALUE_SIZE   128

// Slot states for crash-consistent state transitions
enum slot_state {
    SLOT_EMPTY    = 0x0, // Available for allocation
    SLOT_WRITING  = 0x1, // Torn-write protection: currently being updated
    SLOT_VALID    = 0x2, // Committed and readable
    SLOT_TOMBSTONE= 0x3  // Deleted (prevents breaking linear/quadratic probe chains)
};

// Align to 64 bytes (CPU cache line size) to prevent false sharing and optimize flushes
struct alignas(64) nvm_kv_slot {
    /* --- Cache Line 0: Metadata & Key (64 Bytes) --- */
    
    // 8-byte atomic commit header:
    // Updating this via a single 8-byte atomic store + clwb/sfence commits the slot.
    uint8_t  state;          // 1 byte:  enum slot_state
    uint16_t version;        // 2 bytes: Generation counter (prevents ABA issues / supports lock-free reads)
    uint8_t  key_len;        // 1 byte:  Actual length of the key (0 to MAX_KEY_SIZE)
    uint32_t checksum;       // 4 bytes: CRC32 of (hash + key + value) to detect torn writes/bit-rot
    
    uint64_t key_hash;       // 8 bytes: 32-bit hash FNV-1a (remaining 32 bits are reserved for future usage)
    
    char     key[MAX_KEY_SIZE]; // 32 bytes: Inline fixed-size key
    
    uint8_t  reserved[16];   // 16 bytes: Explicit padding to align 'value' to Cache Line 1
                             // (Can also be used for TTL timestamps or extra key bytes)

    /* --- Cache Lines 1 & 2: Payload (128 Bytes) --- */
    uint8_t  value[VALUE_SIZE]; // 128 bytes: Actual user data
};

#endif /* KV_H_ */
