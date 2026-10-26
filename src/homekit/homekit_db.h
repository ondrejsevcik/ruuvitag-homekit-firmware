/**
 * @file homekit_db.h
 * @brief HomeKit attribute database of the RuuviTag accessory.
 */
#ifndef HOMEKIT_DB_H
#define HOMEKIT_DB_H

#include "HAP.h"

/** Total number of HomeKit attributes (services and characteristics). */
#define kHomeKitAttributeCount ((size_t) 29)

extern const HAPService homekit_accessory_information_service;
extern const HAPService homekit_protocol_information_service;
extern const HAPService homekit_pairing_service;
extern const HAPService homekit_temperature_service;
extern const HAPService homekit_humidity_service;
extern const HAPService homekit_battery_service;

extern const HAPFloatCharacteristic homekit_temperature_characteristic;
extern const HAPFloatCharacteristic homekit_humidity_characteristic;
extern const HAPUInt8Characteristic homekit_battery_level_characteristic;
extern const HAPUInt8Characteristic homekit_battery_status_low_characteristic;

/* Implemented by app_homekit.c. */
HAP_RESULT_USE_CHECK
HAPError homekit_handle_temperature_read(
        HAPAccessoryServerRef* server,
        const HAPFloatCharacteristicReadRequest* request,
        float* value,
        void* _Nullable context);
HAP_RESULT_USE_CHECK
HAPError homekit_handle_humidity_read(
        HAPAccessoryServerRef* server,
        const HAPFloatCharacteristicReadRequest* request,
        float* value,
        void* _Nullable context);
HAP_RESULT_USE_CHECK
HAPError homekit_handle_battery_level_read(
        HAPAccessoryServerRef* server,
        const HAPUInt8CharacteristicReadRequest* request,
        uint8_t* value,
        void* _Nullable context);
HAP_RESULT_USE_CHECK
HAPError homekit_handle_charging_state_read(
        HAPAccessoryServerRef* server,
        const HAPUInt8CharacteristicReadRequest* request,
        uint8_t* value,
        void* _Nullable context);
HAP_RESULT_USE_CHECK
HAPError homekit_handle_status_low_battery_read(
        HAPAccessoryServerRef* server,
        const HAPUInt8CharacteristicReadRequest* request,
        uint8_t* value,
        void* _Nullable context);

#endif
