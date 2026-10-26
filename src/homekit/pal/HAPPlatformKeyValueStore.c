/**
 * @file HAPPlatformKeyValueStore.c
 * @brief HomeKit ADK key-value store on Nordic FDS (flash data storage).
 *
 * Mapping: domain -> FDS file (baseFileID + domain), key -> FDS record key (key + 1,
 * since record key 0 is reserved). Each record starts with a 4 byte header holding
 * the value length, as FDS only tracks lengths in words.
 *
 * FDS operations are asynchronous; completion events arrive from SoftDevice
 * interrupt context. All functions block in main context until completion.
 */

#include "HAPPlatform.h"
#include "HAPPlatformKeyValueStore+Init.h"

#include "app_util_platform.h"
#include "fds.h"
#include "ruuvi_interface_yield.h"

static const HAPLogObject logObject = { .subsystem = kHAPPlatform_LogSubsystem, .category = "KeyValueStore" };

#define kHeaderBytes ((size_t) 4)

typedef struct {
    volatile bool isPending;
    volatile ret_code_t result;
    fds_evt_id_t id;
    uint32_t recordID;  /**< For write / update / delete record. */
    uint16_t fileID;    /**< For delete file. */
} PendingOperation;

static PendingOperation pending;
static bool isRegistered;

/** Write buffer. FDS requires word alignment and that data stays valid until the write completes. */
static uint32_t writeWords[(kHeaderBytes + HAP_PLATFORM_KEY_VALUE_STORE_MAX_VALUE_BYTES + 3) / 4];

static void HandleFDSEvent(fds_evt_t const* evt) {
    // Interrupt context. FDS reports every user's operations to every handler: match ours.
    if (!pending.isPending || evt->id != pending.id) {
        return;
    }
    switch (evt->id) {
        case FDS_EVT_WRITE:
        case FDS_EVT_UPDATE:
            if (evt->write.record_id != pending.recordID) {
                return;
            }
            break;
        case FDS_EVT_DEL_RECORD:
            if (evt->del.record_id != pending.recordID) {
                return;
            }
            break;
        case FDS_EVT_DEL_FILE:
            if (evt->del.file_id != pending.fileID) {
                return;
            }
            break;
        default:
            break;
    }
    pending.result = evt->result;
    pending.isPending = false;
}

static void Begin(fds_evt_id_t id) {
    HAPAssert(!pending.isPending);
    pending.id = id;
    pending.recordID = 0;
    pending.fileID = 0;
    pending.result = FDS_SUCCESS;
    pending.isPending = true;
}

/** Waits for the pending operation, whose ID fields must be filled in by now. */
HAP_RESULT_USE_CHECK
static ret_code_t Wait(void) {
    while (pending.isPending) {
        (void) ri_yield();
    }
    return pending.result;
}

static void Cancel(void) {
    pending.isPending = false;
}

HAP_RESULT_USE_CHECK
static ret_code_t RunGarbageCollection(void) {
    Begin(FDS_EVT_GC);
    ret_code_t e = fds_gc();
    if (e != FDS_SUCCESS) {
        Cancel();
        return e;
    }
    return Wait();
}

HAP_RESULT_USE_CHECK
static bool FindRecord(
        HAPPlatformKeyValueStoreRef keyValueStore,
        HAPPlatformKeyValueStoreDomain domain,
        HAPPlatformKeyValueStoreKey key,
        fds_record_desc_t* desc) {
    fds_find_token_t token;
    HAPRawBufferZero(&token, sizeof token);
    HAPRawBufferZero(desc, sizeof *desc);
    return fds_record_find(
                   (uint16_t)(keyValueStore->baseFileID + domain), (uint16_t)(key + 1U), desc, &token) ==
           FDS_SUCCESS;
}

HAPError HAPPlatformKeyValueStoreCreate(
        HAPPlatformKeyValueStoreRef keyValueStore,
        const HAPPlatformKeyValueStoreOptions* options) {
    HAPPrecondition(keyValueStore);
    HAPPrecondition(options);
    HAPPrecondition(options->baseFileID + 0xFFU < 0xC000U);

    keyValueStore->baseFileID = options->baseFileID;
    if (!isRegistered) {
        ret_code_t e = fds_register(HandleFDSEvent);
        if (e != FDS_SUCCESS) {
            HAPLogError(&logObject, "fds_register failed: 0x%04lX.", (unsigned long) e);
            return kHAPError_Unknown;
        }
        isRegistered = true;
    }
    return kHAPError_None;
}

