# Proposal: advertise the wake/boot cause in the MSD

Status: draft, implemented on a fork branch (`gcormier/Firmware`,
`greg/m5paper-wake-reason-cba5e2`) for evaluation. Not yet raised in
`opendisplay-protocol`. This document is the intended basis for that issue.

## Problem

A battery OpenDisplay tag spends almost all its life asleep and is reachable
only during a short advertising window after each wake. A passive host (Home
Assistant) can already see *that* the device is up, and partly *how*: status
bit1 (`OD_MSD_STATUS_REBOOT_FLAG`) is set on a real reboot and cleared on a
timer/RTC wake. It cannot tell:

- an RTC power-off wake from a deep-sleep timer wake;
- a button wake from either of those;
- a cold power-on (on the M5Paper: the rocker held for 2-3 s) from a crash,
  watchdog or brownout reset, all of which set the reboot flag the same way.

Users want automations like "the M5Paper was switched on by hand" or "the tag
browned out" without connecting to the device.

## Constraints

- `MsdAdvertisement` is a fixed 16-byte record; changing its size or any
  existing offset is a MAJOR bump and breaks every host.
- The `status` byte is full (bit0 voltage MSB, bit1 reboot, bit2 connection
  requested, bit3 encryption, bits 4-7 loop counter).
- The 11-byte `dynamic` area is the established extension point: every
  publisher in it (buttons, touch, SHT40/AHT20, BQ27220) owns a slot chosen by
  its config packet, and hosts already read the config to know which byte is
  what.

## Design

### Config: `PowerOption.wake_info_msd_slot` (carved from `reserved[0]`)

| offset | field | value |
|---|---|---|
| 26 | `wake_info_msd_slot` | **1-based** slot: 0 = not published, N (1-11) = `dynamic[N-1]`; >11 treated as 0 |
| 27-29 | `reserved[3]` | must be 0 |

Why PowerOption: wake cause is a property of the power/sleep subsystem, it is
a required singleton (exactly one slot per device), and it applies to every
board, not only those with an RTC sensor block.

Why one-based: the byte used to be `reserved`, must-be-0. A zero-based index
would make every existing config suddenly publish into `dynamic[0]` (where most
configs put their buttons). One-based keeps 0 = off, so the carve is a MINOR,
backward-compatible change (structs 2.1 -> 2.2).

### Advertisement: the `MsdWakeInfo` byte

Published at `dynamic[wake_info_msd_slot - 1]`:

| bits | meaning |
|---|---|
| 0-3 | `WakeCause` |
| 4-7 | reserved, 0 (see "Future" below) |

`WakeCause` (`enum WakeCause` in `opendisplay_structs.h`):

| value | name | ESP32 source |
|---|---|---|
| 0 | `unknown` | not determined (nRF today; slot configured but target does not report) |
| 1 | `power_on` | `esp_reset_reason() == ESP_RST_POWERON`, no RTC flag. M5Paper on battery: rocker held. **Also an EN/reset-pin reset** (the ESP32 ROM reports both as POWERON) |
| 2 | `reset` | `ESP_RST_EXT` (only on SoCs that distinguish it) |
| 3 | `rtc` | PCF8563/BM8563 TF/AF flag set at boot (`rtcConsumeWake()`), i.e. an RTC power-off wake |
| 4 | `timer` | `ESP_SLEEP_WAKEUP_TIMER` |
| 5 | `button` | `ESP_SLEEP_WAKEUP_EXT0/EXT1/GPIO` |
| 6 | `software` | `ESP_RST_SW` (config write, OTA, reboot command) |
| 7 | `panic` | `ESP_RST_PANIC` |
| 8 | `watchdog` | `ESP_RST_INT_WDT/TASK_WDT/WDT` |
| 9 | `brownout` | `ESP_RST_BROWNOUT` |
| 10 | `usb` | reserved: power-on with USB/external power present (not emitted yet; no cheap generic VBUS sense) |
| 11-14 | reserved | hosts map to `other` |
| 15 | `other` | any other reset reason or wake source |

The value is decided once in `setup()` and never changes until the next boot,
so every advertisement of a wake window carries it, and `CMD_READ_MSD` returns
the same byte. Firmware overlays it onto the advertised copy rather than writing
`dynamicreturndata`, so it does not count as "activity" for the sleep timer.

Cost: one byte of RAM, one `esp_reset_reason()` call per boot, a byte store per
MSD rebuild. No I2C, flash or heap.

### Host side

1. Read `PowerOption.wake_info_msd_slot` from the device config.
2. If non-zero, decode `dynamic[slot-1] & 0x0F` as `WakeCause`.
3. Expose it as a state ("last wake") and fire an event when a **new wake** is
   observed. Because the cause is constant within a wake and often repeats
   (rtc, rtc, rtc ...), a new wake is: the first advertisement after the device
   was unseen for longer than a gap threshold, OR a change of cause, OR a
   reboot-flag False -> True edge.

### Backward compatibility

- Old firmware + new host: slot reads 0 from the config, nothing decoded.
- New firmware + old host: the slot is never set by an old config writer, so
  nothing is published; if it is set, an old host ignores an unassigned dynamic
  byte (button trackers in py-opendisplay only look at configured button bytes).
- Old config tools that round-trip `reserved` as raw bytes keep the value.
- Overlap with another publisher's slot is a config error, the same as two
  overlapping sensors today; config tooling should warn.

## Future: wake window timing (out of scope here)

The upper nibble and a second slot leave room for the "going to sleep" side
without a new layout:

- **bit 4 `sleep_imminent`**: set for the last few seconds of the window (or
  on the advertisement emitted just before sleep). Rejected as the *only*
  signal because passive scanners miss short bursts, but cheap as a hint.
- **bits 5-7**: a coarse "next sleep in" bucket (e.g. 0 = n/a, 1 = <5 s,
  2 = <10 s, 3 = <30 s, 4 = <60 s, ...), valid during the window.
- **`PowerOption.wake_timing_msd_slot`** (carve `reserved[1]`, one-based again):
  a 2-byte little-endian slot, `sleep_in_s` (u8, seconds until this window
  closes) + `sleep_for_min` (u8, scheduled sleep in minutes; 0 = not sleeping,
  0xFF = >= 255 min). With the advertisement timestamp a host computes "next
  wake at" exactly, which is what an HA timestamp sensor needs.

Both are additive (MINOR) and decodable by a host that already knows this
proposal.

## Open questions for the maintainers

1. PowerOption carve vs. a field in SystemConfig: PowerOption was chosen
   because sleep lives there; any objection?
2. Is `reset` worth a value of its own when the main ESP32 targets cannot
   report it? Kept so SoCs/nRF that can (nRF `RESETREAS.RESETPIN`) have a home.
3. nRF52840: `RESETREAS` gives pin reset, watchdog, soft reset, lockup and
   wake-from-System-OFF (GPIO/NFC). A follow-up can fill the same enum.
