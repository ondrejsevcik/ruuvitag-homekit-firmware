/**
 * @file homekit_db.c
 * @brief HomeKit attribute database of the RuuviTag accessory.
 *
 * Services:
 * - Accessory Information, HAP Protocol Information, Pairing (mandatory)
 * - Temperature Sensor (primary)
 * - Humidity Sensor
 * - Battery
 *
 * Sensor readings support event notifications. While no controller is
 * connected the ADK delivers them as broadcast or disconnected events, so a
 * home hub (HomePod / Apple TV) is updated without keeping a connection open.
 */

#include "homekit_db.h"

// Instance IDs. Never reuse or renumber: controllers cache them.
#define kIID_AccessoryInformation                 ((uint64_t) 0x0001)
#define kIID_AccessoryInformationIdentify         ((uint64_t) 0x0002)
#define kIID_AccessoryInformationManufacturer     ((uint64_t) 0x0003)
#define kIID_AccessoryInformationModel            ((uint64_t) 0x0004)
#define kIID_AccessoryInformationName             ((uint64_t) 0x0005)
#define kIID_AccessoryInformationSerialNumber     ((uint64_t) 0x0006)
#define kIID_AccessoryInformationFirmwareRevision ((uint64_t) 0x0007)
#define kIID_AccessoryInformationADKVersion       ((uint64_t) 0x0009)

#define kIID_HAPProtocolInformation                 ((uint64_t) 0x0010)
#define kIID_HAPProtocolInformationServiceSignature ((uint64_t) 0x0011)
#define kIID_HAPProtocolInformationVersion          ((uint64_t) 0x0012)

#define kIID_Pairing                ((uint64_t) 0x0020)
#define kIID_PairingPairSetup       ((uint64_t) 0x0022)
#define kIID_PairingPairVerify      ((uint64_t) 0x0023)
#define kIID_PairingPairingFeatures ((uint64_t) 0x0024)
#define kIID_PairingPairingPairings ((uint64_t) 0x0025)

#define kIID_Temperature                 ((uint64_t) 0x0030)
#define kIID_TemperatureServiceSignature ((uint64_t) 0x0031)
#define kIID_TemperatureName             ((uint64_t) 0x0032)
#define kIID_TemperatureCurrent          ((uint64_t) 0x0033)

#define kIID_Humidity                 ((uint64_t) 0x0040)
#define kIID_HumidityServiceSignature ((uint64_t) 0x0041)
#define kIID_HumidityName             ((uint64_t) 0x0042)
#define kIID_HumidityCurrent          ((uint64_t) 0x0043)

#define kIID_Battery                 ((uint64_t) 0x0050)
#define kIID_BatteryServiceSignature ((uint64_t) 0x0051)
#define kIID_BatteryLevel            ((uint64_t) 0x0052)
#define kIID_BatteryChargingState    ((uint64_t) 0x0053)
#define kIID_BatteryStatusLowBattery ((uint64_t) 0x0054)

HAP_STATIC_ASSERT(kHomeKitAttributeCount == 8 + 3 + 5 + 4 + 4 + 5, AttributeCount_mismatch);

/** Characteristic properties. BLE broadcast and disconnected events only make sense with event support. */
#define PROPERTIES(readable_, writable_, events_, hidden_, readableWithoutSecurity_, writableWithoutSecurity_) \
    { \
        .readable = (readable_), .writable = (writable_), .supportsEventNotification = (events_), \
        .hidden = (hidden_), .requiresTimedWrite = false, .supportsAuthorizationData = false, \
        .ip = { .controlPoint = false, .supportsWriteResponse = false }, \
        .ble = { .supportsBroadcastNotification = (events_), \
                 .supportsDisconnectedNotification = (events_), \
                 .readableWithoutSecurity = (readableWithoutSecurity_), \
                 .writableWithoutSecurity = (writableWithoutSecurity_) } \
    }

#define READ_ONLY PROPERTIES(true, false, false, false, false, false)

