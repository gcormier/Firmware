# Proposal: advertise wake cause and sleep timing in the MSD

Status: **provisional**. Discussion: opendisplay-protocol#15 (placement, the
sleep-duration encoding, the cause enum, and whether the countdown pauses while
connected are open questions there). Implemented for evaluation on the fork
branch `gcormier/Firmware` `feat/wake-cause-msd`; host side on py-opendisplay
`feat/wake-cause-msd` and Home_Assistant_Integration `feat/wake-cause`. Any
answer on the issue may change the encoding below.

## Problem

A battery OpenDisplay tag spends almost all its life asleep and is reachable
only during a short advertising window after each wake. A passive host (Home
Assistant) can see *that* the device is up, but not:

- **why** it woke: an RTC power-off wake vs a deep-sleep timer wake vs a button
  wake vs a cold power-on (on the M5Paper: the rocker held 2-3 s) vs a crash,
  watchdog or brownout reset. Status bit1 (reboot flag) only splits
  "scheduled" (clear) from "some reboot" (set), and the first BLE connection
  clears it;
- **when it will sleep** and **for how long**, so it cannot say "asleep now,
  next wake at 14:32" without guessing.

A last-moment "going to sleep" advertisement was rejected: passive scanners
miss short bursts. The timing has to be in every advertisement of the window.

## Constraints

- `MsdAdvertisement` is a fixed 16-byte record; changing its size or an
  existing offset is a MAJOR bump that breaks every host.
- The `status` byte is full.
- The 11-byte `dynamic` area is the established extension point: buttons,
  touch, SHT40/AHT20 and BQ27220 each own a config-chosen slot, and hosts read
  the config to know which byte is what.

## Design

### Config: `PowerOption.wake_info_msd_slot` (carved from `reserved[0]`)

| offset | field | value |
|---|---|---|
| 26 | `wake_info_msd_slot` | **1-based** start of a 3-byte block: 0 = off, N (1-9) = `dynamic[N-1..N+1]`; >9 = off |
| 27-29 | `reserved[3]` | must be 0 |

PowerOption because wake/sleep is the power subsystem's, it is a required
singleton, and it applies to every board, not only those with an RTC.
One-based because the byte used to be must-be-0: a zero-based index would make
every existing config publish into `dynamic[0]` (usually buttons). With 0 =
off the carve is a MINOR, backward-compatible change.

### Advertisement: the `MsdWakeInfo` block (3 bytes)

| byte | meaning |
|---|---|
| 0 | bits 0-3 `WakeCause`; bits 4-7 reserved, 0 |
| 1 | seconds until sleep, counted from this advertisement: 0-253 exact, 254 = "254 s or more", 0xFF = not going to sleep (wired, deep sleep off) or unknown |
| 2 | planned sleep duration code (below); 0 = no timed sleep |

**Sleep-duration code** (one byte, rounded up by the encoder):

| code | duration |
|---|---|
| 0 | no timed sleep follows this window |
| 1-120 | code x 15 s (15 s ... 30 min) |
| 121-254 | 30 min + (code - 120) x 8 min (... 18 h 22 min) |
| 255 | longer than that |

