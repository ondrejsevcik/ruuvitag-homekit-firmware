/**
 * @file HAPPlatform.c
 * @brief Small HomeKit ADK platform services for the RuuviTag (nRF52832 + S132).
 *
 * Clock, randomness, logging, abort and static accessory setup information.
 * Features this accessory does not have (display, programmable NFC, Apple
 * authentication coprocessor, software tokens) are stubbed out.
 */

#include "HAPPlatform.h"
#include "HAPPlatform+Init.h"

#include "nrf_nvic.h"
#include "nrf_sdm.h"
#include "nrf_soc.h"
#include "ruuvi_interface_log.h"
#include "ruuvi_interface_rtc.h"

#include "homekit_setup_info.h"

#ifndef HOMEKIT_PLATFORM_VERSION
#define HOMEKIT_PLATFORM_VERSION "1"
#endif

//----------------------------------------------------------------------------------------------------------------------
// Platform identification.

uint32_t HAPPlatformGetCompatibilityVersion(void) {
    return HAP_PLATFORM_COMPATIBILITY_VERSION;
}

const char* HAPPlatformGetIdentification(void) {
    return "RuuviTag nRF52832 S132";
}

const char* HAPPlatformGetVersion(void) {
    return HOMEKIT_PLATFORM_VERSION;
}

const char* HAPPlatformGetBuild(void) {
    return __DATE__ " " __TIME__;
}

//----------------------------------------------------------------------------------------------------------------------
// Abort.

