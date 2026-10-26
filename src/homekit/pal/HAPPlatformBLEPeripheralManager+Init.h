/**
 * @file HAPPlatformBLEPeripheralManager+Init.h
 * @brief HomeKit ADK BLE peripheral manager on top of the nRF52 S132 SoftDevice.
 */
#ifndef HAP_PLATFORM_BLE_PERIPHERAL_MANAGER_INIT_H
#define HAP_PLATFORM_BLE_PERIPHERAL_MANAGER_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "HAPPlatform.h"

/** Maximum number of GATT attributes (services, characteristics, descriptors) registered by the ADK. */
#ifndef HAP_PLATFORM_BLE_MAX_ATTRIBUTES
#define HAP_PLATFORM_BLE_MAX_ATTRIBUTES 96
#endif

/** Maximum number of distinct 128-bit UUID bases registered with the SoftDevice. */
#ifndef HAP_PLATFORM_BLE_MAX_UUID_BASES
#define HAP_PLATFORM_BLE_MAX_UUID_BASES 6
#endif

/** Size of the buffer for GATT queued (long) writes. */
#ifndef HAP_PLATFORM_BLE_QUEUED_WRITE_BYTES
#define HAP_PLATFORM_BLE_QUEUED_WRITE_BYTES 768
#endif

/** Number of SoftDevice events that can be queued for processing in main context. */
#ifndef HAP_PLATFORM_BLE_EVENT_QUEUE_LENGTH
#define HAP_PLATFORM_BLE_EVENT_QUEUE_LENGTH 4
#endif

/** Largest write payload that fits in a single ATT Write Request (ATT MTU - 3). */
#ifndef HAP_PLATFORM_BLE_MAX_WRITE_BYTES
#define HAP_PLATFORM_BLE_MAX_WRITE_BYTES 244
#endif

/**
 * Record of one GATT attribute registration.
 *
 * The SoftDevice cannot remove services once added. When the ADK re-registers
 * its (static) database after an accessory server restart, the registrations
 * are replayed against these records and the original handles are returned.
 */
typedef struct {
    uint16_t signature;   /**< CRC over the registration parameters. */
    uint16_t valueHandle; /**< Value / descriptor / service handle. */
    uint16_t cccHandle;   /**< CCC descriptor handle, if any. */
} HAPPlatformBLEPeripheralManagerAttributeRecord;

/** Event copied out of SoftDevice interrupt context for processing in main context. */
typedef struct {
    uint8_t kind;
    uint8_t op;
    uint16_t connectionHandle;
    uint16_t attributeHandle;
    uint16_t offset;
    uint16_t numBytes;
    uint8_t bytes[HAP_PLATFORM_BLE_MAX_WRITE_BYTES];
} HAPPlatformBLEPeripheralManagerEvent;

struct HAPPlatformBLEPeripheralManager {
    // Opaque type. Do not access the instance fields directly.
    HAPPlatformBLEPeripheralManagerDelegate delegate;

    // GATT database.
    HAPPlatformBLEPeripheralManagerAttributeRecord records[HAP_PLATFORM_BLE_MAX_ATTRIBUTES];
    size_t numRecords;
    size_t replayIndex;
    uint16_t lastServiceHandle;
    bool isReplaying : 1;
    bool isPublished : 1;
    struct {
        uint8_t bytes[16];
        uint8_t type;
    } uuidBases[HAP_PLATFORM_BLE_MAX_UUID_BASES];
    size_t numUUIDBases;

    // Connection.
    uint16_t connectionHandle;
    bool isConnected : 1;
    bool isIndicationPending : 1;

    // Advertising.
    uint8_t advertisingHandle;
    uint8_t advertisingBytes[2][31];
    uint8_t scanResponseBytes[2][31];
    uint8_t numAdvertisingBytes;
    uint8_t numScanResponseBytes;
    uint8_t advertisingBufferIndex; /**< Buffer in use by the SoftDevice. */
    uint8_t stagedBufferIndex;      /**< Buffer holding the latest requested payload. */
    HAPBLEAdvertisingInterval advertisingInterval;
    HAPBLEAdvertisingInterval activeAdvertisingInterval;
    bool wantsAdvertising : 1;
    bool isAdvertising : 1;
    bool isAdvertisingConfigured : 1;

    // Read response for the attribute currently being read (served via Read Blob).
    uint8_t readBytes[kHAPPlatformBLEPeripheralManager_MaxAttributeBytes];
    size_t numReadBytes;
    uint16_t readHandle;
    bool isReadValid : 1;

    // Value storage shared by all HAP characteristics (BLE_GATTS_VLOC_USER).
    uint8_t valueBytes[kHAPPlatformBLEPeripheralManager_MaxAttributeBytes];

    // Queued writes (Prepare Write / Execute Write).
    uint8_t queuedWriteBytes[HAP_PLATFORM_BLE_QUEUED_WRITE_BYTES];
    uint8_t writeBytes[kHAPPlatformBLEPeripheralManager_MaxAttributeBytes];

    // Events from interrupt context.
    HAPPlatformBLEPeripheralManagerEvent events[HAP_PLATFORM_BLE_EVENT_QUEUE_LENGTH];
    volatile uint8_t eventsHead;
    volatile uint8_t eventsTail;
    volatile bool isProcessingScheduled;

    int8_t txPower;
    uint8_t connectionConfigurationTag;
};

typedef struct {
    /** Radio TX power in dBm, used for advertising and connections. */
    int8_t txPower;

    /** SoftDevice connection configuration tag used with sd_ble_gap_adv_start. */
    uint8_t connectionConfigurationTag;
} HAPPlatformBLEPeripheralManagerOptions;

/**
 * Initializes the BLE peripheral manager. The SoftDevice must be enabled.
 *
 * Only one instance is supported.
 */
void HAPPlatformBLEPeripheralManagerCreate(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        const HAPPlatformBLEPeripheralManagerOptions* options);

/** Returns whether the radio is advertising or connected, i.e. the accessory is reachable. */
HAP_RESULT_USE_CHECK
bool HAPPlatformBLEPeripheralManagerIsActive(HAPPlatformBLEPeripheralManagerRef blePeripheralManager);

/** Returns whether a central is currently connected. */
HAP_RESULT_USE_CHECK
bool HAPPlatformBLEPeripheralManagerIsConnected(HAPPlatformBLEPeripheralManagerRef blePeripheralManager);

#ifdef __cplusplus
}
#endif

#endif