#define STRING_CHARACTERISTIC(name_, iid_, type_, description_, handler_) \
    static const HAPStringCharacteristic name_ = { \
        .format = kHAPCharacteristicFormat_String, \
        .iid = (iid_), \
        .characteristicType = &(type_), \
        .debugDescription = (description_), \
        .manufacturerDescription = NULL, \
        .properties = READ_ONLY, \
        .constraints = { .maxLength = 64 }, \
        .callbacks = { .handleRead = (handler_), .handleWrite = NULL } \
    }

#define SERVICE_SIGNATURE_CHARACTERISTIC(name_, iid_) \
    static const HAPDataCharacteristic name_ = { \
        .format = kHAPCharacteristicFormat_Data, \
        .iid = (iid_), \
        .characteristicType = &kHAPCharacteristicType_ServiceSignature, \
        .debugDescription = kHAPCharacteristicDebugDescription_ServiceSignature, \
        .manufacturerDescription = NULL, \
        .properties = PROPERTIES(true, false, false, false, false, false), \
        .constraints = { .maxLength = 2097152 }, \
        .callbacks = { .handleRead = HAPHandleServiceSignatureRead, .handleWrite = NULL } \
    }

//----------------------------------------------------------------------------------------------------------------------
// Accessory Information.

static const HAPBoolCharacteristic accessoryInformationIdentify = {
    .format = kHAPCharacteristicFormat_Bool,
    .iid = kIID_AccessoryInformationIdentify,
    .characteristicType = &kHAPCharacteristicType_Identify,
    .debugDescription = kHAPCharacteristicDebugDescription_Identify,
    .manufacturerDescription = NULL,
    .properties = PROPERTIES(false, true, false, false, false, false),
    .callbacks = { .handleRead = NULL, .handleWrite = HAPHandleAccessoryInformationIdentifyWrite }
};

STRING_CHARACTERISTIC(
        accessoryInformationManufacturer,
        kIID_AccessoryInformationManufacturer,
        kHAPCharacteristicType_Manufacturer,
        kHAPCharacteristicDebugDescription_Manufacturer,
        HAPHandleAccessoryInformationManufacturerRead);
STRING_CHARACTERISTIC(
        accessoryInformationModel,
        kIID_AccessoryInformationModel,
        kHAPCharacteristicType_Model,
        kHAPCharacteristicDebugDescription_Model,
        HAPHandleAccessoryInformationModelRead);
STRING_CHARACTERISTIC(
        accessoryInformationName,
        kIID_AccessoryInformationName,
        kHAPCharacteristicType_Name,
        kHAPCharacteristicDebugDescription_Name,
        HAPHandleAccessoryInformationNameRead);
STRING_CHARACTERISTIC(
        accessoryInformationSerialNumber,
        kIID_AccessoryInformationSerialNumber,
        kHAPCharacteristicType_SerialNumber,
        kHAPCharacteristicDebugDescription_SerialNumber,
        HAPHandleAccessoryInformationSerialNumberRead);
STRING_CHARACTERISTIC(
        accessoryInformationFirmwareRevision,
        kIID_AccessoryInformationFirmwareRevision,
        kHAPCharacteristicType_FirmwareRevision,
        kHAPCharacteristicDebugDescription_FirmwareRevision,
        HAPHandleAccessoryInformationFirmwareRevisionRead);

static const HAPStringCharacteristic accessoryInformationADKVersion = {
    .format = kHAPCharacteristicFormat_String,
    .iid = kIID_AccessoryInformationADKVersion,
    .characteristicType = &kHAPCharacteristicType_ADKVersion,
    .debugDescription = kHAPCharacteristicDebugDescription_ADKVersion,
    .manufacturerDescription = NULL,
    .properties = PROPERTIES(true, false, false, true, false, false),
    .constraints = { .maxLength = 64 },
    .callbacks = { .handleRead = HAPHandleAccessoryInformationADKVersionRead, .handleWrite = NULL }
};

