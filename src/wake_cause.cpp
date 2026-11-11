#include "wake_cause.h"

#include "structs.h"
#include "od_log.h"

#ifdef TARGET_ESP32
#include <esp_sleep.h>
#include <esp_system.h>
#endif

static uint8_t s_wakeCause = OD_WAKE_CAUSE_UNKNOWN;

#ifdef TARGET_ESP32
static uint8_t causeFromResetReason(esp_reset_reason_t r) {
    switch (r) {
        // Includes an EN-pin reset on every ESP32 variant this firmware targets:
        // the ROM reports both as POWERON, so they cannot be told apart here.
        case ESP_RST_POWERON:   return OD_WAKE_CAUSE_POWER_ON;
        case ESP_RST_EXT:       return OD_WAKE_CAUSE_RESET_PIN;
        case ESP_RST_SW:        return OD_WAKE_CAUSE_SOFTWARE;
        case ESP_RST_PANIC:     return OD_WAKE_CAUSE_PANIC;
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:       return OD_WAKE_CAUSE_WATCHDOG;
        case ESP_RST_BROWNOUT:  return OD_WAKE_CAUSE_BROWNOUT;
        default:                return OD_WAKE_CAUSE_OTHER;
    }
}

void wakeCauseDetectEsp32(int sleepWakeCause, bool rtcWake) {
    switch ((esp_sleep_wakeup_cause_t)sleepWakeCause) {
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            // Not a deep-sleep wake: an RTC power-off wake looks like a cold
            // power-on to the ESP32, so the RTC's own flag decides first.
            s_wakeCause = rtcWake ? (uint8_t)OD_WAKE_CAUSE_RTC
                                  : causeFromResetReason(esp_reset_reason());
            break;
        case ESP_SLEEP_WAKEUP_TIMER:
            s_wakeCause = OD_WAKE_CAUSE_TIMER;
            break;
        case ESP_SLEEP_WAKEUP_EXT0:
        case ESP_SLEEP_WAKEUP_EXT1:
        case ESP_SLEEP_WAKEUP_GPIO:
            s_wakeCause = OD_WAKE_CAUSE_BUTTON;
            break;
        default:
            s_wakeCause = OD_WAKE_CAUSE_OTHER;
            break;
    }
    od_log_info("Wake cause: %s (%u)", wakeCauseName(s_wakeCause), (unsigned)s_wakeCause);
}
#endif

uint8_t wakeCauseGet(void) {
    return s_wakeCause;
}

const char* wakeCauseName(uint8_t cause) {
    switch (cause) {
        case OD_WAKE_CAUSE_UNKNOWN:   return "unknown";
        case OD_WAKE_CAUSE_POWER_ON:  return "power_on";
        case OD_WAKE_CAUSE_RESET_PIN: return "reset";
        case OD_WAKE_CAUSE_RTC:       return "rtc";
        case OD_WAKE_CAUSE_TIMER:     return "timer";
        case OD_WAKE_CAUSE_BUTTON:    return "button";
        case OD_WAKE_CAUSE_SOFTWARE:  return "software";
        case OD_WAKE_CAUSE_PANIC:     return "panic";
        case OD_WAKE_CAUSE_WATCHDOG:  return "watchdog";
        case OD_WAKE_CAUSE_BROWNOUT:  return "brownout";
        case OD_WAKE_CAUSE_USB_POWER: return "usb";
        default:                      return "other";
    }
}

void wakeCauseApplyToMsd(uint8_t* dynamic, uint8_t len, uint8_t slot) {
    if (slot == 0 || slot > len) return;
    dynamic[slot - 1] = (uint8_t)(s_wakeCause & OD_MSD_WAKE_INFO_CAUSE_MASK);
}
