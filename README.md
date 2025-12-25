# RowCoach (ESP32S3_SpeedCoach_Touch) — Open DIY Rowing GPS & Stroke Coach

This repository contains a DIY, open-source rowing GPS and stroke coaching project built on ESP32-S3. It combines simple stroke detection and GPS-based speed/pace calculations with a compact LVGL touchscreen UI to provide a lower-cost alternative for coaches and athletes.

## 1. Features

- Display + touch UI using LVGL and an ESP32-S3 driver stack.
- Responsive UI Data Page with three configurable data slots (time, strokes, SPM by default). See [main/ui/ui_data_page.c](main/ui/ui_data_page.c) for layout and font logic.
- Activity toast: a small circular status toast used to indicate activity start/stop. Background color shows state (green = start, red = stop); icon text remains white. Configurable in [main/ui/ui_data_page.c](main/ui/ui_data_page.c).
- Shutdown prompt with two action buttons (`Shutdown` and `Cancel`). Buttons are styled with colored backgrounds and white labels. See [main/ui/ui_core.c](main/ui/ui_core.c).
- Dark/light theme support and orientation handling. Theme code lives in [main/ui/ui_theme.c/h](/main/ui/ui_theme.c) (where applicable) and is initialized at startup.
- Modular components under `components/` for sensors, drivers and helpers (I2C, SD/MMC, RTC, GPS, touch controller, etc.).

## 2. Background

RowCoach aims to make basic rowing performance tools accessible and affordable. The project provides:

- Simple, robust stroke detection (sensor-driven) to count strokes and detect stroke events.
- GPS-based speed and pacing calculations to report instantaneous and average pace/speed.
- A compact touchscreen UI for real-time feedback and basic interactions (start/stop, settings, activity logs).

The codebase is intentionally small and componentized so hobbyists and coaches can adapt hardware, tweak algorithms, and extend functionality. It has been tested with an ESP32-S3 development board and a Waveshare 2.8" SPI TFT touchscreen; pin mappings and driver selection are controlled via the `components/` folder and `sdkconfig`.

Development workflow: standard ESP-IDF build flow. Use `idf.py build` and `idf.py flash` from the repository root (make sure ESP-IDF and the toolchain are set up per Espressif docs).

## 3. Dev changelog

Recent development entries (newest first):

- 2025-12-26 — 4add2b3 — activity logger task init
- 2025-12-26 — dc0840f — added activity saving
- 2025-12-24 — 337d897 — pwr_key ux
- 2025-12-24 — b254c29 — pwr_key, battery init
- 2025-12-23 — 99d8e04 — rtc_pcf85063_init
- 2025-12-22 — 4dde18a — status bar component
- 2025-12-22 — 3799df6 — menu page and activity page init
- 2025-12-21 — 7ad657e — update SPM algo - dynamic

If you want the changelog to reflect a different range of commits or more/less detail, tell me how many entries you want and I'll update it.

---

See the `main/ui/` folder for UI implementation details and `components/` for hardware driver code. If you'd like, I can add small usage examples or a contributor guide next.
