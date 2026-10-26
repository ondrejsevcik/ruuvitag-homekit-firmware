/**
 * @file HAPPlatformTimer.c
 * @brief HomeKit ADK timers multiplexed on one Ruuvi (app_timer) single-shot timer.
 *
 * Timer callbacks run in main context through the Ruuvi scheduler, in deadline
 * order, and in registration order for equal deadlines.
 */

#include "HAPPlatform.h"
#include "HAPPlatform+Init.h"

#include "ruuvi_interface_scheduler.h"
#include "ruuvi_interface_timer.h"

static const HAPLogObject logObject = { .subsystem = kHAPPlatform_LogSubsystem, .category = "Timer" };

#ifndef HAP_PLATFORM_TIMER_MAX_TIMERS
#define HAP_PLATFORM_TIMER_MAX_TIMERS 16
#endif

/** app_timer runs on a 24 bit RTC at 32768 Hz. Re-evaluate at least this often. */
#define kMaxTimeoutMs ((uint32_t) 60000)

typedef struct {
    HAPTime deadline;
    uint32_t sequence; /**< Registration order, for stable ordering. */
    HAPPlatformTimerCallback callback;
    void* _Nullable context;
    uint8_t generation;
    bool active;
} Timer;

static Timer timers[HAP_PLATFORM_TIMER_MAX_TIMERS];
static uint32_t nextSequence;
static ri_timer_id_t hardwareTimer;
static bool isInitialized;
static volatile bool isProcessingScheduled;
static bool isProcessing;

static HAPPlatformTimerRef MakeRef(size_t index) {
    return (HAPPlatformTimerRef)((index + 1U) | ((uintptr_t) timers[index].generation << 8U));
}

static Timer* _Nullable FromRef(HAPPlatformTimerRef ref) {
    size_t index = (size_t)(ref & 0xFFU);
    if (index == 0 || index > HAPArrayCount(timers)) {
        return NULL;
    }
    Timer* t = &timers[index - 1U];
    if (!t->active || t->generation != (uint8_t)(ref >> 8U)) {
        return NULL;
    }
    return t;
}

static Timer* _Nullable GetEarliest(void) {
    Timer* earliest = NULL;
    for (size_t i = 0; i < HAPArrayCount(timers); i++) {
        Timer* t = &timers[i];
        if (!t->active) {
            continue;
        }
        if (!earliest || t->deadline < earliest->deadline ||
            (t->deadline == earliest->deadline && t->sequence < earliest->sequence)) {
            earliest = t;
        }
    }
    return earliest;
}

static void ProcessExpired(void* _Nullable data, uint16_t size);

static void ScheduleProcessing(void) {
    if (!isProcessingScheduled) {
        isProcessingScheduled = true;
        if (ri_scheduler_event_put(NULL, 0, ProcessExpired) != RD_SUCCESS) {
            // Queue full. The hardware timer retries shortly.
            isProcessingScheduled = false;
            (void) ri_timer_stop(hardwareTimer);
            (void) ri_timer_start(hardwareTimer, 10, NULL);
        }
    }
}

static void HandleHardwareTimeout(void* const context HAP_UNUSED) {
    // Interrupt context.
    ScheduleProcessing();
}

static void Rearm(void) {
    (void) ri_timer_stop(hardwareTimer);
    Timer* earliest = GetEarliest();
    if (!earliest) {
        return;
    }
    HAPTime now = HAPPlatformClockGetCurrent();
    if (earliest->deadline <= now) {
        ScheduleProcessing();
        return;
    }
    HAPTime delta = earliest->deadline - now;
    uint32_t timeoutMs = delta > kMaxTimeoutMs ? kMaxTimeoutMs : (uint32_t) delta;
    if (timeoutMs < 1) {
        timeoutMs = 1;
    }
    rd_status_t e = ri_timer_start(hardwareTimer, timeoutMs, NULL);
    if (e != RD_SUCCESS) {
        HAPLogError(&logObject, "ri_timer_start failed: 0x%08lX.", (unsigned long) e);
    }
}

static void ProcessExpired(void* _Nullable data HAP_UNUSED, uint16_t size HAP_UNUSED) {
    isProcessingScheduled = false;
    isProcessing = true;
    for (;;) {
        Timer* t = GetEarliest();
        if (!t || t->deadline > HAPPlatformClockGetCurrent()) {
            break;
        }
        // Deregister before invoking: the callback may register new timers.
        HAPPlatformTimerRef ref = MakeRef((size_t)(t - timers));
        HAPPlatformTimerCallback callback = t->callback;
        void* _Nullable context = t->context;
        t->active = false;
        callback(ref, context);
    }
    isProcessing = false;
    Rearm();
}

HAPError HAPPlatformTimerInit(void) {
    if (isInitialized) {
        return kHAPError_None;
    }
    rd_status_t e = ri_timer_create(&hardwareTimer, RI_TIMER_MODE_SINGLE_SHOT, HandleHardwareTimeout);
    if (e != RD_SUCCESS) {
        HAPLogError(&logObject, "ri_timer_create failed: 0x%08lX.", (unsigned long) e);
        return kHAPError_Unknown;
    }
    isInitialized = true;
    return kHAPError_None;
}

HAPError HAPPlatformTimerRegister(
        HAPPlatformTimerRef* timer,
        HAPTime deadline,
        HAPPlatformTimerCallback callback,
        void* _Nullable context) {
    HAPPrecondition(timer);
    HAPPrecondition(callback);
    HAPPrecondition(isInitialized);

    for (size_t i = 0; i < HAPArrayCount(timers); i++) {
        Timer* t = &timers[i];
        if (t->active) {
            continue;
        }
        t->active = true;
        t->generation++;
        t->deadline = deadline;
        t->sequence = nextSequence++;
        t->callback = callback;
        t->context = context;
        *timer = MakeRef(i);
        if (!isProcessing) {
            Rearm();
        }
        return kHAPError_None;
    }
    HAPLogError(&logObject, "No free timers. Increase HAP_PLATFORM_TIMER_MAX_TIMERS.");
    return kHAPError_OutOfResources;
}

void HAPPlatformTimerDeregister(HAPPlatformTimerRef timer) {
    Timer* t = FromRef(timer);
    HAPPrecondition(t);
    t->active = false;
    if (!isProcessing) {
        Rearm();
    }
}