HAPError HAPPlatformKeyValueStoreGet(
        HAPPlatformKeyValueStoreRef keyValueStore,
        HAPPlatformKeyValueStoreDomain domain,
        HAPPlatformKeyValueStoreKey key,
        void* _Nullable bytes,
        size_t maxBytes,
        size_t* _Nullable numBytes,
        bool* found) {
    HAPPrecondition(keyValueStore);
    HAPPrecondition(!maxBytes || bytes);
    HAPPrecondition((bytes == NULL) == (numBytes == NULL));
    HAPPrecondition(found);

    *found = false;
    fds_record_desc_t desc;
    if (!FindRecord(keyValueStore, domain, key, &desc)) {
        return kHAPError_None;
    }

    fds_flash_record_t record;
    ret_code_t e = fds_record_open(&desc, &record);
    if (e != FDS_SUCCESS) {
        HAPLogError(&logObject, "fds_record_open failed: 0x%04lX.", (unsigned long) e);
        return kHAPError_Unknown;
    }
    const uint8_t* data = record.p_data;
    size_t numStoredBytes = (size_t) data[0] | ((size_t) data[1] << 8U);
    size_t capacity = (size_t) record.p_header->length_words * 4U;
    if (capacity < kHeaderBytes || numStoredBytes > capacity - kHeaderBytes) {
        (void) fds_record_close(&desc);
        HAPLogError(&logObject, "Corrupted record %02X.%02X.", domain, key);
        return kHAPError_Unknown;
    }
    *found = true;
    if (bytes) {
        *numBytes = numStoredBytes < maxBytes ? numStoredBytes : maxBytes;
        HAPRawBufferCopyBytes(HAPNonnullVoid(bytes), &data[kHeaderBytes], *numBytes);
    }
    (void) fds_record_close(&desc);
    return kHAPError_None;
}

HAPError HAPPlatformKeyValueStoreSet(
        HAPPlatformKeyValueStoreRef keyValueStore,
        HAPPlatformKeyValueStoreDomain domain,
        HAPPlatformKeyValueStoreKey key,
        const void* bytes,
        size_t numBytes) {
    HAPPrecondition(keyValueStore);
    HAPPrecondition(bytes);

    if (numBytes > HAP_PLATFORM_KEY_VALUE_STORE_MAX_VALUE_BYTES) {
        HAPLogError(&logObject, "Value for %02X.%02X too large (%zu bytes).", domain, key, numBytes);
        return kHAPError_Unknown;
    }

    uint8_t* w = (uint8_t*) writeWords;
    HAPRawBufferZero(writeWords, sizeof writeWords);
    w[0] = (uint8_t)(numBytes & 0xFFU);
    w[1] = (uint8_t)(numBytes >> 8U);
    HAPRawBufferCopyBytes(&w[kHeaderBytes], bytes, numBytes);

    fds_record_t record = {
        .file_id = (uint16_t)(keyValueStore->baseFileID + domain),
        .key = (uint16_t)(key + 1U),
        .data = { .p_data = writeWords, .length_words = (uint16_t)((kHeaderBytes + numBytes + 3U) / 4U) },
    };

    for (int attempt = 0; attempt < 2; attempt++) {
        fds_record_desc_t desc;
        bool exists = FindRecord(keyValueStore, domain, key, &desc);

        ret_code_t e;
        // The new record ID is only known once the operation is queued. Hold off the
        // completion interrupt until it is recorded.
        CRITICAL_REGION_ENTER();
        if (exists) {
            Begin(FDS_EVT_UPDATE);
            e = fds_record_update(&desc, &record);
        } else {
            Begin(FDS_EVT_WRITE);
            e = fds_record_write(&desc, &record);
        }
        pending.recordID = desc.record_id;
        CRITICAL_REGION_EXIT();
        if (e == FDS_SUCCESS) {
            e = Wait();
            if (e == FDS_SUCCESS) {
                return kHAPError_None;
            }
        } else {
            Cancel();
        }

        if (e != FDS_ERR_NO_SPACE_IN_FLASH || attempt > 0) {
            HAPLogError(&logObject, "Writing %02X.%02X failed: 0x%04lX.", domain, key, (unsigned long) e);
            return kHAPError_Unknown;
        }
        HAPLogInfo(&logObject, "Flash full, collecting garbage.");
        e = RunGarbageCollection();
        if (e != FDS_SUCCESS) {
            HAPLogError(&logObject, "fds_gc failed: 0x%04lX.", (unsigned long) e);
            return kHAPError_Unknown;
        }
    }
    return kHAPError_Unknown;
}

