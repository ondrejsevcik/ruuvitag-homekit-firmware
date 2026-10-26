/**
 * @file HAPPlatformBLEPeripheralManager.c
 * @brief HomeKit ADK BLE peripheral manager on top of the nRF52 S132 SoftDevice.
 *
 * Threading model:
 * - SoftDevice events arrive in interrupt context (NRF_SDH_DISPATCH_MODEL_INTERRUPT).
 *   Events that only need a stack-level answer (MTU, PHY, data length, security,
 *   queued write memory) are answered right there.
 * - Everything that involves the ADK (connections, authorized reads / writes,
 *   indication confirmations) is copied into a small queue and handled in main
 *   context through the Ruuvi scheduler. The ADK, and with it the flash backed
 *   key-value store, therefore never runs in interrupt context.
 *
 * GATT model:
 * - HAP characteristics use read and write authorization, so every access is
 *   routed through the ADK delegate. All of them share one value buffer
 *   (BLE_GATTS_VLOC_USER) since only one central is ever connected.
 * - Constant attributes (instance IDs) are served by the SoftDevice directly.
 * - The SoftDevice cannot remove services. The ADK database is static, so when
 *   the ADK registers it again after an accessory server restart the original
 *   registration is replayed and the same handles are returned.
 */

#include <string.h>

#include "HAPPlatform.h"
#include "HAPPlatformBLEPeripheralManager+Init.h"

#include "ble.h"
#include "ble_gap.h"
#include "ble_gatts.h"
#include "nrf_sdh_ble.h"
#include "ruuvi_interface_scheduler.h"

static const HAPLogObject logObject = { .subsystem = kHAPPlatform_LogSubsystem, .category = "BLEPeripheralManager" };

/** SoftDevice observer priority (0 is highest). */
#define HAP_BLE_OBSERVER_PRIO 2

enum {
    kEvent_Connected = 1,
    kEvent_Disconnected,
    kEvent_Read,
    kEvent_Write,
    kEvent_ExecuteWrite,
    kEvent_IndicationConfirmed,
    kEvent_SubscriptionChanged,
};

enum {
    kRecordKind_Service = 1,
    kRecordKind_Characteristic,
    kRecordKind_Descriptor,
};

static HAPPlatformBLEPeripheralManager* _Nullable instance;

//----------------------------------------------------------------------------------------------------------------------
// Helpers.

static uint16_t Crc16(uint16_t crc, const void* bytes_, size_t numBytes) {
    const uint8_t* bytes = bytes_;
    for (size_t i = 0; i < numBytes; i++) {
        crc = (uint16_t)((crc >> 8) | (crc << 8));
        crc ^= bytes[i];
        crc ^= (uint16_t)((crc & 0xFF) >> 4);
        crc ^= (uint16_t)(crc << 12);
        crc ^= (uint16_t)((crc & 0xFF) << 5);
    }
    return crc;
}

static uint16_t RecordSignature(
        uint8_t kind,
        const HAPPlatformBLEPeripheralManagerUUID* type,
        uint8_t properties,
        const void* _Nullable constBytes,
        size_t constNumBytes) {
    uint16_t crc = 0xFFFF;
    crc = Crc16(crc, &kind, sizeof kind);
    crc = Crc16(crc, type->bytes, sizeof type->bytes);
    crc = Crc16(crc, &properties, sizeof properties);
    if (constBytes) {
        crc = Crc16(crc, constBytes, constNumBytes);
    }
    return crc;
}

static void LogNRFError(const char* function, uint32_t e) {
    HAPLogError(&logObject, "%s failed: 0x%04lX.", function, (unsigned long) e);
}

/**
 * Converts an ADK 128-bit UUID into a SoftDevice UUID, registering its base if needed.
 */
HAP_RESULT_USE_CHECK
static HAPError GetSoftDeviceUUID(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        const HAPPlatformBLEPeripheralManagerUUID* type,
        ble_uuid_t* uuid) {
    // Bytes 12 and 13 are the 16-bit alias, the rest forms the base.
    uint8_t base[16];
    HAPRawBufferCopyBytes(base, type->bytes, sizeof base);
    base[12] = 0;
    base[13] = 0;

    size_t i;
    for (i = 0; i < blePeripheralManager->numUUIDBases; i++) {
        if (HAPRawBufferAreEqual(blePeripheralManager->uuidBases[i].bytes, base, sizeof base)) {
            break;
        }
    }
    if (i == blePeripheralManager->numUUIDBases) {
        if (i >= HAPArrayCount(blePeripheralManager->uuidBases)) {
            HAPLogError(&logObject, "Too many UUID bases. Increase HAP_PLATFORM_BLE_MAX_UUID_BASES.");
            return kHAPError_OutOfResources;
        }
        ble_uuid128_t base128;
        HAPRawBufferCopyBytes(base128.uuid128, base, sizeof base128.uuid128);
        uint8_t uuidType;
        uint32_t e = sd_ble_uuid_vs_add(&base128, &uuidType);
        if (e) {
            LogNRFError("sd_ble_uuid_vs_add", e);
            return kHAPError_OutOfResources;
        }
        HAPRawBufferCopyBytes(blePeripheralManager->uuidBases[i].bytes, base, sizeof base);
        blePeripheralManager->uuidBases[i].type = uuidType;
        blePeripheralManager->numUUIDBases++;
    }

    uuid->type = blePeripheralManager->uuidBases[i].type;
    uuid->uuid = (uint16_t)(type->bytes[12] | (type->bytes[13] << 8));
    return kHAPError_None;
}

/**
 * Returns the record for the next registration.
 *
 * - While replaying, verifies that the registration matches the original one.
 * - Otherwise, allocates a new record.
 */
HAP_RESULT_USE_CHECK
static HAPPlatformBLEPeripheralManagerAttributeRecord* _Nullable NextRecord(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        uint16_t signature,
        bool* isReplay) {
    if (blePeripheralManager->isReplaying) {
        if (blePeripheralManager->replayIndex >= blePeripheralManager->numRecords ||
            blePeripheralManager->records[blePeripheralManager->replayIndex].signature != signature) {
            // The SoftDevice cannot remove attributes. Restart to build the new database from scratch.
            HAPLogError(&logObject, "GATT database changed at runtime. Restarting.");
            HAPPlatformAbort();
        }
        *isReplay = true;
        return &blePeripheralManager->records[blePeripheralManager->replayIndex++];
    }

    if (blePeripheralManager->numRecords >= HAPArrayCount(blePeripheralManager->records)) {
        HAPLogError(&logObject, "GATT database full. Increase HAP_PLATFORM_BLE_MAX_ATTRIBUTES.");
        return NULL;
    }
    *isReplay = false;
    HAPPlatformBLEPeripheralManagerAttributeRecord* record =
            &blePeripheralManager->records[blePeripheralManager->numRecords++];
    HAPRawBufferZero(record, sizeof *record);
    record->signature = signature;
    return record;
}

