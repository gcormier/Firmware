# Findings: wake-info MSD block on the M5Paper (2026-11-10)

Hardware check of the provisional 3-byte MsdWakeInfo block
(docs/PROPOSAL_WAKE_REASON_MSD.md, opendisplay-protocol#15) on firmware
`903ae56` (branch `feat/wake-cause-msd`, env `esp32-wrover-e-N4R8`).

## Setup

- Board: original M5Stack M5Paper, ESP32-D0WDQ6-V3, CH9102 USB-serial (COM20),
  BLE `xx:xx:xx:xx:xx:xx`, name `ODxxxxxx` ("bathroom" board; the "kitchen"
  M5Paper `xx:xx:xx:xx:xx:xx` was not available).
- Was running official 2.27.1 with the same config as the kitchen board
  (battery mode, PCF8563 RTC sensor type 8, MOSFET latch, 900 s sleep, 20 s
  window). Backed up first: `C:\od\cfg\bath-backup-2271.yaml`.
- App image flashed only (`write-flash 0x10000`), LittleFS config kept.
- Test config: the backup with `deep_sleep_time_seconds: 120` and
  `wake_info_msd_slot: 7` (block at dynamic[6..8]).
- **USB powered throughout.** On USB the latch release does not cut the rail,
  so every scheduled wake is an ESP32 deep-sleep timer wake, not an RTC
  power-off wake.
- Tools: `C:\od\cfg\wakewatch.py` (active BLE scan, logs and decodes the
  block), `serlog.py` (serial without resetting), `sleepcmd.py` (0x0053),
  `holdconn.py` (hold a connection N s). Note: the Windows scanner only sees
  `ODxxxxxx` with **active** scanning, and it drops reports for 10-50 s at a
  time, so not every advertisement is logged.

## Results

| # | Check | Result |
|---|---|---|
| 1 | EN reset (serial adapter RTS) -> cause | `power_on` (1). As predicted, the ESP32 reports an EN reset as POWERON. |
| 2 | Deep-sleep timer wake -> cause | `timer` (4), 7 wakes in a row. |
| 3 | Planned sleep byte vs config | code 8 = 120 s with the 120 s config; see 7 for 900 s. |
| 4 | Seconds-until-sleep counts down | yes: 119, 117, 115 ... 18 after the cold boot (120 s min-wake hold); 20 -> 1 in each 20 s timer-wake window. |
| 5 | Countdown predicts the actual sleep | first advert 22:26:53.1 said 119 s -> sleep due 22:28:52; serial: sleep at boot +123.5 s (~22:28:51.5). Later windows within 1 s. |
| 6 | 0x0053 with a 90 s override | serial "Entering deep sleep for 90 seconds (host override, one cycle)"; adverts gone 22:35:52 -> back 22:37:22 (~90 s). The following cycle reverted to 120 s (22:37:44 -> 22:39:44). The planned-sleep byte read 120 throughout: the override sleeps immediately and is never advertised (by design, see proposal). |
| 7 | 900 s config with the block on | written live over BLE; the next advertisements already read code 60 = 900 s (the config write raised the reboot flag, then a connection cleared it). Slept ~22:48:01 ("900 seconds (config)"), next adverts 23:02:53 with until=19 -> woke ~23:02:52, i.e. 900 s; cause `timer`. |
| 8 | A connection resets the countdown | connected at boot +6.5 s, held ~35 s, disconnected at +41.4 s, slept at +61.4 s (full 20 s window after the disconnect); adverts after the disconnect read 10 -> 4, consistent with the restarted window (without the reset it would have slept at ~+21 s). |
| 9 | Reboot flag (status bit1) | stayed 1 through timer wakes until a client connected, then 0. |
| 10 | Host decode | py-opendisplay `feat/wake-cause-msd` decodes the captured payloads to the same cause / seconds / duration. |

Example payloads (manufacturer data after the company id, slot 7):

```
0000000000000177080000a19733  power_on  until=119  sleep=120  (cold boot)
0000000000000414080000a098d3  timer     until=20   sleep=120  (timer wake)
```

## Battery tests (2026-11-11, same board, 120 s config, USB unplugged ~08:48)

| # | Check | Result |
|---|---|---|
| 11 | RTC power-off wake on battery | **`rtc` (3)** on all 5 wakes 08:49:50 ... 08:56:55, every ~2 min 20 s (120 s sleep + 20 s window); reboot flag 0 (cleared by firmware on RTC wakes, as before). The wakes logged just before the unplug read `timer`. Battery read 4.10 V. |
| 12 | Rocker held 2-3 s while powered off | **`power_on` (1)** cold boot at ~08:57:25 with the 120 s cold-boot countdown (until=88 at 08:57:57). |
| 13 | Rocker pressed again while awake | no reboot; button byte dynamic[0] 0x00 -> 0x81 -> 0x21 (a button event), cause stays `power_on`. |
| 14 | Brownout | **`brownout` (9)**, unprovoked: on a cold boot (on USB, 08:26) the brownout detector fired during display init; the next boot advertised `brownout`. |

The rocker power-on advertised reboot flag 0, where a cold boot starts at 1.
Explained: this board IS configured in the user's Home Assistant (released
OpenDisplay integration, entry `ODxxxxxx`), which connects after a reboot
(config resync, firmware-version read 0x0043 seen at the first logged boot)
and that connection clears the flag. HA was running throughout these tests,
so its connections are part of the observed behaviour (e.g. the reboot flag
going 1 -> 0 in the USB runs). This is exactly the level-1 limitation noted
below: the flag is a poor wake-cause signal once any client connects.

## Not verified

- **EN/rear reset on battery**: on battery the rear button cuts power, so it
  reduces to test 12 (rocker power-on).
- **Button wake (`button`)**: the M5Paper config has no deep-sleep wake
  button.
- `software`, `panic`, `watchdog`: not provoked.
- The new HA entities on a live HA instance: not done. The board is in the
  user's HA, but HA runs the released integration, not the
  `feat/wake-cause` branch.

## Implications for the HA level-1 fallback

Result 9 matters for the reboot-flag fallback (`scheduled` / `restart`): the
flag only clears when a client connects. On a deep-sleeping device that no
host ever connects to, every scheduled wake keeps advertising reboot=1, so the
fallback reports `restart` for scheduled wakes. Under the HA integration this
is normally fine because HA connects after a reboot (config resync), which
clears the flag; it is a limitation of level 1, and one reason for level 2.

## Final state of the board

After the battery tests the board is to be restored to official 2.27.1 and
the original config (`C:\od\cfg\bath-backup-2271.yaml`), which needs USB.
The restore procedure was run once already on 2026-11-11 (app flash + config
write, readback identical) before the battery tests re-flashed the test build.
