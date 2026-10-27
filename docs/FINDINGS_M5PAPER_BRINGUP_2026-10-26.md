# M5Paper (original, ESP32) hardware bring-up — 2026-10-26

Hardware validation of the two M5Paper branches on a real device:

- `greg/awesome-dijkstra-5pq3ha` (PR 1): panel IC 3005 on the FastEPD IT8951
  path, `m5paper_init()`.
- `greg/awesome-dijkstra-5pq3ha-rtc` (PR 2, stacked): RTC power-off sleep,
  sensor type 8 (PCF8563/BM8563) and the MOSFET latch.

**Device:** original M5Stack M5Paper. ESP32-D0WDQ6-V3, MAC 3c:61:05:09:20:54,
16 MB flash, **CP2104** USB-UART (not CH9102, so it needs the Silicon Labs
CP210x driver). The panel is rigid, which suggests v1.0, but this is not
confirmed. Built as `esp32-wrover-e-N4R8`, pioarduino 55.03.39, flashed from
Windows 11 on COM16. The original 16 MB flash was backed up before erasing.

**Host-side tooling:** serial logging opened with DTR/RTS deasserted, because
opening the CP2104 normally resets the ESP32. BLE uploads used `py-opendisplay`
7.14.1. Touch and button events are only published in the BLE MSD, not on
serial at INFO level, so they were read from the advertisements or from a
`-DOD_LOG_LEVEL=OD_LOG_DEBUG` build.

## Summary

| Area | Result |
|---|---|
| Boot, `m5paper_init()`, FastEPD init | PASS |
| 4bpp gray image | **FAIL → fixed** (byte-swapped IT8951 load, see §1) |
| Orientation | PASS (landscape, not mirrored) |
| Contrast at FastEPD's fixed VCOM −1.5 V | Acceptable to the eye; −2.30 V patch kept unapplied (§2) |
| Repeated pushes | PASS (several full pushes, no wedge, no WDT) |
| Mono (color_scheme 0), DU fast path | Load verified by IT8951 memory readback (§1). Not checked on screen |
| FAST refresh in 4bpp mode | **FAIL → fixed** (gray data sent as 1bpp DU, §1) |
| Touch (GT911) | PASS with flags **0x06**. Fixed GPIO36 errors (§3) |
| Buttons 37/38/39 | PASS (press and release on all three) |
| Battery hold (GPIO2) after unplugging USB | PASS (19 min on battery, still advertising) |
| Power-button hold time | **Not measured** (§6) |
| Battery voltage factor | **0xAE** (2:1 divider, measured at full charge, §7) |
| RTC sleep on USB (fallback path) | PASS |
| RTC power-off on battery, 120 s | PASS: 20 arms, 19 wakes + 1 pending, 0 missed (§4) |
| 600 s (1/60 Hz countdown) | PASS on USB: RTC timer fired (TF set) before the ESP32 timer |
| 16000 s (minute/hour alarm) | PASS on USB: alarm flag set at the expected minute (§4) |
| Image push in a wake window | see §5 |
| Off current | **Not measured** |
| Boot screen not redrawn on battery RTC wakes | **Not observed visually.** The firmware path (`is_deep_sleep_wake`) is confirmed on USB |
| All CI targets build | PASS (12 envs in `firmware-targets.json`, not 11) |

## 1. IT8951 byte order (FastEPD bug) — fixed on PR 1

**Symptom:** the 4bpp test image was readable and the right way up, but fine
detail looked "interlaced / shifted horizontally".

**Cause:** FastEPD 2.2.0 loads IT8951 frames with `IT8951_LDIMG_L_ENDIAN`
unless the panel has `BB_PANEL_FLAG_MIRROR_X`. That swaps the two bytes of
every 16-bit word, so 4bpp pixel pairs land 2 px off and 1bpp bytes land
8 px off. M5Stack's M5EPD driver sends the same high-nibble-first bytes with
`IT8951_LDIMG_B_ENDIAN`. The reTerminal ED103TC2 is MIRROR_X, so it already
gets B_ENDIAN, and nobody had seen the bug. The old note in
`fastepd_partial_refresh()` about "IT8951 region DU had persistent edge
alignment issues" may be the same bug.