static void ConfigureTxPower(HAPPlatformBLEPeripheralManagerRef blePeripheralManager, uint8_t role, uint16_t handle) {
    uint32_t e = sd_ble_gap_tx_power_set(role, handle, blePeripheralManager->txPower);
    if (e) {
        LogNRFError("sd_ble_gap_tx_power_set", e);
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Advertising.

static void StopAdvertisingInternal(HAPPlatformBLEPeripheralManagerRef blePeripheralManager) {
    if (blePeripheralManager->isAdvertising) {
        uint32_t e = sd_ble_gap_adv_stop(blePeripheralManager->advertisingHandle);
        if (e && e != NRF_ERROR_INVALID_STATE) {
            LogNRFError("sd_ble_gap_adv_stop", e);
        }
        blePeripheralManager->isAdvertising = false;
    }
}

/**
 * Applies the requested advertising state, if possible.
 *
 * - Connectable advertising is not possible while connected (single link); it is
 *   resumed once the central disconnects.
 */
static void SyncAdvertising(HAPPlatformBLEPeripheralManagerRef blePeripheralManager) {
    uint32_t e;

    if (!blePeripheralManager->wantsAdvertising || blePeripheralManager->isConnected) {
        StopAdvertisingInternal(blePeripheralManager);
        return;
    }

    // While advertising, the staged payload is never in the buffer the SoftDevice is reading.
    uint8_t i = blePeripheralManager->stagedBufferIndex;
    ble_gap_adv_data_t data = {
        .adv_data = { .p_data = blePeripheralManager->advertisingBytes[i],
                      .len = blePeripheralManager->numAdvertisingBytes },
        .scan_rsp_data = { .p_data = blePeripheralManager->numScanResponseBytes ?
                                             blePeripheralManager->scanResponseBytes[i] :
                                             NULL,
                           .len = blePeripheralManager->numScanResponseBytes },
    };

    bool intervalChanged = blePeripheralManager->advertisingInterval != blePeripheralManager->activeAdvertisingInterval;
    if (blePeripheralManager->isAdvertising && !intervalChanged) {
        // Update the payload in place.
        e = sd_ble_gap_adv_set_configure(&blePeripheralManager->advertisingHandle, &data, NULL);
        if (e) {
            LogNRFError("sd_ble_gap_adv_set_configure (update)", e);
            return;
        }
        blePeripheralManager->advertisingBufferIndex = i;
        return;
    }

    StopAdvertisingInternal(blePeripheralManager);

    ble_gap_adv_params_t params;
    HAPRawBufferZero(&params, sizeof params);
    params.properties.type = BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED;
    params.p_peer_addr = NULL;
    params.filter_policy = BLE_GAP_ADV_FP_ANY;
    params.interval = blePeripheralManager->advertisingInterval;
    params.duration = BLE_GAP_ADV_TIMEOUT_GENERAL_UNLIMITED;
    params.primary_phy = BLE_GAP_PHY_1MBPS;

    e = sd_ble_gap_adv_set_configure(&blePeripheralManager->advertisingHandle, &data, &params);
    if (e) {
        LogNRFError("sd_ble_gap_adv_set_configure", e);
        return;
    }
    blePeripheralManager->advertisingBufferIndex = i;
    blePeripheralManager->isAdvertisingConfigured = true;
    ConfigureTxPower(blePeripheralManager, BLE_GAP_TX_POWER_ROLE_ADV, blePeripheralManager->advertisingHandle);

    e = sd_ble_gap_adv_start(blePeripheralManager->advertisingHandle, blePeripheralManager->connectionConfigurationTag);
    if (e) {
        LogNRFError("sd_ble_gap_adv_start", e);
        return;
    }
    blePeripheralManager->isAdvertising = true;
    blePeripheralManager->activeAdvertisingInterval = blePeripheralManager->advertisingInterval;
}

//----------------------------------------------------------------------------------------------------------------------
// Main context event processing.

static void AuthorizeReply(uint16_t connectionHandle, const ble_gatts_rw_authorize_reply_params_t* reply) {
    uint32_t e = sd_ble_gatts_rw_authorize_reply(connectionHandle, reply);
    if (e && e != BLE_ERROR_INVALID_CONN_HANDLE) {
        LogNRFError("sd_ble_gatts_rw_authorize_reply", e);
    }
}

static uint16_t GATTStatusForError(HAPError err, bool isWrite) {
    switch (err) {
        case kHAPError_None:
            return BLE_GATT_STATUS_SUCCESS;
        case kHAPError_InvalidState:
            return isWrite ? BLE_GATT_STATUS_ATTERR_WRITE_NOT_PERMITTED : BLE_GATT_STATUS_ATTERR_READ_NOT_PERMITTED;
        case kHAPError_InvalidData:
            return BLE_GATT_STATUS_ATTERR_INVALID_ATT_VAL_LENGTH;
        case kHAPError_OutOfResources:
            return BLE_GATT_STATUS_ATTERR_INSUF_RESOURCES;
        default:
            return BLE_GATT_STATUS_ATTERR_UNLIKELY_ERROR;
    }
}

static void HandleRead(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        const HAPPlatformBLEPeripheralManagerEvent* event) {
    ble_gatts_rw_authorize_reply_params_t reply;
    HAPRawBufferZero(&reply, sizeof reply);
    reply.type = BLE_GATTS_AUTHORIZE_TYPE_READ;

    // A long read starts at offset 0 and continues with Read Blob requests on the same handle.
    // The ADK is only asked again once the previous value has been fully transferred.
    bool needsValue = event->offset == 0 || !blePeripheralManager->isReadValid ||
                      blePeripheralManager->readHandle != event->attributeHandle;
    if (needsValue) {
        blePeripheralManager->isReadValid = false;
        HAPError err = kHAPError_InvalidState;
        size_t numBytes = 0;
        if (blePeripheralManager->delegate.handleReadRequest) {
            err = blePeripheralManager->delegate.handleReadRequest(
                    blePeripheralManager,
                    event->connectionHandle,
                    event->attributeHandle,
                    blePeripheralManager->readBytes,
                    sizeof blePeripheralManager->readBytes,
                    &numBytes,
                    blePeripheralManager->delegate.context);
        }
        if (err) {
            reply.params.read.gatt_status = GATTStatusForError(err, /* isWrite: */ false);
            AuthorizeReply(event->connectionHandle, &reply);
            return;
        }
        HAPAssert(numBytes <= sizeof blePeripheralManager->readBytes);
        blePeripheralManager->numReadBytes = numBytes;
        blePeripheralManager->readHandle = event->attributeHandle;
        blePeripheralManager->isReadValid = true;
    }

    // Replace the whole attribute value. The SoftDevice responds starting at the requested offset.
    reply.params.read.gatt_status = BLE_GATT_STATUS_SUCCESS;
    reply.params.read.update = 1;
    reply.params.read.offset = 0;
    reply.params.read.len = (uint16_t) blePeripheralManager->numReadBytes;
    reply.params.read.p_data = blePeripheralManager->readBytes;
    AuthorizeReply(event->connectionHandle, &reply);

    if (event->offset + (size_t) 1 >= blePeripheralManager->numReadBytes) {
        // Nothing left to read beyond this request.
        blePeripheralManager->isReadValid = false;
    }
}

static HAPError DeliverWrite(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        uint16_t connectionHandle,
        uint16_t attributeHandle,
        void* bytes,
        size_t numBytes) {
    // Any write ends a pending long read.
    blePeripheralManager->isReadValid = false;
    if (!blePeripheralManager->delegate.handleWriteRequest) {
        return kHAPError_InvalidState;
    }
    return blePeripheralManager->delegate.handleWriteRequest(
            blePeripheralManager,
            connectionHandle,
            attributeHandle,
            bytes,
            numBytes,
            blePeripheralManager->delegate.context);
}

static void HandleWrite(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        HAPPlatformBLEPeripheralManagerEvent* event) {
    HAPError err = kHAPError_InvalidData;
    if (event->offset == 0) {
        HAPRawBufferCopyBytes(blePeripheralManager->writeBytes, event->bytes, event->numBytes);
        err = DeliverWrite(
                blePeripheralManager,
                event->connectionHandle,
                event->attributeHandle,
                blePeripheralManager->writeBytes,
                event->numBytes);
    }

    ble_gatts_rw_authorize_reply_params_t reply;
    HAPRawBufferZero(&reply, sizeof reply);
    reply.type = BLE_GATTS_AUTHORIZE_TYPE_WRITE;
    reply.params.write.gatt_status = GATTStatusForError(err, /* isWrite: */ true);
    if (!err) {
        reply.params.write.update = 1;
        reply.params.write.offset = 0;
        reply.params.write.len = event->numBytes;
        reply.params.write.p_data = event->bytes;
    }
    AuthorizeReply(event->connectionHandle, &reply);
}

/**
 * Reassembles a long write from the queued write buffer.
 *
 * Buffer layout (SoftDevice): repeated { handle (2), offset (2), length (2), data (length) },
 * terminated by BLE_GATT_HANDLE_INVALID.
 */
static void HandleExecuteWrite(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        const HAPPlatformBLEPeripheralManagerEvent* event) {
    const uint8_t* q = blePeripheralManager->queuedWriteBytes;
    size_t i = 0;
    uint16_t attributeHandle = BLE_GATT_HANDLE_INVALID;
    size_t numBytes = 0;
    HAPError err = kHAPError_None;

    while (i + 6 <= sizeof blePeripheralManager->queuedWriteBytes) {
        uint16_t handle = (uint16_t)(q[i] | (q[i + 1] << 8));
        if (handle == BLE_GATT_HANDLE_INVALID) {
            break;
        }
        uint16_t offset = (uint16_t)(q[i + 2] | (q[i + 3] << 8));
        uint16_t length = (uint16_t)(q[i + 4] | (q[i + 5] << 8));
        i += 6;
        if (i + length > sizeof blePeripheralManager->queuedWriteBytes) {
            err = kHAPError_InvalidData;
            break;
        }
        if (attributeHandle != BLE_GATT_HANDLE_INVALID && handle != attributeHandle) {
            // HAP never writes more than one characteristic in a single queued write.
            err = kHAPError_InvalidData;
            break;
        }
        attributeHandle = handle;
        if ((size_t) offset + length > sizeof blePeripheralManager->writeBytes) {
            err = kHAPError_OutOfResources;
            break;
        }
        HAPRawBufferCopyBytes(&blePeripheralManager->writeBytes[offset], &q[i], length);
        if ((size_t) offset + length > numBytes) {
            numBytes = (size_t) offset + length;
        }
        i += length;
    }
    if (!err && attributeHandle == BLE_GATT_HANDLE_INVALID) {
        err = kHAPError_InvalidData;
    }
    if (!err) {
        err = DeliverWrite(
                blePeripheralManager,
                event->connectionHandle,
                attributeHandle,
                blePeripheralManager->writeBytes,
                numBytes);
    }

    ble_gatts_rw_authorize_reply_params_t reply;
    HAPRawBufferZero(&reply, sizeof reply);
    reply.type = BLE_GATTS_AUTHORIZE_TYPE_WRITE;
    reply.params.write.gatt_status = GATTStatusForError(err, /* isWrite: */ true);
    AuthorizeReply(event->connectionHandle, &reply);
}

static void HandleEvent(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        HAPPlatformBLEPeripheralManagerEvent* event) {
    switch (event->kind) {
        case kEvent_Connected: {
            HAPLogInfo(&logObject, "Connected (handle 0x%04X).", event->connectionHandle);
            if (blePeripheralManager->delegate.handleConnectedCentral) {
                blePeripheralManager->delegate.handleConnectedCentral(
                        blePeripheralManager, event->connectionHandle, blePeripheralManager->delegate.context);
            }
        } break;
        case kEvent_Disconnected: {
            HAPLogInfo(&logObject, "Disconnected (handle 0x%04X).", event->connectionHandle);
            blePeripheralManager->isReadValid = false;
            if (blePeripheralManager->delegate.handleDisconnectedCentral) {
                blePeripheralManager->delegate.handleDisconnectedCentral(
                        blePeripheralManager, event->connectionHandle, blePeripheralManager->delegate.context);
            }
            SyncAdvertising(blePeripheralManager);
        } break;
        case kEvent_Read: {
            HandleRead(blePeripheralManager, event);
        } break;
        case kEvent_Write: {
            HandleWrite(blePeripheralManager, event);
        } break;
        case kEvent_ExecuteWrite: {
            HandleExecuteWrite(blePeripheralManager, event);
        } break;
        case kEvent_SubscriptionChanged: {
            // The SoftDevice has already stored the CCC descriptor value. The ADK tracks
            // subscriptions per HAP session, so report the write to it as well.
            HAPRawBufferCopyBytes(blePeripheralManager->writeBytes, event->bytes, event->numBytes);
            HAPError err = DeliverWrite(
                    blePeripheralManager,
                    event->connectionHandle,
                    event->attributeHandle,
                    blePeripheralManager->writeBytes,
                    event->numBytes);
            if (err) {
                HAPLogError(&logObject, "CCC descriptor write 0x%04X rejected: %u.", event->attributeHandle, err);
            }
        } break;
        case kEvent_IndicationConfirmed: {
            if (blePeripheralManager->delegate.handleReadyToUpdateSubscribers) {
                blePeripheralManager->delegate.handleReadyToUpdateSubscribers(
                        blePeripheralManager, event->connectionHandle, blePeripheralManager->delegate.context);
            }
        } break;
        default:
            HAPFatalError();
    }
}

static void ProcessEvents(void* _Nullable data HAP_UNUSED, uint16_t size HAP_UNUSED) {
    HAPPlatformBLEPeripheralManagerRef blePeripheralManager = instance;
    if (!blePeripheralManager) {
        return;
    }
    blePeripheralManager->isProcessingScheduled = false;
    while (blePeripheralManager->eventsTail != blePeripheralManager->eventsHead) {
        uint8_t tail = blePeripheralManager->eventsTail;
        HandleEvent(blePeripheralManager, &blePeripheralManager->events[tail]);
        blePeripheralManager->eventsTail = (uint8_t)((tail + 1U) % HAP_PLATFORM_BLE_EVENT_QUEUE_LENGTH);
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Interrupt context.

/**
 * Reserves a slot in the event queue. Called in SoftDevice interrupt context only.
 */
static HAPPlatformBLEPeripheralManagerEvent* _Nullable EnqueueBegin(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        uint8_t kind,
        uint16_t connectionHandle) {
    uint8_t head = blePeripheralManager->eventsHead;
    uint8_t next = (uint8_t)((head + 1U) % HAP_PLATFORM_BLE_EVENT_QUEUE_LENGTH);
    if (next == blePeripheralManager->eventsTail) {
        // Cannot happen with a single connection (ATT is request / response), but be safe.
        (void) sd_ble_gap_disconnect(connectionHandle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
        return NULL;
    }
    HAPPlatformBLEPeripheralManagerEvent* event = &blePeripheralManager->events[head];
    event->kind = kind;
    event->connectionHandle = connectionHandle;
    event->attributeHandle = BLE_GATT_HANDLE_INVALID;
    event->op = 0;
    event->offset = 0;
    event->numBytes = 0;
    return event;
}

static void EnqueueCommit(HAPPlatformBLEPeripheralManagerRef blePeripheralManager) {
    blePeripheralManager->eventsHead =
            (uint8_t)((blePeripheralManager->eventsHead + 1U) % HAP_PLATFORM_BLE_EVENT_QUEUE_LENGTH);
    if (!blePeripheralManager->isProcessingScheduled) {
        blePeripheralManager->isProcessingScheduled = true;
        if (ri_scheduler_event_put(NULL, 0, ProcessEvents) != RD_SUCCESS) {
            blePeripheralManager->isProcessingScheduled = false;
        }
    }
}

static void Enqueue(HAPPlatformBLEPeripheralManagerRef blePeripheralManager, uint8_t kind, uint16_t connectionHandle) {
    if (EnqueueBegin(blePeripheralManager, kind, connectionHandle)) {
        EnqueueCommit(blePeripheralManager);
    }
}

static bool IsCCCDescriptor(HAPPlatformBLEPeripheralManagerRef blePeripheralManager, uint16_t handle) {
    for (size_t i = 0; i < blePeripheralManager->numRecords; i++) {
        if (blePeripheralManager->records[i].cccHandle == handle) {
            return true;
        }
    }
    return false;
}

static void RejectWrite(uint16_t connectionHandle, uint16_t status) {
    ble_gatts_rw_authorize_reply_params_t reply;
    HAPRawBufferZero(&reply, sizeof reply);
    reply.type = BLE_GATTS_AUTHORIZE_TYPE_WRITE;
    reply.params.write.gatt_status = status;
    (void) sd_ble_gatts_rw_authorize_reply(connectionHandle, &reply);
}

static void OnAuthorizeRequest(HAPPlatformBLEPeripheralManagerRef blePeripheralManager, const ble_gatts_evt_t* gatts) {
    const ble_gatts_evt_rw_authorize_request_t* request = &gatts->params.authorize_request;
    uint16_t connectionHandle = gatts->conn_handle;

    if (request->type == BLE_GATTS_AUTHORIZE_TYPE_READ) {
        HAPPlatformBLEPeripheralManagerEvent* event = EnqueueBegin(blePeripheralManager, kEvent_Read, connectionHandle);
        if (event) {
            event->attributeHandle = request->request.read.handle;
            event->offset = request->request.read.offset;
            EnqueueCommit(blePeripheralManager);
        }
        return;
    }
    if (request->type != BLE_GATTS_AUTHORIZE_TYPE_WRITE) {
        return;
    }

    const ble_gatts_evt_write_t* write = &request->request.write;
    switch (write->op) {
        case BLE_GATTS_OP_WRITE_REQ: {
            if (write->len > HAP_PLATFORM_BLE_MAX_WRITE_BYTES) {
                RejectWrite(connectionHandle, BLE_GATT_STATUS_ATTERR_INVALID_ATT_VAL_LENGTH);
                return;
            }
            HAPPlatformBLEPeripheralManagerEvent* event =
                    EnqueueBegin(blePeripheralManager, kEvent_Write, connectionHandle);
            if (event) {
                event->attributeHandle = write->handle;
                event->op = write->op;
                event->offset = write->offset;
                event->numBytes = write->len;
                memcpy(event->bytes, write->data, write->len);
                EnqueueCommit(blePeripheralManager);
            }
        } break;
        case BLE_GATTS_OP_PREP_WRITE_REQ: {
            // Data is collected by the SoftDevice in the queued write buffer.
            ble_gatts_rw_authorize_reply_params_t reply;
            HAPRawBufferZero(&reply, sizeof reply);
            reply.type = BLE_GATTS_AUTHORIZE_TYPE_WRITE;
            reply.params.write.gatt_status = BLE_GATT_STATUS_SUCCESS;
            (void) sd_ble_gatts_rw_authorize_reply(connectionHandle, &reply);
        } break;
        case BLE_GATTS_OP_EXEC_WRITE_REQ_NOW: {
            Enqueue(blePeripheralManager, kEvent_ExecuteWrite, connectionHandle);
        } break;
        case BLE_GATTS_OP_EXEC_WRITE_REQ_CANCEL: {
            RejectWrite(connectionHandle, BLE_GATT_STATUS_SUCCESS);
        } break;
        default: {
            RejectWrite(connectionHandle, BLE_GATT_STATUS_ATTERR_REQUEST_NOT_SUPPORTED);
        } break;
    }
}

static void OnBLEEvent(ble_evt_t const* evt, void* _Nullable context HAP_UNUSED) {
    HAPPlatformBLEPeripheralManagerRef blePeripheralManager = instance;
    if (!blePeripheralManager) {
        return;
    }

    uint32_t e;
    switch (evt->header.evt_id) {
        case BLE_GAP_EVT_CONNECTED: {
            if (evt->evt.gap_evt.params.connected.role != BLE_GAP_ROLE_PERIPH) {
                break;
            }
            blePeripheralManager->connectionHandle = evt->evt.gap_evt.conn_handle;
            blePeripheralManager->isConnected = true;
            blePeripheralManager->isAdvertising = false;
            blePeripheralManager->isIndicationPending = false;
            ConfigureTxPower(blePeripheralManager, BLE_GAP_TX_POWER_ROLE_CONN, evt->evt.gap_evt.conn_handle);
            Enqueue(blePeripheralManager, kEvent_Connected, evt->evt.gap_evt.conn_handle);
        } break;
        case BLE_GAP_EVT_DISCONNECTED: {
            if (!blePeripheralManager->isConnected ||
                evt->evt.gap_evt.conn_handle != blePeripheralManager->connectionHandle) {
                break;
            }
            blePeripheralManager->isConnected = false;
            blePeripheralManager->isIndicationPending = false;
            blePeripheralManager->connectionHandle = BLE_CONN_HANDLE_INVALID;
            Enqueue(blePeripheralManager, kEvent_Disconnected, evt->evt.gap_evt.conn_handle);
        } break;
        case BLE_GAP_EVT_ADV_SET_TERMINATED: {
            blePeripheralManager->isAdvertising = false;
        } break;
        case BLE_GAP_EVT_SEC_PARAMS_REQUEST: {
            // HAP has its own security layer. BLE pairing is not supported.
            e = sd_ble_gap_sec_params_reply(
                    evt->evt.gap_evt.conn_handle, BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP, NULL, NULL);
            (void) e;
        } break;
        case BLE_GAP_EVT_PHY_UPDATE_REQUEST: {
            ble_gap_phys_t const phys = { .rx_phys = BLE_GAP_PHY_AUTO, .tx_phys = BLE_GAP_PHY_AUTO };
            e = sd_ble_gap_phy_update(evt->evt.gap_evt.conn_handle, &phys);
            (void) e;
        } break;
        case BLE_GAP_EVT_DATA_LENGTH_UPDATE_REQUEST: {
            e = sd_ble_gap_data_length_update(evt->evt.gap_evt.conn_handle, NULL, NULL);
            (void) e;
        } break;
        case BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST: {
            e = sd_ble_gatts_exchange_mtu_reply(evt->evt.gatts_evt.conn_handle, NRF_SDH_BLE_GATT_MAX_MTU_SIZE);
            (void) e;
        } break;
        case BLE_GATTS_EVT_SYS_ATTR_MISSING: {
            e = sd_ble_gatts_sys_attr_set(evt->evt.gatts_evt.conn_handle, NULL, 0, 0);
            (void) e;
        } break;
        case BLE_EVT_USER_MEM_REQUEST: {
            HAPRawBufferZero(blePeripheralManager->queuedWriteBytes, sizeof blePeripheralManager->queuedWriteBytes);
            ble_user_mem_block_t const block = { .p_mem = blePeripheralManager->queuedWriteBytes,
                                                 .len = sizeof blePeripheralManager->queuedWriteBytes };
            e = sd_ble_user_mem_reply(evt->evt.common_evt.conn_handle, &block);
            (void) e;
        } break;
        case BLE_GATTS_EVT_RW_AUTHORIZE_REQUEST: {
            if (blePeripheralManager->isConnected &&
                evt->evt.gatts_evt.conn_handle == blePeripheralManager->connectionHandle) {
                OnAuthorizeRequest(blePeripheralManager, &evt->evt.gatts_evt);
            }
        } break;
        case BLE_GATTS_EVT_WRITE: {
            // Writes without authorization: only CCC descriptors, handled by the SoftDevice.
            const ble_gatts_evt_write_t* write = &evt->evt.gatts_evt.params.write;
            if (blePeripheralManager->isConnected &&
                evt->evt.gatts_evt.conn_handle == blePeripheralManager->connectionHandle &&
                write->op == BLE_GATTS_OP_WRITE_REQ && write->len == 2 && IsCCCDescriptor(blePeripheralManager, write->handle)) {
                HAPPlatformBLEPeripheralManagerEvent* event =
                        EnqueueBegin(blePeripheralManager, kEvent_SubscriptionChanged, evt->evt.gatts_evt.conn_handle);
                if (event) {
                    event->attributeHandle = write->handle;
                    event->numBytes = write->len;
                    memcpy(event->bytes, write->data, write->len);
                    EnqueueCommit(blePeripheralManager);
                }
            }
        } break;
        case BLE_GATTS_EVT_HVC: {
            if (blePeripheralManager->isIndicationPending) {
                blePeripheralManager->isIndicationPending = false;
                Enqueue(blePeripheralManager, kEvent_IndicationConfirmed, evt->evt.gatts_evt.conn_handle);
            }
        } break;
        case BLE_GATTS_EVT_TIMEOUT: {
            e = sd_ble_gap_disconnect(evt->evt.gatts_evt.conn_handle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            (void) e;
        } break;
        default:
            break;
    }
}

NRF_SDH_BLE_OBSERVER(hapBLEObserver, HAP_BLE_OBSERVER_PRIO, OnBLEEvent, NULL);

//----------------------------------------------------------------------------------------------------------------------
// Public API.

void HAPPlatformBLEPeripheralManagerCreate(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        const HAPPlatformBLEPeripheralManagerOptions* options) {
    HAPPrecondition(blePeripheralManager);
    HAPPrecondition(options);
    HAPPrecondition(!instance);

    HAPRawBufferZero(blePeripheralManager, sizeof *blePeripheralManager);
    blePeripheralManager->connectionHandle = BLE_CONN_HANDLE_INVALID;
    blePeripheralManager->advertisingHandle = BLE_GAP_ADV_SET_HANDLE_NOT_SET;
    blePeripheralManager->txPower = options->txPower;
    blePeripheralManager->connectionConfigurationTag = options->connectionConfigurationTag;
    instance = blePeripheralManager;
}

bool HAPPlatformBLEPeripheralManagerIsActive(HAPPlatformBLEPeripheralManagerRef blePeripheralManager) {
    HAPPrecondition(blePeripheralManager);
    return blePeripheralManager->isConnected || blePeripheralManager->isAdvertising;
}

bool HAPPlatformBLEPeripheralManagerIsConnected(HAPPlatformBLEPeripheralManagerRef blePeripheralManager) {
    HAPPrecondition(blePeripheralManager);
    return blePeripheralManager->isConnected;
}

void HAPPlatformBLEPeripheralManagerSetDelegate(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        const HAPPlatformBLEPeripheralManagerDelegate* _Nullable delegate) {
    HAPPrecondition(blePeripheralManager);

    if (delegate) {
        blePeripheralManager->delegate = *delegate;
    } else {
        HAPRawBufferZero(&blePeripheralManager->delegate, sizeof blePeripheralManager->delegate);
    }
}

void HAPPlatformBLEPeripheralManagerSetDeviceAddress(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        const HAPPlatformBLEPeripheralManagerDeviceAddress* deviceAddress) {
    HAPPrecondition(blePeripheralManager);
    HAPPrecondition(deviceAddress);
    HAPPrecondition(!blePeripheralManager->isConnected);

    // The address cannot change while advertising.
    bool wasAdvertising = blePeripheralManager->isAdvertising;
    StopAdvertisingInternal(blePeripheralManager);

    ble_gap_addr_t address;
    HAPRawBufferZero(&address, sizeof address);
    address.addr_type = BLE_GAP_ADDR_TYPE_RANDOM_STATIC;
    HAPAssert(sizeof address.addr == sizeof deviceAddress->bytes);
    HAPRawBufferCopyBytes(address.addr, deviceAddress->bytes, sizeof address.addr);
    uint32_t e = sd_ble_gap_addr_set(&address);
    if (e) {
        LogNRFError("sd_ble_gap_addr_set", e);
    }

    if (wasAdvertising) {
        blePeripheralManager->activeAdvertisingInterval = 0;
        SyncAdvertising(blePeripheralManager);
    }
}

void HAPPlatformBLEPeripheralManagerSetDeviceName(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        const char* deviceName) {
    HAPPrecondition(blePeripheralManager);
    HAPPrecondition(deviceName);

    ble_gap_conn_sec_mode_t mode;
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&mode);
    uint32_t e = sd_ble_gap_device_name_set(
            &mode, (const uint8_t*) deviceName, (uint16_t) HAPStringGetNumBytes(deviceName));
    if (e) {
        LogNRFError("sd_ble_gap_device_name_set", e);
    }
}

void HAPPlatformBLEPeripheralManagerRemoveAllServices(HAPPlatformBLEPeripheralManagerRef blePeripheralManager) {
    HAPPrecondition(blePeripheralManager);

    if (blePeripheralManager->numRecords) {
        // Attributes stay in the SoftDevice. Replay the next registration against them.
        blePeripheralManager->isReplaying = true;
        blePeripheralManager->replayIndex = 0;
    }
    blePeripheralManager->isPublished = false;
}

HAPError HAPPlatformBLEPeripheralManagerAddService(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        const HAPPlatformBLEPeripheralManagerUUID* type,
        bool isPrimary) {
    HAPPrecondition(blePeripheralManager);
    HAPPrecondition(type);
    HAPPrecondition(!blePeripheralManager->isPublished);

    uint16_t signature = RecordSignature(kRecordKind_Service, type, isPrimary, NULL, 0);
    bool isReplay;
    HAPPlatformBLEPeripheralManagerAttributeRecord* record = NextRecord(blePeripheralManager, signature, &isReplay);
    if (!record) {
        return kHAPError_OutOfResources;
    }
    if (isReplay) {
        blePeripheralManager->lastServiceHandle = record->valueHandle;
        return kHAPError_None;
    }

    ble_uuid_t uuid;
    HAPError err = GetSoftDeviceUUID(blePeripheralManager, type, &uuid);
    if (err) {
        return err;
    }
    uint16_t handle;
    uint32_t e = sd_ble_gatts_service_add(
            isPrimary ? BLE_GATTS_SRVC_TYPE_PRIMARY : BLE_GATTS_SRVC_TYPE_SECONDARY, &uuid, &handle);
    if (e) {
        LogNRFError("sd_ble_gatts_service_add", e);
        return kHAPError_OutOfResources;
    }
    record->valueHandle = handle;
    blePeripheralManager->lastServiceHandle = handle;
    return kHAPError_None;
}

HAPError HAPPlatformBLEPeripheralManagerAddCharacteristic(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        const HAPPlatformBLEPeripheralManagerUUID* type,
        HAPPlatformBLEPeripheralManagerCharacteristicProperties properties,
        const void* _Nullable constBytes,
        size_t constNumBytes,
        HAPPlatformBLEPeripheralManagerAttributeHandle* valueHandle,
        HAPPlatformBLEPeripheralManagerAttributeHandle* _Nullable cccDescriptorHandle) {
    HAPPrecondition(blePeripheralManager);
    HAPPrecondition(type);
    HAPPrecondition(!constBytes || constNumBytes <= BLE_GATTS_FIX_ATTR_LEN_MAX);
    HAPPrecondition(valueHandle);
    HAPPrecondition((properties.notify || properties.indicate) == (cccDescriptorHandle != NULL));
    HAPPrecondition(!blePeripheralManager->isPublished);
    HAPPrecondition(blePeripheralManager->lastServiceHandle);

    uint8_t propertyBits = (uint8_t)(
            (properties.read ? 1U << 0 : 0U) | (properties.writeWithoutResponse ? 1U << 1 : 0U) |
            (properties.write ? 1U << 2 : 0U) | (properties.notify ? 1U << 3 : 0U) |
            (properties.indicate ? 1U << 4 : 0U));
    uint16_t signature =
            RecordSignature(kRecordKind_Characteristic, type, propertyBits, constBytes, constBytes ? constNumBytes : 0);
    bool isReplay;
    HAPPlatformBLEPeripheralManagerAttributeRecord* record = NextRecord(blePeripheralManager, signature, &isReplay);
    if (!record) {
        return kHAPError_OutOfResources;
    }

    if (!isReplay) {
        ble_uuid_t uuid;
        HAPError err = GetSoftDeviceUUID(blePeripheralManager, type, &uuid);
        if (err) {
            return err;
        }

        ble_gatts_attr_md_t cccMetadata;
        HAPRawBufferZero(&cccMetadata, sizeof cccMetadata);
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccMetadata.read_perm);
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccMetadata.write_perm);
        cccMetadata.vloc = BLE_GATTS_VLOC_STACK;

        ble_gatts_char_md_t characteristicMetadata;
        HAPRawBufferZero(&characteristicMetadata, sizeof characteristicMetadata);
        characteristicMetadata.char_props.read = properties.read;
        characteristicMetadata.char_props.write_wo_resp = properties.writeWithoutResponse;
        characteristicMetadata.char_props.write = properties.write;
        characteristicMetadata.char_props.notify = properties.notify;
        characteristicMetadata.char_props.indicate = properties.indicate;
        characteristicMetadata.p_cccd_md = (properties.notify || properties.indicate) ? &cccMetadata : NULL;

        ble_gatts_attr_md_t valueMetadata;
        HAPRawBufferZero(&valueMetadata, sizeof valueMetadata);
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&valueMetadata.read_perm);
        if (properties.write || properties.writeWithoutResponse) {
            BLE_GAP_CONN_SEC_MODE_SET_OPEN(&valueMetadata.write_perm);
        } else {
            BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&valueMetadata.write_perm);
        }

        ble_gatts_attr_t value;
        HAPRawBufferZero(&value, sizeof value);
        value.p_uuid = &uuid;
        value.p_attr_md = &valueMetadata;
        if (constBytes) {
            // Constant value served by the SoftDevice.
            valueMetadata.vloc = BLE_GATTS_VLOC_STACK;
            value.init_len = (uint16_t) constNumBytes;
            value.max_len = (uint16_t) constNumBytes;
            value.p_value = (uint8_t*) (uintptr_t) constBytes;
        } else {
            // Dynamic value served through the ADK delegate.
            valueMetadata.vloc = BLE_GATTS_VLOC_USER;
            valueMetadata.vlen = 1;
            valueMetadata.rd_auth = 1;
            valueMetadata.wr_auth = 1;
            value.init_len = 0;
            value.max_len = (uint16_t) sizeof blePeripheralManager->valueBytes;
            value.p_value = blePeripheralManager->valueBytes;
        }

        ble_gatts_char_handles_t handles;
        uint32_t e = sd_ble_gatts_characteristic_add(
                blePeripheralManager->lastServiceHandle, &characteristicMetadata, &value, &handles);
        if (e) {
            LogNRFError("sd_ble_gatts_characteristic_add", e);
            return kHAPError_OutOfResources;
        }
        record->valueHandle = handles.value_handle;
        record->cccHandle = handles.cccd_handle;
    }

    *valueHandle = record->valueHandle;
    if (cccDescriptorHandle) {
        *cccDescriptorHandle = record->cccHandle;
    }
    return kHAPError_None;
}

HAPError HAPPlatformBLEPeripheralManagerAddDescriptor(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        const HAPPlatformBLEPeripheralManagerUUID* type,
        HAPPlatformBLEPeripheralManagerDescriptorProperties properties,
        const void* _Nullable constBytes,
        size_t constNumBytes,
        HAPPlatformBLEPeripheralManagerAttributeHandle* descriptorHandle) {
    HAPPrecondition(blePeripheralManager);
    HAPPrecondition(type);
    HAPPrecondition(!constBytes || constNumBytes <= BLE_GATTS_FIX_ATTR_LEN_MAX);
    HAPPrecondition(descriptorHandle);
    HAPPrecondition(!blePeripheralManager->isPublished);

    uint8_t propertyBits = (uint8_t)((properties.read ? 1U << 0 : 0U) | (properties.write ? 1U << 1 : 0U));
    uint16_t signature =
            RecordSignature(kRecordKind_Descriptor, type, propertyBits, constBytes, constBytes ? constNumBytes : 0);
    bool isReplay;
    HAPPlatformBLEPeripheralManagerAttributeRecord* record = NextRecord(blePeripheralManager, signature, &isReplay);
    if (!record) {
        return kHAPError_OutOfResources;
    }

    if (!isReplay) {
        ble_uuid_t uuid;
        HAPError err = GetSoftDeviceUUID(blePeripheralManager, type, &uuid);
        if (err) {
            return err;
        }

        ble_gatts_attr_md_t metadata;
        HAPRawBufferZero(&metadata, sizeof metadata);
        if (properties.read) {
            BLE_GAP_CONN_SEC_MODE_SET_OPEN(&metadata.read_perm);
        } else {
            BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&metadata.read_perm);
        }
        if (properties.write) {
            BLE_GAP_CONN_SEC_MODE_SET_OPEN(&metadata.write_perm);
        } else {
            BLE_GAP_CONN_SEC_MODE_SET_NO_ACCESS(&metadata.write_perm);
        }

        ble_gatts_attr_t value;
        HAPRawBufferZero(&value, sizeof value);
        value.p_uuid = &uuid;
        value.p_attr_md = &metadata;
        if (constBytes) {
            metadata.vloc = BLE_GATTS_VLOC_STACK;
            value.init_len = (uint16_t) constNumBytes;
            value.max_len = (uint16_t) constNumBytes;
            value.p_value = (uint8_t*) (uintptr_t) constBytes;
        } else {
            metadata.vloc = BLE_GATTS_VLOC_USER;
            metadata.vlen = 1;
            metadata.rd_auth = properties.read;
            metadata.wr_auth = properties.write;
            value.init_len = 0;
            value.max_len = (uint16_t) sizeof blePeripheralManager->valueBytes;
            value.p_value = blePeripheralManager->valueBytes;
        }

        uint16_t handle;
        uint32_t e = sd_ble_gatts_descriptor_add(BLE_GATT_HANDLE_INVALID, &value, &handle);
        if (e) {
            LogNRFError("sd_ble_gatts_descriptor_add", e);
            return kHAPError_OutOfResources;
        }
        record->valueHandle = handle;
    }

    *descriptorHandle = record->valueHandle;
    return kHAPError_None;
}

