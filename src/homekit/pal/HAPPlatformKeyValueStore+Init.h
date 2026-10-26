/**
 * @file HAPPlatformKeyValueStore+Init.h
 * @brief HomeKit ADK key-value store on Nordic FDS (flash data storage).
 */
#ifndef HAP_PLATFORM_KEY_VALUE_STORE_INIT_H
#define HAP_PLATFORM_KEY_VALUE_STORE_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "HAPPlatform.h"

/** Largest value that can be stored. */
#ifndef HAP_PLATFORM_KEY_VALUE_STORE_MAX_VALUE_BYTES
#define HAP_PLATFORM_KEY_VALUE_STORE_MAX_VALUE_BYTES 256
#endif

struct HAPPlatformKeyValueStore {
    // Opaque type. Do not access the instance fields directly.
    uint16_t baseFileID;
};

typedef struct {
    /**
     * FDS file ID of domain 0. Domain d is stored in file baseFileID + d.
     * The 256 file IDs starting here must not be used by anything else.
     */
    uint16_t baseFileID;
} HAPPlatformKeyValueStoreOptions;

/**
 * Initializes the key-value store. FDS must already be initialized (ri_flash_init).
 *
 * Must only be used from main context: operations block until flash writes complete.
 */
HAP_RESULT_USE_CHECK
HAPError HAPPlatformKeyValueStoreCreate(
        HAPPlatformKeyValueStoreRef keyValueStore,
        const HAPPlatformKeyValueStoreOptions* options);

/**
 * Deletes records from a file not owned by the key-value store, e.g. stale application data.
 */
HAP_RESULT_USE_CHECK
HAPError HAPPlatformKeyValueStoreDeleteFile(HAPPlatformKeyValueStoreRef keyValueStore, uint16_t fileID);

/**
 * Runs garbage collection if at least minDirtyRecords records are deleted.
 *
 * Erasing a flash page takes ~85 ms. The SoftDevice can only schedule that between radio
 * events, so compact while not connected rather than letting a write run out of space
 * in the middle of a HomeKit session.
 */
HAP_RESULT_USE_CHECK
HAPError HAPPlatformKeyValueStoreCompact(HAPPlatformKeyValueStoreRef keyValueStore, size_t minDirtyRecords);

#ifdef __cplusplus
}
#endif

#endif