**Fix (`f271dc5`):** for unmirrored IT8951 panels (only the M5Paper today),
full refreshes go through `it8951_full_write_unmirrored()` in
`src/display_fastepd.cpp`. It sends the same frame with the same GC16
waveform, loaded B_ENDIAN. `it8951_fullscreen_du()` uses B_ENDIAN too.
Mirrored panels keep FastEPD's own writer, unchanged.

**Verified:** the 4bpp image is sharp after the fix (user confirmed).
A temporary readback of IT8951 memory after each load (MEM_BST_RD, sampled
every 27th row, not committed) confirms the frame lands as sent:

| Path | Sampled bytes matching the framebuffer |
|---|---|
| Full, 4bpp (GC16) | 19200/19200 |
| Full, 1bpp (mono config) | 2400/2400 (1232 with the old byte swap) |
| DU, 1bpp (mono config, FAST upload) | 2400/2400 |

**Not verified:** the 1bpp bit order on screen (the readback only proves the
bytes arrive unswapped). Worth reporting upstream to FastEPD.

**FAST refresh in 4bpp mode (fixed, `27016a4`, PR 1):** the readback also
showed that a FAST upload with the GRAY16 config sent the 4bpp framebuffer
through `it8951_fullscreen_du()`, which loads w/8 bytes per row as 1bpp. Every
sampled byte in IT8951 memory equalled the raw 4bpp bytes, so the top quarter
of the gray image would show as 1bpp noise. The parallel path already fell
back to a full refresh for 4bpp. The IT8951 path now does too, for direct
and partial transfers. After the fix the same upload logs `EPD refresh: FAST`
and loads through the 4bpp full path (19200/19200). The ED103TC2 (reTerminal)
takes the same branch but was not tested.

## 2. VCOM

`bbepInitIT8951()` always writes VCOM −1.5 V. The `iVCOM` passed to
`setPanelSize()` is not used on the IT8951 path, so the branch's `-1600` is a
no-op. M5EPD uses −2.30 V for this panel. Gray steps 0 (black) to 15 (white)
looked right at −1.5 V, so nothing was changed. A tested patch that writes
−2.30 V after `initIT8951()` for panel 3005 is available if a side-by-side
comparison favours it.

## 3. Touch

- **Mapping:** the GT911 reports portrait coordinates (540×960). With
  flags 0 the firmware clamps y to 539. Corner taps logged with SWAP_XY only:
  TL (63,483), TR (906,488), BR (903,46), BL (50,58), centre (479,271). So
  x is correct and y is inverted, giving **flags = SWAP_XY | INVERT_Y = 0x06**,
  the same as M5EPD's ROTATE_0. The opendisplay.org `m5-paper` preset is
  updated (`a075d2c`).
- **GPIO36 errors (fixed, `f271dc5`):** GT911 INT is GPIO36, an input-only pad
  with no internal pull-up. `gt911_int_wake_before_irq()`, `attach_touch_int()`
  and `gt911_hw_reset()` drove it as an output or set INPUT_PULLUP, which
  logged `GPIO can only be used as input mode` and `gpio_pullup_en` errors on
  every boot and every EPD refresh. These pads are now never driven and are
  read as plain INPUT.
- **I2C address varies:** because INT can't be driven, the GT911 comes up at
  0x5D on some boots and 0x14 on others. Auto-detect handles it.

## 4. RTC power-off (PR 2)

**Schematic (A4-SYS_PWR):** FET1 (PMOS) switches VBAT. Its gate, `PWR_EN`,
is pulled up to VBAT_IN and pulled low by FET2 (`PS_ON` = GPIO2), by
`KEY_PUSH` (the rocker pushed in) through D10, and by `RTC_ALM` (PCF8563 INT)
through D7. `KEY_PUSH_IO` (GPIO38) reads the key through D8 and appears to
share the R34 pull-up node with `RTC_ALM`, so an asserted RTC INT probably
also reads as "rocker pushed" until it is cleared.