const HAPService homekit_accessory_information_service = {
    .iid = kIID_AccessoryInformation,
    .serviceType = &kHAPServiceType_AccessoryInformation,
    .debugDescription = kHAPServiceDebugDescription_AccessoryInformation,
    .name = NULL,
    .properties = { .primaryService = false, .hidden = false, .ble = { .supportsConfiguration = false } },
    .linkedServices = NULL,
    .characteristics = (const HAPCharacteristic* const[]) { &accessoryInformationIdentify,
                                                            &accessoryInformationManufacturer,
                                                            &accessoryInformationModel,
                                                            &accessoryInformationName,
                                                            &accessoryInformationSerialNumber,
                                                            &accessoryInformationFirmwareRevision,
                                                            &accessoryInformationADKVersion,
                                                            NULL }
};

//----------------------------------------------------------------------------------------------------------------------
// HAP Protocol Information.

static const HAPDataCharacteristic hapProtocolInformationServiceSignature = {
    .format = kHAPCharacteristicFormat_Data,
    .iid = kIID_HAPProtocolInformationServiceSignature,
    .characteristicType = &kHAPCharacteristicType_ServiceSignature,
    .debugDescription = kHAPCharacteristicDebugDescription_ServiceSignature,
    .manufacturerDescription = NULL,
    .properties = PROPERTIES(true, false, false, false, false, false),
    .constraints = { .maxLength = 2097152 },
    .callbacks = { .handleRead = HAPHandleServiceSignatureRead, .handleWrite = NULL }
};

STRING_CHARACTERISTIC(
        hapProtocolInformationVersion,
        kIID_HAPProtocolInformationVersion,
        kHAPCharacteristicType_Version,
        kHAPCharacteristicDebugDescription_Version,
        HAPHandleHAPProtocolInformationVersionRead);

const HAPService homekit_protocol_information_service = {
    .iid = kIID_HAPProtocolInformation,
    .serviceType = &kHAPServiceType_HAPProtocolInformation,
    .debugDescription = kHAPServiceDebugDescription_HAPProtocolInformation,
    .name = NULL,
    .properties = { .primaryService = false, .hidden = false, .ble = { .supportsConfiguration = true } },
    .linkedServices = NULL,
    .characteristics = (const HAPCharacteristic* const[]) { &hapProtocolInformationServiceSignature,
                                                            &hapProtocolInformationVersion,
                                                            NULL }
};

//----------------------------------------------------------------------------------------------------------------------
// Pairing.

static const HAPTLV8Characteristic pairingPairSetup = {
    .format = kHAPCharacteristicFormat_TLV8,
    .iid = kIID_PairingPairSetup,
    .characteristicType = &kHAPCharacteristicType_PairSetup,
    .debugDescription = kHAPCharacteristicDebugDescription_PairSetup,
    .manufacturerDescription = NULL,
    .properties = PROPERTIES(false, false, false, false, true, true),
    .callbacks = { .handleRead = HAPHandlePairingPairSetupRead, .handleWrite = HAPHandlePairingPairSetupWrite }
};

static const HAPTLV8Characteristic pairingPairVerify = {
    .format = kHAPCharacteristicFormat_TLV8,
    .iid = kIID_PairingPairVerify,
    .characteristicType = &kHAPCharacteristicType_PairVerify,
    .debugDescription = kHAPCharacteristicDebugDescription_PairVerify,
    .manufacturerDescription = NULL,
    .properties = PROPERTIES(false, false, false, false, true, true),
    .callbacks = { .handleRead = HAPHandlePairingPairVerifyRead, .handleWrite = HAPHandlePairingPairVerifyWrite }
};

static const HAPUInt8Characteristic pairingPairingFeatures = {
    .format = kHAPCharacteristicFormat_UInt8,
    .iid = kIID_PairingPairingFeatures,
    .characteristicType = &kHAPCharacteristicType_PairingFeatures,
    .debugDescription = kHAPCharacteristicDebugDescription_PairingFeatures,
    .manufacturerDescription = NULL,
    .properties = PROPERTIES(false, false, false, false, true, false),
    .units = kHAPCharacteristicUnits_None,
    .constraints = { .minimumValue = 0,
                     .maximumValue = UINT8_MAX,
                     .stepValue = 0,
                     .validValues = NULL,
                     .validValuesRanges = NULL },
    .callbacks = { .handleRead = HAPHandlePairingPairingFeaturesRead, .handleWrite = NULL }
};