HAPError HAPPlatformKeyValueStoreRemove(
        HAPPlatformKeyValueStoreRef keyValueStore,
        HAPPlatformKeyValueStoreDomain domain,
        HAPPlatformKeyValueStoreKey key) {
    HAPPrecondition(keyValueStore);

    fds_record_desc_t desc;
    while (FindRecord(keyValueStore, domain, key, &desc)) {
        Begin(FDS_EVT_DEL_RECORD);
        pending.recordID = desc.record_id;
        ret_code_t e = fds_record_delete(&desc);
        if (e == FDS_SUCCESS) {
            e = Wait();
        } else {
            Cancel();
        }
        if (e != FDS_SUCCESS) {
            HAPLogError(&logObject, "Deleting %02X.%02X failed: 0x%04lX.", domain, key, (unsigned long) e);
            return kHAPError_Unknown;
        }
    }
    return kHAPError_None;
}

HAPError HAPPlatformKeyValueStoreEnumerate(
        HAPPlatformKeyValueStoreRef keyValueStore,
        HAPPlatformKeyValueStoreDomain domain,
        HAPPlatformKeyValueStoreEnumerateCallback callback,
        void* _Nullable context) {
    HAPPrecondition(keyValueStore);
    HAPPrecondition(callback);

    // Collect keys first: the callback may modify the store.
    uint32_t keys[256 / 32];
    HAPRawBufferZero(keys, sizeof keys);

    fds_find_token_t token;
    HAPRawBufferZero(&token, sizeof token);
    fds_record_desc_t desc;
    HAPRawBufferZero(&desc, sizeof desc);
    while (fds_record_find_in_file((uint16_t)(keyValueStore->baseFileID + domain), &desc, &token) == FDS_SUCCESS) {
        fds_flash_record_t record;
        if (fds_record_open(&desc, &record) != FDS_SUCCESS) {
            continue;
        }
        uint16_t recordKey = record.p_header->record_key;
        (void) fds_record_close(&desc);
        if (recordKey >= 1 && recordKey <= 256) {
            uint8_t key = (uint8_t)(recordKey - 1U);
            keys[key / 32U] |= 1UL << (key % 32U);
        }
    }

    bool shouldContinue = true;
    for (size_t key = 0; key < 256 && shouldContinue; key++) {
        if (!(keys[key / 32U] & (1UL << (key % 32U)))) {
            continue;
        }
        HAPError err = callback(context, keyValueStore, domain, (HAPPlatformKeyValueStoreKey) key, &shouldContinue);
        if (err) {
            HAPAssert(err == kHAPError_Unknown);
            return err;
        }
    }
    return kHAPError_None;
}

HAPError HAPPlatformKeyValueStoreDeleteFile(HAPPlatformKeyValueStoreRef keyValueStore, uint16_t fileID) {
    HAPPrecondition(keyValueStore);

    Begin(FDS_EVT_DEL_FILE);
    pending.fileID = fileID;
    ret_code_t e = fds_file_delete(fileID);
    if (e == FDS_SUCCESS) {
        e = Wait();
    } else {
        Cancel();
    }
    if (e != FDS_SUCCESS) {
        HAPLogError(&logObject, "Deleting file %04X failed: 0x%04lX.", fileID, (unsigned long) e);
        return kHAPError_Unknown;
    }
    return kHAPError_None;
}

HAPError HAPPlatformKeyValueStorePurgeDomain(
        HAPPlatformKeyValueStoreRef keyValueStore,
        HAPPlatformKeyValueStoreDomain domain) {
    HAPPrecondition(keyValueStore);
    return HAPPlatformKeyValueStoreDeleteFile(keyValueStore, (uint16_t)(keyValueStore->baseFileID + domain));
}

HAPError HAPPlatformKeyValueStoreCompact(HAPPlatformKeyValueStoreRef keyValueStore, size_t minDirtyRecords) {
    HAPPrecondition(keyValueStore);

    fds_stat_t stat;
    if (fds_stat(&stat) != FDS_SUCCESS) {
        return kHAPError_Unknown;
    }
    if (!stat.dirty_records || stat.dirty_records < minDirtyRecords) {
        return kHAPError_None;
    }
    HAPLogInfo(&logObject, "Collecting garbage (%u dirty records).", stat.dirty_records);
    return RunGarbageCollection() == FDS_SUCCESS ? kHAPError_None : kHAPError_Unknown;
}