That covers the full `u16 deep_sleep_time_seconds` (18 h 12 min) in one byte.
Typical values are exact: 120 s = 8, 900 s = 60, 1 h = 124. Above 30 min the
error is under 8 min, which is under the PCF8563's own minute-resolution wake
for sleeps over 255 s anyway. If the maintainers prefer exact seconds, a
2-byte little-endian `u16` (the issue's "1-2 bytes") is the alternative; it
costs one more dynamic byte.

`WakeCause` (`enum WakeCause` in `opendisplay_structs.h`):

| value | name | ESP32 source |
|---|---|---|
| 0 | `unknown` | not determined (nRF today) |
| 1 | `power_on` | `ESP_RST_POWERON`, no RTC flag. M5Paper on battery: the rocker. **Also an EN/reset-pin reset**: the ESP32 ROM reports both as POWERON |
| 2 | `reset` | `ESP_RST_EXT` (only SoCs that distinguish it) |
| 3 | `rtc` | PCF8563/BM8563 TF/AF flag set at boot (`rtcConsumeWake()`) |
| 4 | `timer` | `ESP_SLEEP_WAKEUP_TIMER` |
| 5 | `button` | `ESP_SLEEP_WAKEUP_EXT0/EXT1/GPIO` |
| 6 | `software` | `ESP_RST_SW` (config write, OTA, reboot command) |
| 7 | `panic` | `ESP_RST_PANIC` |
| 8 | `watchdog` | `ESP_RST_INT_WDT/TASK_WDT/WDT` |
| 9 | `brownout` | `ESP_RST_BROWNOUT` |
| 10 | `usb` | reserved: power-on with USB present (not emitted; no cheap generic VBUS sense) |
| 11-14 | reserved | hosts map to `other` |
| 15 | `other` | anything else |

### Firmware behaviour

- The cause is decided once in `setup()` and held until the next boot.
- Seconds-until-sleep mirrors the firmware's own sleep decision: sleep happens
  when the quiet time since the last activity reaches `sleep_timeout_ms` and
  the min-wake hold (cold boot / button wake) has run out. Any activity
  restarts it. A connection counts as activity on every loop pass, so **the
  countdown is held at the full window while connected** and restarts from the
  full window when the client disconnects. (The device does not advertise while
  connected, so hosts see the reset value on the first advertisement after.)
- The block is overlaid on the advertised copy, never on `dynamicreturndata`,
  so the changing countdown never counts as activity.
- Cost: one byte of RAM. While a countdown is running the MSD is rebuilt and
  re-advertised once a second (the ESP32 transport restarts advertising on
  each change: one ~50 ms stop/start per second, plus a battery ADC read).
  Nothing is refreshed when the slot is off or the device will not sleep.

#### The 0x0053 override

`CMD_DEEP_SLEEP` (0x0053) with a duration payload sleeps **immediately**, for
that one cycle. There is no advertising window between the command and the
sleep, so byte 2 can never carry the override; it always reports the configured
`deep_sleep_time_seconds`. The host that sent 0x0053 knows the override. If a
deferred "sleep in N s for M s" command is ever added, its M belongs in byte 2.

### Host side

1. Read `PowerOption.wake_info_msd_slot` from the device config.
2. If non-zero, decode `dynamic[slot-1 .. slot+1]`.
3. *Last wake* = cause. A **new wake** is the first advertisement after more
   than ~60 s of silence, a cause change, or a reboot-flag False -> True edge
   (the cause is constant within a wake and often repeats: rtc, rtc, ...).
4. *Next wake* = advertisement time + seconds-until-sleep + sleep duration.
5. *Asleep* = the announced sleep time (+ a few seconds) passed with no newer
   advertisement. While the host itself holds the BLE link it must not declare
   the device asleep (it is connected, so silent).

**Fallback (level 1, no wake-info block)**: for a deep-sleeping device the
reboot flag on the *first* advertisement of a wake gives `scheduled` (clear:
timer or RTC) vs `restart` (set: power-on, reset, crash, or a live config
write). Next wake = last seen + `deep_sleep_time_seconds`; asleep = ~30 s of
silence.

### Backward compatibility

- Old firmware + new host: the slot reads 0, host uses the level-1 fallback.
- New firmware + old host: an old config writer never sets the slot; if set,
  old hosts ignore unassigned dynamic bytes.
- Old config tools that round-trip `reserved` as raw bytes keep the value.
- Overlap with another publisher's bytes is a config error, the same as two
  overlapping sensors today; config tooling should warn.

## Room left

- Byte 0 bits 4-7 (e.g. a "connection holds the window" flag).
- `PowerOption.reserved[1..3]` for further blocks.

## Open questions for the maintainers (mirrors opendisplay-protocol#15)

1. Placement: PowerOption carve (this proposal) vs a new config block vs
   growing the MSD / scan response.
2. Sleep duration: 1-byte piecewise code (this proposal) vs 2-byte seconds.
3. The `WakeCause` list. `reset` is kept although the main ESP32 targets
   cannot report it, so nRF (`RESETREAS.RESETPIN`) has a home.
4. Countdown while connected: held at the full window (this proposal, it is
   what the sleep logic does) vs paused at its current value.