void HAPPlatformAbort(void) {
    uint8_t enabled = 0;
    (void) sd_softdevice_is_enabled(&enabled);
    if (enabled) {
        (void) sd_nvic_SystemReset();
    }
    NVIC_SystemReset();
    for (;;) {
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Clock.

HAPTime HAPPlatformClockGetCurrent(void) {
    // RTC2 based millisecond counter, started during sensor initialization.
    uint64_t now = ri_rtc_millis();
    HAPAssert(now != RD_UINT64_INVALID);
    return (HAPTime) now;
}

//----------------------------------------------------------------------------------------------------------------------
// Random numbers.

void HAPPlatformRandomNumberFill(void* bytes, size_t numBytes) {
    HAPPrecondition(!numBytes || bytes);

    uint8_t* b = bytes;
    while (numBytes) {
        uint8_t available = 0;
        uint32_t e = sd_rand_application_bytes_available_get(&available);
        HAPAssert(!e);
        if (!available) {
            // The RNG pool refills within microseconds.
            continue;
        }
        uint8_t n = (uint8_t)(numBytes < available ? numBytes : available);
        e = sd_rand_application_vector_get(b, n);
        if (e == NRF_SUCCESS) {
            b += n;
            numBytes -= n;
        }
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Logging.

HAPPlatformLogEnabledTypes HAPPlatformLogGetEnabledTypes(const HAPLogObject* log HAP_UNUSED) {
    switch (HAP_LOG_LEVEL) {
        case 0:
            return kHAPPlatformLogEnabledTypes_None;
        case 1:
            return kHAPPlatformLogEnabledTypes_Default;
        case 2:
            return kHAPPlatformLogEnabledTypes_Info;
        default:
            return kHAPPlatformLogEnabledTypes_Debug;
    }
}

void HAPPlatformLogCapture(
        const HAPLogObject* log,
        HAPLogType type,
        const char* message,
        const void* _Nullable bufferBytes,
        size_t numBufferBytes) {
    HAPPrecondition(log);
    HAPPrecondition(message);

    ri_log_severity_t severity;
    switch (type) {
        case kHAPLogType_Debug:
            severity = RI_LOG_LEVEL_DEBUG;
            break;
        case kHAPLogType_Info:
        case kHAPLogType_Default:
            severity = RI_LOG_LEVEL_INFO;
            break;
        default:
            severity = RI_LOG_LEVEL_ERROR;
            break;
    }
    if (log->category) {
        ri_log(severity, log->category);
        ri_log(severity, ": ");
    }
    ri_log(severity, message);
    ri_log(severity, "\r\n");
    if (bufferBytes && numBufferBytes) {
        ri_log_hex(severity, bufferBytes, numBufferBytes);
        ri_log(severity, "\r\n");
    }
}

//----------------------------------------------------------------------------------------------------------------------
// Accessory setup. Salt and verifier are provisioned at build time (tools/homekit_provision.py),
// the setup code itself is not stored on the device.

void HAPPlatformAccessorySetupCreate(HAPPlatformAccessorySetupRef accessorySetup) {
    HAPPrecondition(accessorySetup);
    HAPRawBufferZero(accessorySetup, sizeof *accessorySetup);
}

void HAPPlatformAccessorySetupLoadSetupInfo(HAPPlatformAccessorySetupRef accessorySetup, HAPSetupInfo* setupInfo) {
    HAPPrecondition(accessorySetup);
    HAPPrecondition(setupInfo);

    static const uint8_t salt[] = HOMEKIT_SETUP_SALT;
    static const uint8_t verifier[] = HOMEKIT_SETUP_VERIFIER;
    HAP_STATIC_ASSERT(sizeof salt == sizeof setupInfo->salt, salt);
    HAP_STATIC_ASSERT(sizeof verifier == sizeof setupInfo->verifier, verifier);
    HAPRawBufferCopyBytes(setupInfo->salt, salt, sizeof setupInfo->salt);
    HAPRawBufferCopyBytes(setupInfo->verifier, verifier, sizeof setupInfo->verifier);
}

void HAPPlatformAccessorySetupLoadSetupCode(
        HAPPlatformAccessorySetupRef accessorySetup HAP_UNUSED,
        HAPSetupCode* setupCode HAP_UNUSED) {
    // Only needed for displays and programmable NFC, which this accessory does not use.
    HAPLogFault(&kHAPLog_Default, "Setup code is not stored on the device.");
    HAPFatalError();
}

void HAPPlatformAccessorySetupLoadSetupID(
        HAPPlatformAccessorySetupRef accessorySetup,
        bool* valid,
        HAPSetupID* setupID) {
    HAPPrecondition(accessorySetup);
    HAPPrecondition(valid);
    HAPPrecondition(setupID);

    static const char id[] = HOMEKIT_SETUP_ID;
    HAP_STATIC_ASSERT(sizeof id == sizeof setupID->stringValue, setupID);
    HAPRawBufferCopyBytes(setupID->stringValue, id, sizeof setupID->stringValue);
    *valid = true;
}

HAPPlatformAccessorySetupCapabilities
        HAPPlatformAccessorySetupGetCapabilities(HAPPlatformAccessorySetupRef accessorySetup HAP_UNUSED) {
    return (HAPPlatformAccessorySetupCapabilities) { .supportsDisplay = false, .supportsProgrammableNFC = false };
}

void HAPPlatformAccessorySetupUpdateSetupPayload(
        HAPPlatformAccessorySetupRef accessorySetup HAP_UNUSED,
        const HAPSetupPayload* _Nullable setupPayload HAP_UNUSED,
        const HAPSetupCode* _Nullable setupCode HAP_UNUSED) {
}

void HAPPlatformAccessorySetupDisplayUpdateSetupPayload(
        HAPPlatformAccessorySetupDisplayRef setupDisplay HAP_UNUSED,
        const HAPSetupPayload* _Nullable setupPayload HAP_UNUSED,
        const HAPSetupCode* _Nullable setupCode HAP_UNUSED) {
}

void HAPPlatformAccessorySetupDisplayHandleStartPairing(HAPPlatformAccessorySetupDisplayRef setupDisplay HAP_UNUSED) {
}

void HAPPlatformAccessorySetupDisplayHandleStopPairing(HAPPlatformAccessorySetupDisplayRef setupDisplay HAP_UNUSED) {
}

void HAPPlatformAccessorySetupNFCUpdateSetupPayload(
        HAPPlatformAccessorySetupNFCRef setupNFC HAP_UNUSED,
        const HAPSetupPayload* setupPayload HAP_UNUSED,
        bool isPairable HAP_UNUSED) {
}

//----------------------------------------------------------------------------------------------------------------------
// Apple authentication: not available. The platform never hands these objects to the ADK.

bool HAPPlatformMFiHWAuthIsPoweredOn(HAPPlatformMFiHWAuthRef mfiHWAuth HAP_UNUSED) {
    return false;
}

HAPError HAPPlatformMFiHWAuthPowerOn(HAPPlatformMFiHWAuthRef mfiHWAuth HAP_UNUSED) {
    return kHAPError_Unknown;
}

void HAPPlatformMFiHWAuthPowerOff(HAPPlatformMFiHWAuthRef mfiHWAuth HAP_UNUSED) {
}

HAPError HAPPlatformMFiHWAuthWrite(
        HAPPlatformMFiHWAuthRef mfiHWAuth HAP_UNUSED,
        const void* bytes HAP_UNUSED,
        size_t numBytes HAP_UNUSED) {
    return kHAPError_Unknown;
}

HAPError HAPPlatformMFiHWAuthRead(
        HAPPlatformMFiHWAuthRef mfiHWAuth HAP_UNUSED,
        uint8_t registerAddress HAP_UNUSED,
        void* bytes HAP_UNUSED,
        size_t numBytes HAP_UNUSED) {
    return kHAPError_Unknown;
}

HAPError HAPPlatformMFiTokenAuthLoad(
        HAPPlatformMFiTokenAuthRef mfiTokenAuth HAP_UNUSED,
        bool* valid,
        HAPPlatformMFiTokenAuthUUID* _Nullable mfiTokenUUID HAP_UNUSED,
        void* _Nullable mfiTokenBytes HAP_UNUSED,
        size_t maxMFiTokenBytes HAP_UNUSED,
        size_t* _Nullable numMFiTokenBytes HAP_UNUSED) {
    HAPPrecondition(valid);
    *valid = false;
    return kHAPError_None;
}

HAPError HAPPlatformMFiTokenAuthUpdate(
        HAPPlatformMFiTokenAuthRef mfiTokenAuth HAP_UNUSED,
        const void* mfiTokenBytes HAP_UNUSED,
        size_t numMFiTokenBytes HAP_UNUSED) {
    return kHAPError_Unknown;
}
