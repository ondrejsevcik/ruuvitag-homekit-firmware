/**
 * @file HAPPlatform+Init.h
 * @brief Platform object definitions for the RuuviTag HomeKit port.
 */
#ifndef HAP_PLATFORM_INIT_H
#define HAP_PLATFORM_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "HAPPlatform.h"
#include "HAPPlatformBLEPeripheralManager+Init.h"
#include "HAPPlatformKeyValueStore+Init.h"

/** Static accessory setup information. Has no state. */
struct HAPPlatformAccessorySetup {
    uint8_t unused;
};

/** Placeholders. These features are not used, but the ADK headers declare the types. */
struct HAPPlatformAccessorySetupDisplay {
    uint8_t unused;
};
struct HAPPlatformAccessorySetupNFC {
    uint8_t unused;
};
struct HAPPlatformMFiHWAuth {
    uint8_t unused;
};
struct HAPPlatformMFiTokenAuth {
    uint8_t unused;
};

void HAPPlatformAccessorySetupCreate(HAPPlatformAccessorySetupRef accessorySetup);

/**
 * Initializes the timer service. Must be called once before the accessory server starts.
 *
 * @return kHAPError_None on success, kHAPError_Unknown if no hardware timer is available.
 */
HAP_RESULT_USE_CHECK
HAPError HAPPlatformTimerInit(void);

#ifdef __cplusplus
}
#endif

#endif
