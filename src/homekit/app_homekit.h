/**
 * @file app_homekit.h
 * @brief Native HomeKit accessory: glue between the Ruuvi application and the HomeKit ADK.
 */
#ifndef APP_HOMEKIT_H
#define APP_HOMEKIT_H

#include <stdbool.h>

#include "ruuvi_driver_error.h"
#include "ruuvi_driver_sensor.h"

/**
 * @brief Start the HomeKit accessory.
 *
 * Requires the SoftDevice (ri_radio_init), flash storage (rt_flash_init),
 * RTC (app_sensor_init) and timers to be initialized.
 */
rd_status_t app_homekit_init (void);

/**
 * @brief Pass a new sensor sample to HomeKit. Call from main context.
 *
 * Temperature and humidity changes larger than the configured deltas are
 * pushed to HomeKit controllers as events.
 */
void app_homekit_sensor_update (const rd_sensor_data_t * const data);

/**
 * @brief Pass a new battery voltage to HomeKit. Call from main context.
 */
void app_homekit_battery_update (const float battery_voltage, const float temperature);

/**
 * @brief Whether the accessory server runs and the tag is advertising or connected.
 *
 * Used to feed the watchdog only while HomeKit is reachable.
 */
bool app_homekit_is_operational (void);

/**
 * @brief Periodic housekeeping, call from main context (e.g. every heartbeat).
 *
 * Compacts flash storage while no controller is connected.
 */
void app_homekit_maintenance (void);

/**
 * @brief Remove all HomeKit pairings and restart, making the accessory pairable again.
 */
void app_homekit_reset_pairings (void);

/**
 * @brief Map a CR2477 coin cell voltage to a battery percentage. Exposed for tests.
 */
unsigned int app_homekit_battery_percent (const float battery_voltage);

/**
 * @brief Battery is low, same thresholds as Ruuvi Station. Exposed for tests.
 */
bool app_homekit_battery_is_low (const float battery_voltage, const float temperature);

#endif
