# ESP32-S3 Speed Coach — UI and product architecture

Portrait 240×320 is the reference on-water layout. Landscape is supported but secondary.

## Live screen model

Active screens are instruments, not app pages:

- No conventional app bar
- Compact 22 px status rail: recording, water-lock, GPS quality (including stale), battery
- Wall-clock time stays on menu / settings / history
- Primary value: instant pace `/500 m` in tabular 56 px numerals
- Secondary pair: SPM and distance (32 px tabular)
- Slim split row: progress in the configured split, average pace, last-split delta vs average

Metric slots persist in NVS. While recording, taps and swipes are locked unless the athlete long-presses the status rail. The physical PWR key always starts/stops.

Interval live page: phase + remaining is dominant; SPM and pace share the middle; round is compact. WORK/REST is a persistent border color. The last five seconds of a time interval recolor the remaining slot; they do not cover the numbers. Interval CSV logs phase rows only — distance splits are not written during interval sessions.

## Threading

`stroke_task` (200 Hz) is the single producer of `activity_update` and of `coach_ui_snapshot_t`. The LVGL task consumes the snapshot at 5 Hz. No LVGL widget APIs from the IMU task.

GPS dropout: speed is omitted from averages when there is no valid fix. Session duration and strokes still advance.

## Immediate vs later

Shipped in this pass: water lock, stale GPS, split progress/delta, brightness + auto-dim, PWR shortcuts, NVS activity IDs, persisted metric slots, race mode (virtual-boat ahead/behind), step-test rate ladder, themed menu tiles, BLE heart-rate strap, C3 tracker GAP connect.

### BLE sensors

Heart-rate straps (`0x180D`) and the RowPod share a two-slot NimBLE central. The strap delivers bpm. The pod delivers one summary per stroke (catch, finish, arc) when its firmware has characteristic `9b7e1005`. Contract, packet layout, reconnect, and the hardware checklist are in [ble_sensors.md](ble_sensors.md).

USB mass-storage stays on the Settings **Export via USB** row, not on the menu.

BLE callbacks must not touch LVGL. The sensors page polls `sensor_hub` at 300 ms. Do not enable PSRAM to hide DRAM cost; log free internal heap before and after NimBLE init. The ESP32-S3 has no ANT+ radio, so ANT-only Garmin straps will not appear.

### Shipped: race mode

Target pace plus race distance. The live race page shows seconds vs a virtual boat (`delta_s = elapsed - distance / v_target`), remaining distance, and projected finish from session average speed. Green fill = ahead by more than 1 s; red = behind by more than 1 s; a 1 s deadband avoids strobing. `ACTIVITY_RACE` / `ACT_CMD_START_RACE` start this session type.

### Later: link mode

Versioned, sequence-numbered peer packets with monotonic timestamps. Evaluate ESP-NOW (low latency broadcast) vs BLE coexistence and battery before choosing transport. Same-module comparison only after the snapshot path is stable.

### Later: presets and sport profiles

Saved interval workouts. Rowing / dragon boat / generic paddle profiles that override IMU axis, stroke-period limits, and default metric slots — not the UI shell.

## Glyphs

UI strings are ASCII or `LV_SYMBOL_*`. The tabular number fonts (`lv_font_num_56`, `lv_font_num_32`) carry digits and `+ - . : /` only. Units (`m`, `st`) stay on the caption. Both number fonts fall back to Montserrat so a stray letter draws as text instead of an empty box.

## Hardware checklist

Portrait first, then all four rotations:

- Long values (`9:59.9`, `99.9 km/h`) do not clip
- Sunlight contrast in dark and light themes
- Water lock blocks metric taps and menu swipe; PWR still works
- GPS loss shows stale/red and does not pull average pace to `--` via zeros
- Interval WORK/REST color and 5 s cue without covering numbers
- Race ahead/behind box color with 1 s deadband; projected finish from average speed
- Step test SPM target rises each piece; SPM box tints outside +/-1 SPM
- Settings Sensors row shows `HR On / Pod Off` and opens the two-card Sensors page; USB export stays in Settings
- Heart-rate card pairs a `0x180D` strap, shows bpm, and reconnects after a drop. RowPod card subscribes to the per-stroke summary and offers Zero / side
- Status-rail Bluetooth glyph: accent while HR is fresh, red when a saved strap is lost or stale, hidden after Disconnect
- HR metric slot shows `--` when bpm is 0. Stroke CSV gains `Heart Rate (bpm)`; splits gain `Avg HR`
- Menu tiles readable in light and dark (surface fill, accent icon, contrast text)
- Menu-page title rail is transparent, 20 px, with a back chevron on every sub-page
- Interval distance/stroke remaining shows a number only; the unit is on the caption
- Race ahead/behind value shows `+` and `-` without a blank box
- Sensors status uses `...`, not a missing ellipsis glyph
- Split rollover updates progress and delta
- Start/stop toast, save, USB export
- Auto-dim after 15 s idle while recording; touch restores
- `idf.py size-components`: flash delta from tabular fonts; LVGL stack HWM; free internal heap
- Active vs idle current before enabling `CONFIG_PM_ENABLE`

### Build environment note (2026-08-19)

This tree targets **ESP-IDF 6.0.1**. Local components now `REQUIRES` the split IDF 6 drivers (`esp_driver_i2c`, `esp_driver_gpio`, `esp_driver_spi`, `esp_driver_ledc`, `esp_driver_sdmmc`) instead of the old catch-all `driver` component. LCD/SD slot pins use `gpio_num_t` / `GPIO_NUM_NC`.

Export `C:\Espressif\.espressif\v6.0.1\esp-idf` then:

```
idf.py fullclean
idf.py build
idf.py size-components
```

Then flash and walk the checklist above. Do not enable PSRAM to hide RAM cost.

## Acceptance

- Primary pace readable at arm’s length on 240×320
- No modal covers live data during normal interval work/rest
- No LVGL calls outside the LVGL-owned context
- GPS loss does not corrupt averages
- `sdkconfig.defaults` captures target, TinyUSB, fonts, and NimBLE central
- Stroke detection unchanged. CSV columns are only appended (`Heart Rate (bpm)`, `Avg HR`)