**USB fallback:** `Entering deep sleep for 120 seconds (config), RTC
power-off`, then a timer wake (`Reset reason: DEEPSLEEP`). The boot screen is
not redrawn, the 20 s window applies, and it re-arms. No `RTC: not reachable`
or `arm failed` errors.

**Battery, 120 s:** BLE presence alone was misleading, because the Windows
scanner misses whole 20 s windows. A temporary NVS counter (not committed)
recorded arms and wakes instead. Over about 46 min (16 min on USB, then
30 min on battery): **arms = 20, wake boots = 19, plus 1 pending at replug**
(`TC=82 T=5E`, 94 s left). No arm went without a wake. 18 of the 19 wake
boots had TF set. The one without was a USB wake where the ESP32 timer beat
the RTC by about 1 s (`T=01` seen at that boot).

**600 s:** boot dump at the wake shows `TC=83 T=0A C2=05`: 1/60 Hz countdown
from 10, TF set, TIE on.

**16000 s:** armed at RTC 13:33:27. 267 min (rounded up) gives a target of
18:00. Readback `AL=00 18 80 80`, `C2=02` (AIE), `TC=03` (timer off). Correct.
It fires up to 59 s early because the alarm ignores seconds.

**16000 s fire (USB, second run):** armed at RTC 04:11:40 with `AL=38 08 80 80`
(08:38). For the test only, the ESP32 backstop timer was extended by 600 s so
the RTC would act first. At the next boot (RTC 08:42:38) the dump showed
`C2=0A`: **AF set**, AIE on, so the alarm matched. The ESP32 itself woke on its
timer (`Wake-up reason: timer`): on USB the RTC INT only pulls `PWR_EN`, and the
rail is already up, so nothing wakes the chip. That is expected. On battery the
same INT switches the rail on. The ESP32 timer (16600 s nominal) expired after
about 16260 s of RTC time, so the ESP32 slow clock ran about 2% fast. The wake
cycle after that re-armed the 120 s countdown and fired normally (`C2=05`, TF).
Not verified: the 16000 s wake on battery.

**Leftover RTC state from M5Stack's factory firmware:** with the USB-only
config (no sensor type 8), the board switched itself back on about 60–75 s
after every reset-button power-off. A repeating RTC timer left armed by the
factory firmware survives a flash erase, because the RTC is on VBAT_IN. The
RTC config clears it at every boot. **Fixed (`0b7bf40`, PR 1):**
`m5paper_init()` now clears the RTC interrupts, timer and alarm on every
boot. On -rtc it skips that when RTC power-off is configured, because
`rtcConsumeWake()` does the clear itself and needs the flags. Verified with a
temporary readback: armed `C2=01 TC=82` before, `C2=00 TC=03` after. Not
verified: that the phantom power-on is gone after a battery power-off.

**VL flag:** the seconds register has VL (bit 7) set, meaning the RTC lost
power at some point (probably long ago). The firmware doesn't use wall-clock
time, so it's harmless.

## 5. Image push in a wake window

PASS on USB, pushed during a 20 s timer-wake window: `[EPD session]
acquire: COLD bring-up` (the IT8951 re-inits after deep sleep), then
`DW complete: 71 chunks, 259200/259200 bytes … 11.92 s` and `EPD refresh:
FULL`, `refreshSuccess=1`. It was a full image transfer, as expected. Not
repeated during a battery RTC-wake window, where the boot path differs only in
how the wake is detected.

## 6. Power-on, power-off and buttons

- **Power button = rocker pushed in** (KEY_PUSH, GPIO38). M5Stack documents a
  2 s hold. `m5paper_init()` drives GPIO2 at about 0.17 s after boot (about
  0.3–0.5 s after power-on, counting the ROM and bootloader). The hold time was
  not measured, because tests were confused by the leftover RTC timer and by
  reset presses that didn't register.