static const HAPTLV8Characteristic pairingPairingPairings = {
    .format = kHAPCharacteristicFormat_TLV8,
    .iid = kIID_PairingPairingPairings,
    .characteristicType = &kHAPCharacteristicType_PairingPairings,
    .debugDescription = kHAPCharacteristicDebugDescription_PairingPairings,
    .manufacturerDescription = NULL,
    .properties = PROPERTIES(true, true, false, false, false, false),
    .callbacks = { .handleRead = HAPHandlePairingPairingPairingsRead,
                   .handleWrite = HAPHandlePairingPairingPairingsWrite }
};

const HAPService homekit_pairing_service = {
    .iid = kIID_Pairing,
    .serviceType = &kHAPServiceType_Pairing,
    .debugDescription = kHAPServiceDebugDescription_Pairing,
    .name = NULL,
    .properties = { .primaryService = false, .hidden = false, .ble = { .supportsConfiguration = false } },
    .linkedServices = NULL,
    .characteristics = (const HAPCharacteristic* const[]) { &pairingPairSetup,
                                                            &pairingPairVerify,
                                                            &pairingPairingFeatures,
                                                            &pairingPairingPairings,
                                                            NULL }
};

//----------------------------------------------------------------------------------------------------------------------
// Temperature Sensor.

SERVICE_SIGNATURE_CHARACTERISTIC(temperatureServiceSignature, kIID_TemperatureServiceSignature);
STRING_CHARACTERISTIC(
        temperatureName,
        kIID_TemperatureName,
        kHAPCharacteristicType_Name,
        kHAPCharacteristicDebugDescription_Name,
        HAPHandleNameRead);

const HAPFloatCharacteristic homekit_temperature_characteristic = {
    .format = kHAPCharacteristicFormat_Float,
    .iid = kIID_TemperatureCurrent,
    .characteristicType = &kHAPCharacteristicType_CurrentTemperature,
    .debugDescription = kHAPCharacteristicDebugDescription_CurrentTemperature,
    .manufacturerDescription = NULL,
    .properties = PROPERTIES(true, false, true, false, false, false),
    .units = kHAPCharacteristicUnits_Celsius,
    // RuuviTag sensors cover -40 .. +85 °C.
    .constraints = { .minimumValue = -40, .maximumValue = 85, .stepValue = 0.1F },
    .callbacks = { .handleRead = homekit_handle_temperature_read, .handleWrite = NULL }
};

const HAPService homekit_temperature_service = {
    .iid = kIID_Temperature,
    .serviceType = &kHAPServiceType_TemperatureSensor,
    .debugDescription = kHAPServiceDebugDescription_TemperatureSensor,
    .name = "Temperature",
    .properties = { .primaryService = true, .hidden = false, .ble = { .supportsConfiguration = false } },
    .linkedServices = NULL,
    .characteristics = (const HAPCharacteristic* const[]) { &temperatureServiceSignature,
                                                            &temperatureName,
                                                            &homekit_temperature_characteristic,
                                                            NULL }
};

//----------------------------------------------------------------------------------------------------------------------
// Humidity Sensor.

SERVICE_SIGNATURE_CHARACTERISTIC(humidityServiceSignature, kIID_HumidityServiceSignature);
STRING_CHARACTERISTIC(
        humidityName,
        kIID_HumidityName,
        kHAPCharacteristicType_Name,
        kHAPCharacteristicDebugDescription_Name,
        HAPHandleNameRead);

const HAPFloatCharacteristic homekit_humidity_characteristic = {
    .format = kHAPCharacteristicFormat_Float,
    .iid = kIID_HumidityCurrent,
    .characteristicType = &kHAPCharacteristicType_CurrentRelativeHumidity,
    .debugDescription = kHAPCharacteristicDebugDescription_CurrentRelativeHumidity,
    .manufacturerDescription = NULL,
    .properties = PROPERTIES(true, false, true, false, false, false),
    .units = kHAPCharacteristicUnits_Percentage,
    .constraints = { .minimumValue = 0, .maximumValue = 100, .stepValue = 1 },
    .callbacks = { .handleRead = homekit_handle_humidity_read, .handleWrite = NULL }
};

