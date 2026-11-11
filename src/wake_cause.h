#ifndef WAKE_CAUSE_H
#define WAKE_CAUSE_H

#include <stdint.h>

// Why this boot happened (WakeCause in opendisplay_structs.h) and when the
// device plans to sleep, published as the 3-byte MsdWakeInfo block at
// PowerOption.wake_info_msd_slot. The cause is decided once in setup() and held
// until the next boot, so every advertisement of a wake window carries it.
// Costs one byte of RAM and no I2C/flash/heap.

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

// Sleep-duration code for MsdWakeInfo byte 2 (OD_MSD_SLEEP_* in
// opendisplay_structs.h). 0 s = no timed sleep. Rounds up.
uint8_t wakeInfoSleepCode(uint32_t seconds);

// Seconds-until-sleep byte for MsdWakeInfo byte 1 from a millisecond countdown
// (UINT32_MAX = not going to sleep). Rounds up, so 0 only once it is due.
uint8_t wakeInfoSleepInByte(uint32_t msUntilSleep);

// Write the MsdWakeInfo block into the MSD dynamic area when `slot` (the
// config's PowerOption.wake_info_msd_slot, 1-based) names a block that fits;
// 0 or a block running past `len` = not published.
void wakeInfoApplyToMsd(uint8_t* dynamic, uint8_t len, uint8_t slot,
                        uint32_t msUntilSleep, uint32_t sleepSeconds);

#endif