- **Reset button (back)** cuts power on battery (GPIO2 released), unless an
  RTC interrupt is holding `PWR_EN`.
- **`CMD_POWER_OFF` (0x0052)** used to NACK on the MOSFET latch. It now
  drives GPIO2 low via `powerLatchTriggerOff()` (on -rtc). On USB the board
  went silent on serial and BLE for 60 s and the reset button brought it
  back. Not verified on battery. The vendored protocol header still says
  "D-FF only" and needs an upstream update; no wire bytes change.
- **Deep-sleep button wake:** a classic ESP32 has ext0 only, so just GPIO37
  (rocker up) is armed. The log warns `pin 37 has no internal pull`. 38 and 39
  are timer-only. On battery with RTC power-off the ESP32 has no power at all,
  so only the rocker push (hardware) or the RTC can switch it on. Rocker
  up/down can't.
- **Cold boot stays awake 120 s** before the first sleep
  (`DEFAULT_MIN_WAKE_TIME_SECONDS`). This is by design. Timer wakes use the
  20 s window.

## 7. Battery voltage

The schematic shows R83 3 kΩ / R86 11 kΩ (VBAT_S = 0.79 × VBAT). M5EPD
assumes 2:1 (`SCALE_INV 2`). After a night on USB (battery full, about 4.20 V),
GPIO35 read raw 2416 and the eFuse-calibrated `analogReadMilliVolts()` read
2101 mV, which is exactly 2:1. So this board does not match that schematic
revision. The firmware computes V = raw × factor / 100000, so 4.20 V needs
factor 174 = **0xAE**. The `m5-paper` preset is updated on both opendisplay.org
branches (`5479e3f`, merged as `4d6e092`). The board now reports 4.20 V. Not
checked against a multimeter, or at a lower charge, where ADC non-linearity
may add error.

## Changes

| Commit | Branch | What |
|---|---|---|
| `f271dc5` | Firmware `greg/awesome-dijkstra-5pq3ha` | IT8951 B_ENDIAN for unmirrored panels; never drive input-only touch INT |
| `207ac66` | Firmware `-rtc` | merge of the above |
| `a075d2c` | opendisplay.org `greg/awesome-dijkstra-5pq3ha` | `m5-paper` touch flags 0x6 |
| `05de81b` | opendisplay.org `-rtc` | merge of the above |
| `0b7bf40` | Firmware `greg/awesome-dijkstra-5pq3ha` | `m5paper_init()` clears the RTC interrupts, timer and alarm |
| `d0072af` | Firmware `-rtc` | merge of the above; 0x0052 on the MOSFET latch; skip the board-init RTC clear when RTC power-off is configured |

| `5479e3f` | opendisplay.org `greg/awesome-dijkstra-5pq3ha` | `m5-paper` voltage_scaling_factor 0xAE |
| `4d6e092` | opendisplay.org `-rtc` | merge of the above |
| `27016a4` | Firmware `greg/awesome-dijkstra-5pq3ha` | IT8951 FAST refresh in 4bpp falls back to full |
| `2d9a8f8` | Firmware `-rtc` | merge of the above |
| (this commit) | Firmware `-rtc` | readback, alarm-fire and battery results in this report |

No changes to the RTC driver itself were needed.

## Open items

- Mono (color_scheme 0) and the DU fast path on screen (bit order).
- Battery voltage against a multimeter, and at lower charge.
- Off-current measurement.
- Power-button hold time.
- Watching the screen during battery RTC wakes for a boot-screen redraw.
- The 16000 s alarm wake on battery (the fire is verified on USB).
- FAST refresh in 4bpp on the ED103TC2 (reTerminal).
- 0x0052 power-off on battery, and no phantom power-on after it.
- Follow-ups: update the 0x0052 `@targets`/`@errors` text in
  opendisplay-protocol (MOSFET latch now supported); upstream FastEPD endian
  bug report.