void HAPPlatformBLEPeripheralManagerPublishServices(HAPPlatformBLEPeripheralManagerRef blePeripheralManager) {
    HAPPrecondition(blePeripheralManager);

    if (blePeripheralManager->isReplaying) {
        if (blePeripheralManager->replayIndex != blePeripheralManager->numRecords) {
            HAPLogError(&logObject, "GATT database shrank at runtime. Restarting.");
            HAPPlatformAbort();
        }
        blePeripheralManager->isReplaying = false;
    }
    blePeripheralManager->isPublished = true;
    HAPLogInfo(&logObject, "Published %u GATT attributes.", (unsigned) blePeripheralManager->numRecords);
}

void HAPPlatformBLEPeripheralManagerStartAdvertising(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        HAPBLEAdvertisingInterval advertisingInterval,
        const void* advertisingBytes,
        size_t numAdvertisingBytes,
        const void* _Nullable scanResponseBytes,
        size_t numScanResponseBytes) {
    HAPPrecondition(blePeripheralManager);
    HAPPrecondition(advertisingInterval);
    HAPPrecondition(advertisingBytes);
    HAPPrecondition(numAdvertisingBytes);
    HAPPrecondition(numAdvertisingBytes <= sizeof blePeripheralManager->advertisingBytes[0]);
    HAPPrecondition(!numScanResponseBytes || scanResponseBytes);
    HAPPrecondition(numScanResponseBytes <= sizeof blePeripheralManager->scanResponseBytes[0]);

    // Stage the payload in a buffer that is not in use by the SoftDevice.
    uint8_t i = blePeripheralManager->isAdvertising ? (uint8_t)(blePeripheralManager->advertisingBufferIndex ^ 1U) :
                                                      blePeripheralManager->advertisingBufferIndex;
    blePeripheralManager->stagedBufferIndex = i;
    HAPRawBufferCopyBytes(blePeripheralManager->advertisingBytes[i], advertisingBytes, numAdvertisingBytes);
    blePeripheralManager->numAdvertisingBytes = (uint8_t) numAdvertisingBytes;
    if (numScanResponseBytes) {
        HAPRawBufferCopyBytes(
                blePeripheralManager->scanResponseBytes[i], HAPNonnullVoid(scanResponseBytes), numScanResponseBytes);
    }
    blePeripheralManager->numScanResponseBytes = (uint8_t) numScanResponseBytes;
    blePeripheralManager->advertisingInterval = advertisingInterval;
    blePeripheralManager->wantsAdvertising = true;

    SyncAdvertising(blePeripheralManager);
}

