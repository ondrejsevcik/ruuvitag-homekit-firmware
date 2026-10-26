/**
 * @file app_homekit.c
 * @brief Native HomeKit accessory: glue between the Ruuvi application and the HomeKit ADK.
 */

#include "app_config.h"
#include "app_homekit.h"

#include <math.h>
#include <stdio.h>

/* Battery mapping is plain arithmetic and is unit tested on the host. */

/** CR2477 voltage that counts as full / empty. */
#define BATTERY_FULL_V  (3.0F)
#define BATTERY_EMPTY_V (2.5F)

unsigned int app_homekit_battery_percent (const float battery_voltage)
{
    if (! (battery_voltage > BATTERY_EMPTY_V))
    {
        return 0U;
    }

    if (battery_voltage >= BATTERY_FULL_V)
    {
        return 100U;
    }

    return (unsigned int) lroundf ( (battery_voltage - BATTERY_EMPTY_V) * 100.0F
                                    / (BATTERY_FULL_V - BATTERY_EMPTY_V));
}

bool app_homekit_battery_is_low (const float battery_voltage, const float temperature)
{
    // Same thresholds as Ruuvi Station: cold batteries sag under load.
    float threshold = 2.5F;

    if (temperature <= -20.0F)
    {
        threshold = 2.0F;
    }
    else if (temperature < 0.0F)
    {
        threshold = 2.3F;
    }

    return battery_voltage < threshold;
}

#if APP_HOMEKIT_ENABLED

#include "HAP.h"
#include "HAPPlatform+Init.h"
#include "homekit_db.h"

#include "app_led.h"
#include "ruuvi_boards.h"
#include "ruuvi_interface_communication_radio.h"
#include "ruuvi_interface_log.h"
#include "ruuvi_interface_power.h"
#include "sdk_config.h"

static const HAPLogObject logObject = { .subsystem = "ruuvi", .category = "HomeKit" };

/** Application KVS domain (the ADK uses 0x80 and up). */
#define kAppDomain               ((HAPPlatformKeyValueStoreDomain) 0x00)
#define kAppKey_FlashMigrated    ((HAPPlatformKeyValueStoreKey) 0x00)

/** FDS file of the Ruuvi history log (app_log.c). Unused in the HomeKit variant. */
#define RUUVI_LOG_FILE_ID (0xF0U)

/** Number of LED toggles for HomeKit Identify. */
#define IDENTIFY_TOGGLES (6U)
#define IDENTIFY_TOGGLE_MS (250U)

/**
 * While disconnected, each event is broadcast for kHAPBLENotification_MinDuration (3 s) and a
 * new event replaces the running broadcast, so events are sent one at a time this far apart.
 */
#define EVENT_SPACING_MS (3100U)

/** Events waiting to be raised, in sending order. */
enum
{
    EVENT_TEMPERATURE = 1U << 0U,
    EVENT_HUMIDITY    = 1U << 1U,
    EVENT_BATTERY_LOW = 1U << 2U,
    EVENT_BATTERY     = 1U << 3U,
};

static struct
{
    HAPPlatformKeyValueStore key_value_store;
    HAPPlatformAccessorySetup accessory_setup;
    HAPPlatformBLEPeripheralManager ble_peripheral_manager;
    HAPPlatform platform;
    HAPAccessoryServerRef server;
    HAPAccessory accessory;
    char name[32];
    char serial_number[16];
    bool is_running;
    bool reset_requested;

    float temperature;        //!< Latest reading, NAN if unknown.
    float humidity;           //!< Latest reading, NAN if unknown.
    float event_temperature;  //!< Value of the last raised event.
    float event_humidity;     //!< Value of the last raised event.
    HAPTime event_temperature_time; //!< Time of the last temperature event.
    HAPTime event_humidity_time;    //!< Time of the last humidity event.
    HAPTime event_time;             //!< Time of the last raised event of any characteristic.
    bool has_event_time;            //!< event_time is valid.
    unsigned int pending_events;    //!< EVENT_* bits.
    HAPPlatformTimerRef event_timer;
    uint8_t battery_percent;
    bool battery_low;
    bool has_battery;

    unsigned int identify_toggles;
    HAPPlatformTimerRef identify_timer;
} m_hk; //!< Zero-initialized (bss): readings are set to NAN in app_homekit_init.

// ---------------------------------------------------------------------------------------------------------------------
// Characteristic callbacks.