const HAPService homekit_humidity_service = {
    .iid = kIID_Humidity,
    .serviceType = &kHAPServiceType_HumiditySensor,
    .debugDescription = kHAPServiceDebugDescription_HumiditySensor,
    .name = "Humidity",
    .properties = { .primaryService = false, .hidden = false, .ble = { .supportsConfiguration = false } },
    .linkedServices = NULL,
    .characteristics = (const HAPCharacteristic* const[]) { &humidityServiceSignature,
                                                            &humidityName,
                                                            &homekit_humidity_characteristic,
                                                            NULL }
};

//----------------------------------------------------------------------------------------------------------------------
// Battery.

SERVICE_SIGNATURE_CHARACTERISTIC(batteryServiceSignature, kIID_BatteryServiceSignature);

const HAPUInt8Characteristic homekit_battery_level_characteristic = {
    .format = kHAPCharacteristicFormat_UInt8,
    .iid = kIID_BatteryLevel,
    .characteristicType = &kHAPCharacteristicType_BatteryLevel,
    .debugDescription = kHAPCharacteristicDebugDescription_BatteryLevel,
    .manufacturerDescription = NULL,
    .properties = PROPERTIES(true, false, true, false, false, false),
    .units = kHAPCharacteristicUnits_Percentage,
    .constraints = { .minimumValue = 0,
                     .maximumValue = 100,
                     .stepValue = 1,
                     .validValues = NULL,
                     .validValuesRanges = NULL },
    .callbacks = { .handleRead = homekit_handle_battery_level_read, .handleWrite = NULL }
};

static const HAPUInt8Characteristic batteryChargingState = {
    .format = kHAPCharacteristicFormat_UInt8,
    .iid = kIID_BatteryChargingState,
    .characteristicType = &kHAPCharacteristicType_ChargingState,
    .debugDescription = kHAPCharacteristicDebugDescription_ChargingState,
    .manufacturerDescription = NULL,
    .properties = PROPERTIES(true, false, true, false, false, false),
    .units = kHAPCharacteristicUnits_None,
    .constraints = { .minimumValue = 0,
                     .maximumValue = 2,
                     .stepValue = 1,
                     .validValues = NULL,
                     .validValuesRanges = NULL },
    .callbacks = { .handleRead = homekit_handle_charging_state_read, .handleWrite = NULL }
};

const HAPUInt8Characteristic homekit_battery_status_low_characteristic = {
    .format = kHAPCharacteristicFormat_UInt8,
    .iid = kIID_BatteryStatusLowBattery,
    .characteristicType = &kHAPCharacteristicType_StatusLowBattery,
    .debugDescription = kHAPCharacteristicDebugDescription_StatusLowBattery,
    .manufacturerDescription = NULL,
    .properties = PROPERTIES(true, false, true, false, false, false),
    .units = kHAPCharacteristicUnits_None,
    .constraints = { .minimumValue = 0,
                     .maximumValue = 1,
                     .stepValue = 1,
                     .validValues = NULL,
                     .validValuesRanges = NULL },
    .callbacks = { .handleRead = homekit_handle_status_low_battery_read, .handleWrite = NULL }
};

const HAPService homekit_battery_service = {
    .iid = kIID_Battery,
    .serviceType = &kHAPServiceType_BatteryService,
    .debugDescription = kHAPServiceDebugDescription_BatteryService,
    .name = NULL,
    .properties = { .primaryService = false, .hidden = false, .ble = { .supportsConfiguration = false } },
    .linkedServices = NULL,
    .characteristics = (const HAPCharacteristic* const[]) { &batteryServiceSignature,
                                                            &homekit_battery_level_characteristic,
                                                            &batteryChargingState,
                                                            &homekit_battery_status_low_characteristic,
                                                            NULL }
};