void HAPPlatformBLEPeripheralManagerStopAdvertising(HAPPlatformBLEPeripheralManagerRef blePeripheralManager) {
    HAPPrecondition(blePeripheralManager);

    blePeripheralManager->wantsAdvertising = false;
    StopAdvertisingInternal(blePeripheralManager);
}

void HAPPlatformBLEPeripheralManagerCancelCentralConnection(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        HAPPlatformBLEPeripheralManagerConnectionHandle connectionHandle) {
    HAPPrecondition(blePeripheralManager);

    uint32_t e = sd_ble_gap_disconnect(connectionHandle, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
    if (e && e != NRF_ERROR_INVALID_STATE && e != BLE_ERROR_INVALID_CONN_HANDLE) {
        LogNRFError("sd_ble_gap_disconnect", e);
    }
}

HAPError HAPPlatformBLEPeripheralManagerSendHandleValueIndication(
        HAPPlatformBLEPeripheralManagerRef blePeripheralManager,
        HAPPlatformBLEPeripheralManagerConnectionHandle connectionHandle,
        HAPPlatformBLEPeripheralManagerAttributeHandle valueHandle,
        const void* _Nullable bytes,
        size_t numBytes) {
    HAPPrecondition(blePeripheralManager);
    HAPPrecondition(!numBytes || bytes);

    if (!blePeripheralManager->isConnected || connectionHandle != blePeripheralManager->connectionHandle ||
        blePeripheralManager->isIndicationPending) {
        return kHAPError_InvalidState;
    }
    if (numBytes > sizeof blePeripheralManager->valueBytes) {
        return kHAPError_OutOfResources;
    }

    static const uint8_t empty[1];
    uint16_t length = (uint16_t) numBytes;
    ble_gatts_hvx_params_t params;
    HAPRawBufferZero(&params, sizeof params);
    params.handle = valueHandle;
    params.type = BLE_GATT_HVX_INDICATION;
    params.offset = 0;
    params.p_len = &length;
    params.p_data = numBytes ? bytes : empty;

    blePeripheralManager->isIndicationPending = true;
    uint32_t e = sd_ble_gatts_hvx(connectionHandle, &params);
    if (e) {
        blePeripheralManager->isIndicationPending = false;
        if (e == BLE_ERROR_GATTS_SYS_ATTR_MISSING) {
            (void) sd_ble_gatts_sys_attr_set(connectionHandle, NULL, 0, 0);
        } else if (e != NRF_ERROR_INVALID_STATE && e != NRF_ERROR_BUSY) {
            LogNRFError("sd_ble_gatts_hvx", e);
        }
        return kHAPError_InvalidState;
    }
    return kHAPError_None;
}