HAPError homekit_handle_temperature_read (HAPAccessoryServerRef * server HAP_UNUSED,
        const HAPFloatCharacteristicReadRequest * request HAP_UNUSED,
        float * value,
        void * _Nullable context HAP_UNUSED)
{
    if (isnan (m_hk.temperature))
    {
        return kHAPError_Busy;
    }

    float t = roundf (m_hk.temperature * 10.0F) / 10.0F;
    *value = t < -40.0F ? -40.0F : (t > 85.0F ? 85.0F : t);
    return kHAPError_None;
}

HAPError homekit_handle_humidity_read (HAPAccessoryServerRef * server HAP_UNUSED,
                                       const HAPFloatCharacteristicReadRequest * request HAP_UNUSED,
                                       float * value,
                                       void * _Nullable context HAP_UNUSED)
{
    if (isnan (m_hk.humidity))
    {
        return kHAPError_Busy;
    }

    float h = roundf (m_hk.humidity);
    *value = h < 0.0F ? 0.0F : (h > 100.0F ? 100.0F : h);
    return kHAPError_None;
}

HAPError homekit_handle_battery_level_read (HAPAccessoryServerRef * server HAP_UNUSED,
        const HAPUInt8CharacteristicReadRequest * request HAP_UNUSED,
        uint8_t * value,
        void * _Nullable context HAP_UNUSED)
{
    *value = m_hk.has_battery ? m_hk.battery_percent : 100U;
    return kHAPError_None;
}

HAPError homekit_handle_charging_state_read (HAPAccessoryServerRef * server HAP_UNUSED,
        const HAPUInt8CharacteristicReadRequest * request HAP_UNUSED,
        uint8_t * value,
        void * _Nullable context HAP_UNUSED)
{
    *value = kHAPCharacteristicValue_ChargingState_NotChargeable;
    return kHAPError_None;
}

HAPError homekit_handle_status_low_battery_read (HAPAccessoryServerRef * server HAP_UNUSED,
        const HAPUInt8CharacteristicReadRequest * request HAP_UNUSED,
        uint8_t * value,
        void * _Nullable context HAP_UNUSED)
{
    *value = m_hk.battery_low ? kHAPCharacteristicValue_StatusLowBattery_Low :
             kHAPCharacteristicValue_StatusLowBattery_Normal;
    return kHAPError_None;
}

static void identify_timer_expired (HAPPlatformTimerRef timer HAP_UNUSED, void * _Nullable context HAP_UNUSED)
{
    m_hk.identify_timer = 0;

    if (m_hk.identify_toggles)
    {
        m_hk.identify_toggles--;
        app_led_activity_signal ( (m_hk.identify_toggles % 2U) == 1U);
        HAPError err = HAPPlatformTimerRegister (&m_hk.identify_timer,
                       HAPPlatformClockGetCurrent() + IDENTIFY_TOGGLE_MS,
                       identify_timer_expired, NULL);

        if (err)
        {
            app_led_activity_signal (false);
        }
    }
}

static HAPError handle_identify (HAPAccessoryServerRef * server HAP_UNUSED,
                                 const HAPAccessoryIdentifyRequest * request HAP_UNUSED,
                                 void * _Nullable context HAP_UNUSED)
{
    HAPLogInfo (&logObject, "Identify.");

    if (!m_hk.identify_timer)
    {
        m_hk.identify_toggles = IDENTIFY_TOGGLES;
        identify_timer_expired (0, NULL);
    }

    return kHAPError_None;
}

// ---------------------------------------------------------------------------------------------------------------------
// Accessory server.

static void handle_updated_state (HAPAccessoryServerRef * server, void * _Nullable context HAP_UNUSED)
{
    HAPAccessoryServerState state = HAPAccessoryServerGetState (server);

    if (state == kHAPAccessoryServerState_Running)
    {
        HAPLogInfo (&logObject, "Accessory server running.");
        return;
    }

    if (state == kHAPAccessoryServerState_Idle && m_hk.reset_requested)
    {
        HAPLogInfo (&logObject, "Removing HomeKit pairings.");
        HAPError err = HAPRestoreFactorySettings (&m_hk.key_value_store);

        if (err)
        {
            HAPLogError (&logObject, "Restoring factory settings failed.");
        }

        // Start over with a fresh accessory server (and a new Bluetooth address).
        ri_power_reset();
    }
}

/**
 * The first boot after flashing replaces the Ruuvi firmware: drop its history log so that the
 * flash space is available to HomeKit and garbage collection never runs during pairing.
 */
