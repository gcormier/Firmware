#ifndef WAKE_CAUSE_H
#define WAKE_CAUSE_H

#include <stdint.h>

// Why this boot happened (WakeCause in opendisplay_structs.h), published as the
// MsdWakeInfo byte at PowerOption.wake_info_msd_slot. Decided once in setup()
// and held until the next boot, so every advertisement of a wake window carries
// the same value. Costs one byte of RAM and no I2C/flash traffic.

#ifdef TARGET_ESP32
// Classify an ESP32 boot. sleepWakeCause is esp_sleep_get_wakeup_cause() (as
// int, to keep esp_sleep.h out of this header); rtcWake is rtcConsumeWake().
// Reads esp_reset_reason() only when neither of those named a cause.
void wakeCauseDetectEsp32(int sleepWakeCause, bool rtcWake);
#endif

// The WakeCause value for this boot (OD_WAKE_CAUSE_UNKNOWN until detected).
uint8_t wakeCauseGet(void);

// Lower-case name for logs ("rtc", "power_on", ...).
const char* wakeCauseName(uint8_t cause);

// Write the MsdWakeInfo byte into the MSD dynamic area when `slot` (the
// config's PowerOption.wake_info_msd_slot, 1-based) names a byte of it;
// 0 or > len = not published.
void wakeCauseApplyToMsd(uint8_t* dynamic, uint8_t len, uint8_t slot);

#endif
