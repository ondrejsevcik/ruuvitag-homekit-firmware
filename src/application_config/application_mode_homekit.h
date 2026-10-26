#ifndef APPLICATION_MODE_HOMEKIT_H
#define APPLICATION_MODE_HOMEKIT_H

/**
 * @file application_mode_homekit.h
 * @brief Native Apple HomeKit (HAP over Bluetooth LE) variant.
 *
 * The tag is a HomeKit accessory instead of a Ruuvi broadcaster:
 * - Bluetooth is owned by the HomeKit stack (src/homekit). Ruuvi advertisements,
 *   the Ruuvi GATT services (NUS, DIS, buttonless DFU) and history logging are disabled.
 * - NFC keeps working and shows the current readings in the Ruuvi app.
 * - Button: hold 5 s to enter the bootloader for firmware updates (pairings are kept),
 *   hold 15 s to reset HomeKit pairings.
 */

#define APP_HOMEKIT_ENABLED (1U)

#ifndef APP_FW_VARIANT
#   define APP_FW_VARIANT "+homekit"
#endif

/** HomeKit firmware revision, must be "major[.minor[.revision]]". */
#ifndef APP_HOMEKIT_FIRMWARE_REVISION
#   define APP_HOMEKIT_FIRMWARE_REVISION "1.0.0"
#endif

/** Sensor sampling interval. HomeKit is not real-time, slower sampling saves battery. */
#ifndef APP_HEARTBEAT_INTERVAL_MS
#   define APP_HEARTBEAT_INTERVAL_MS (10000U)
#endif

/** Regular HomeKit advertising interval. HAP allows 160 .. 2500 ms. */
#ifndef APP_HOMEKIT_ADVERTISING_INTERVAL_MS
#   define APP_HOMEKIT_ADVERTISING_INTERVAL_MS (1022.5F)
#endif

/** Radio transmit power in dBm. Same as the Ruuvi default. */
#ifndef APP_HOMEKIT_TX_POWER_DBM
#   define APP_HOMEKIT_TX_POWER_DBM (4)
#endif

/** Minimum changes that are pushed to HomeKit as events. Reads always get the latest value. */
#ifndef APP_HOMEKIT_TEMPERATURE_EVENT_DELTA
#   define APP_HOMEKIT_TEMPERATURE_EVENT_DELTA (0.2F)
#endif
#ifndef APP_HOMEKIT_HUMIDITY_EVENT_DELTA
#   define APP_HOMEKIT_HUMIDITY_EVENT_DELTA (1.0F)
#endif

/** Button hold times. */
#ifndef APP_BUTTON_LONG_PRESS_TIME_MS
#   define APP_BUTTON_LONG_PRESS_TIME_MS (5000U)
#endif
#ifndef APP_HOMEKIT_RESET_PRESS_TIME_MS
#   define APP_HOMEKIT_RESET_PRESS_TIME_MS (15000U)
#endif

/**
 * Flash storage: 4 data pages + 1 swap page = 20 kB at 0x70000 .. 0x75000, exactly the
 * application data area the Ruuvi bootloader preserves during firmware updates
 * (NRF_DFU_APP_DATA_AREA_SIZE 20480). HomeKit pairings therefore survive updates.
 * Must match .storage_flash in ruuvitag_b_homekit.ld.
 */
#define APP_FLASH_PAGES (4U)

/** Compact flash while disconnected once this many records are deleted. */
#ifndef APP_HOMEKIT_FLASH_COMPACT_DIRTY_RECORDS
#   define APP_HOMEKIT_FLASH_COMPACT_DIRTY_RECORDS (24U)
#endif

/** FDS file IDs 0x4800 .. 0x48FF hold the HomeKit key-value store. */
#ifndef APP_HOMEKIT_KVS_BASE_FILE_ID
#   define APP_HOMEKIT_KVS_BASE_FILE_ID (0x4800U)
#endif

// Ruuvi communication features replaced by HomeKit.
#define APP_ADV_ENABLED  (0U)
#define APP_GATT_ENABLED (0U)
#define RI_RADIO_ENABLED (1U)
#define APP_LOG_ENABLED  (0U)

// SoftDevice configuration for the HomeKit GATT database.
#define NRF_SDH_BLE_PERIPHERAL_LINK_COUNT (1U)
#define NRF_SDH_BLE_CENTRAL_LINK_COUNT    (0U)
#define NRF_SDH_BLE_TOTAL_LINK_COUNT      (1U)
#define NRF_SDH_BLE_VS_UUID_COUNT         (6U)
#define NRF_SDH_BLE_GATTS_ATTR_TAB_SIZE   (0x1400U)
#define NRF_SDH_BLE_GATT_MAX_MTU_SIZE     (185U)
#define NRF_SDH_BLE_GAP_DATA_LENGTH       (189U)
#define NRF_SDH_BLE_SERVICE_CHANGED       (1U)

#endif