static void migrate_flash (void)
{
    bool found = false;
    uint8_t marker = 1;
    HAPError err = HAPPlatformKeyValueStoreGet (&m_hk.key_value_store, kAppDomain, kAppKey_FlashMigrated,
                   NULL, 0, NULL, &found);

    if (!err && found)
    {
        return;
    }

    HAPLogInfo (&logObject, "Removing Ruuvi history log from flash.");
    // Best effort: a failure only means the old log keeps using flash space.
    err = HAPPlatformKeyValueStoreDeleteFile (&m_hk.key_value_store, RUUVI_LOG_FILE_ID);

    if (!err)
    {
        err = HAPPlatformKeyValueStoreCompact (&m_hk.key_value_store, 1);
    }

    if (!err)
    {
        err = HAPPlatformKeyValueStoreSet (&m_hk.key_value_store, kAppDomain, kAppKey_FlashMigrated,
                                           &marker, sizeof marker);
    }

    if (err)
    {
        HAPLogError (&logObject, "Flash migration failed, retrying on next boot.");
    }
}

/** Services of the accessory. Must outlive the accessory server. */
static const HAPService * const m_services[] =
{
    &homekit_accessory_information_service,
    &homekit_protocol_information_service,
    &homekit_pairing_service,
    &homekit_temperature_service,
    &homekit_humidity_service,
    &homekit_battery_service,
    NULL
};

static void create_accessory (void)
{
    uint64_t address = 0;
    (void) ri_radio_address_get (&address);
    // Same naming as the Ruuvi app: last two bytes of the factory MAC address.
    snprintf (m_hk.name, sizeof (m_hk.name), "RuuviTag %04X", (unsigned int) (address & 0xFFFFU));
    snprintf (m_hk.serial_number, sizeof (m_hk.serial_number), "%012llX",
              (unsigned long long) (address & 0xFFFFFFFFFFFFULL));
    m_hk.accessory = (HAPAccessory)
    {
        .aid = 1,
        .category = kHAPAccessoryCategory_Sensors,
        .name = m_hk.name,
        .manufacturer = RB_MANUFACTURER_STRING,
        .model = RB_MODEL_STRING,
        .serialNumber = m_hk.serial_number,
        .firmwareVersion = APP_HOMEKIT_FIRMWARE_REVISION,
        .hardwareVersion = NULL,
        .services = m_services,
        .callbacks = { .identify = handle_identify },
    };
}

rd_status_t app_homekit_init (void)
{
    static HAPBLEGATTTableElementRef gatt_table_elements[kHomeKitAttributeCount];
    static HAPBLESessionCacheElementRef session_cache_elements[kHAPBLESessionCache_MinElements];
    static HAPSessionRef session;
    static uint8_t procedure_bytes[2048];
    static HAPBLEProcedureRef procedures[1];
    static HAPBLEAccessoryServerStorage ble_storage =
    {
        .gattTableElements = gatt_table_elements,
        .numGATTTableElements = HAPArrayCount (gatt_table_elements),
        .sessionCacheElements = session_cache_elements,
        .numSessionCacheElements = HAPArrayCount (session_cache_elements),
        .session = &session,
        .procedures = procedures,
        .numProcedures = HAPArrayCount (procedures),
        .procedureBuffer = { .bytes = procedure_bytes, .numBytes = sizeof procedure_bytes },
    };

    if (m_hk.is_running)
    {
        return RD_ERROR_INVALID_STATE;
    }

    m_hk.temperature = NAN;
    m_hk.humidity = NAN;
    m_hk.event_temperature = NAN;
    m_hk.event_humidity = NAN;

    if (HAPPlatformTimerInit())
    {
        return RD_ERROR_RESOURCES;
    }

    HAPError err = HAPPlatformKeyValueStoreCreate (&m_hk.key_value_store,
                   & (const HAPPlatformKeyValueStoreOptions)
    {
        .baseFileID = APP_HOMEKIT_KVS_BASE_FILE_ID
    });

    if (err)
    {
        return RD_ERROR_INTERNAL;
    }

    migrate_flash();
    HAPPlatformAccessorySetupCreate (&m_hk.accessory_setup);
    HAPPlatformBLEPeripheralManagerCreate (&m_hk.ble_peripheral_manager,
                                           & (const HAPPlatformBLEPeripheralManagerOptions)
    {
        .txPower = APP_HOMEKIT_TX_POWER_DBM,
        .connectionConfigurationTag = RUUVI_NRF5_SDK15_BLE4_STACK_CONN_TAG,
    });
    m_hk.platform = (HAPPlatform)
    {
        .keyValueStore = &m_hk.key_value_store,
        .accessorySetup = &m_hk.accessory_setup,
        .ble = { .blePeripheralManager = &m_hk.ble_peripheral_manager },
    };
    create_accessory();
    HAPAccessoryServerCreate (&m_hk.server,
                              & (const HAPAccessoryServerOptions)
    {
        .maxPairings = kHAPPairingStorage_MinElements,
        .ble = {
            .transport = &kHAPAccessoryServerTransport_BLE,
            .accessoryServerStorage = &ble_storage,
            .preferredAdvertisingInterval =
            HAPBLEAdvertisingIntervalCreateFromMilliseconds (APP_HOMEKIT_ADVERTISING_INTERVAL_MS),
            .preferredNotificationDuration = kHAPBLENotification_MinDuration,
        },
    },
    &m_hk.platform,
    & (const HAPAccessoryServerCallbacks)
    {
        .handleUpdatedState = handle_updated_state
    },
    NULL);
    HAPAccessoryServerStart (&m_hk.server, &m_hk.accessory);
    m_hk.is_running = true;
    return RD_SUCCESS;
}

_Static_assert ( (APP_HOMEKIT_ADVERTISING_INTERVAL_MS >= 160) && (APP_HOMEKIT_ADVERTISING_INTERVAL_MS <= 2500),
                 "HAP allows regular advertising intervals of 160 .. 2500 ms");
_Static_assert ( (APP_HOMEKIT_TX_POWER_DBM == -40) || (APP_HOMEKIT_TX_POWER_DBM == -20)
                 || (APP_HOMEKIT_TX_POWER_DBM == -16) || (APP_HOMEKIT_TX_POWER_DBM == -12)
                 || (APP_HOMEKIT_TX_POWER_DBM == -8) || (APP_HOMEKIT_TX_POWER_DBM == -4)
                 || (APP_HOMEKIT_TX_POWER_DBM == 0) || (APP_HOMEKIT_TX_POWER_DBM == 3)
                 || (APP_HOMEKIT_TX_POWER_DBM == 4),
                 "nRF52832 supports -40, -20, -16, -12, -8, -4, 0, 3 and 4 dBm");

static void event_timer_expired (HAPPlatformTimerRef timer, void * _Nullable context);

/**
 * @brief Raise pending events: all of them while connected, otherwise one per EVENT_SPACING_MS.
 */
static void raise_pending_events (void)
{
    static const struct
    {
        unsigned int event;
        const HAPCharacteristic * characteristic;
        const HAPService * service;
    } events[] =
    {
        { EVENT_TEMPERATURE, &homekit_temperature_characteristic, &homekit_temperature_service },
        { EVENT_HUMIDITY, &homekit_humidity_characteristic, &homekit_humidity_service },
        { EVENT_BATTERY_LOW, &homekit_battery_status_low_characteristic, &homekit_battery_service },
        { EVENT_BATTERY, &homekit_battery_level_characteristic, &homekit_battery_service },
    };

    if (!m_hk.is_running)
    {
        m_hk.pending_events = 0;
        return;
    }

    bool connected = HAPPlatformBLEPeripheralManagerIsConnected (&m_hk.ble_peripheral_manager);
    HAPTime now = HAPPlatformClockGetCurrent();

    for (size_t i = 0; (i < HAPArrayCount (events)) && m_hk.pending_events; i++)
    {
        if (! (m_hk.pending_events & events[i].event))
        {
            continue;
        }

        if (!connected && m_hk.has_event_time && (now - m_hk.event_time < EVENT_SPACING_MS))
        {
            if (!m_hk.event_timer)
            {
                HAPError err = HAPPlatformTimerRegister (&m_hk.event_timer, m_hk.event_time + EVENT_SPACING_MS,
                               event_timer_expired, NULL);
                (void) err; // Out of timers: the next sensor sample retries.
            }

            return;
        }

        m_hk.pending_events &= ~events[i].event;
        m_hk.event_time = now;
        m_hk.has_event_time = true;
        HAPAccessoryServerRaiseEvent (&m_hk.server, events[i].characteristic, events[i].service, &m_hk.accessory);
    }
}

static void event_timer_expired (HAPPlatformTimerRef timer HAP_UNUSED, void * _Nullable context HAP_UNUSED)
{
    m_hk.event_timer = 0;
    raise_pending_events();
}

/**
 * @brief Decide whether a new reading is worth an event, see APP_HOMEKIT_EVENT_MIN_INTERVAL_MS.
 *
 * @param[in] value New reading.
 * @param[in,out] event_value Value of the last event, updated if an event is due.
 * @param[in,out] event_time Time of the last event, updated if an event is due.
 * @param[in] delta Minimum change for an event.
 * @param[in] urgent_delta Change that is sent without waiting for the minimum interval.
 * @return True if an event should be raised.
 */
static bool event_due (const float value, float * const event_value, HAPTime * const event_time,
                       const float delta, const float urgent_delta)
{
    HAPTime now = HAPPlatformClockGetCurrent();
    float change = fabsf (value - *event_value);
    bool due = isnan (*event_value)
               || (change >= urgent_delta)
               || ( (change >= delta) && (now - *event_time >= APP_HOMEKIT_EVENT_MIN_INTERVAL_MS));

    if (due)
    {
        *event_value = value;
        *event_time = now;
    }

    return due;
}

void app_homekit_sensor_update (const rd_sensor_data_t * const data)
{
    if (NULL == data)
    {
        return;
    }

    float temperature = rd_sensor_data_parse (data, RD_SENSOR_TEMP_FIELD);
    float humidity = rd_sensor_data_parse (data, RD_SENSOR_HUMI_FIELD);

    if (!isnan (temperature))
    {
        m_hk.temperature = temperature;

        if (event_due (temperature, &m_hk.event_temperature, &m_hk.event_temperature_time,
                       APP_HOMEKIT_TEMPERATURE_EVENT_DELTA, APP_HOMEKIT_TEMPERATURE_URGENT_DELTA))
        {
            m_hk.pending_events |= EVENT_TEMPERATURE;
        }
    }

    if (!isnan (humidity))
    {
        m_hk.humidity = humidity;

        if (event_due (humidity, &m_hk.event_humidity, &m_hk.event_humidity_time,
                       APP_HOMEKIT_HUMIDITY_EVENT_DELTA, APP_HOMEKIT_HUMIDITY_URGENT_DELTA))
        {
            m_hk.pending_events |= EVENT_HUMIDITY;
        }
    }

    raise_pending_events();
}

void app_homekit_battery_update (const float battery_voltage, const float temperature)
{
    if (isnan (battery_voltage))
    {
        return;
    }

    uint8_t percent = (uint8_t) app_homekit_battery_percent (battery_voltage);
    bool low = app_homekit_battery_is_low (battery_voltage, isnan (temperature) ? 20.0F : temperature);
    // Report percentage changes in 5 % steps to avoid waking controllers for ADC noise.
    bool level_changed = !m_hk.has_battery
                         || (percent > m_hk.battery_percent + 4U) || (percent + 4U < m_hk.battery_percent);
    bool low_changed = !m_hk.has_battery || (low != m_hk.battery_low);
    m_hk.has_battery = true;

    if (level_changed)
    {
        m_hk.battery_percent = percent;
        m_hk.pending_events |= EVENT_BATTERY;
    }

    if (low_changed)
    {
        m_hk.battery_low = low;
        m_hk.pending_events |= EVENT_BATTERY_LOW;
    }

    raise_pending_events();
}

bool app_homekit_is_operational (void)
{
    return m_hk.is_running && !m_hk.reset_requested
           && HAPPlatformBLEPeripheralManagerIsActive (&m_hk.ble_peripheral_manager);
}

void app_homekit_maintenance (void)
{
    if (!m_hk.is_running || HAPPlatformBLEPeripheralManagerIsConnected (&m_hk.ble_peripheral_manager))
    {
        return;
    }

    HAPError err = HAPPlatformKeyValueStoreCompact (&m_hk.key_value_store,
                   APP_HOMEKIT_FLASH_COMPACT_DIRTY_RECORDS);

    if (err)
    {
        HAPLogError (&logObject, "Flash compaction failed.");
    }
}

void app_homekit_reset_pairings (void)
{
    if (!m_hk.is_running || m_hk.reset_requested)
    {
        return;
    }

    m_hk.reset_requested = true;
    // Continues in handle_updated_state once the server has stopped.
    HAPAccessoryServerStop (&m_hk.server);
}

#endif
