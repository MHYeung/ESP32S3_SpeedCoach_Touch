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

## 4. Pins & modules

Below is a concise table of the main function modules and where their drivers/components live. Pin assignments are generally configurable — check the component folder and `sdkconfig` for the concrete mappings used for your board.

| Module | Component / Driver | Default / Notes |
|---|---:|---|
| I2C (sensor bus) | [components/i2c_helper](components/i2c_helper) | SDK reports 2 I2C ports (CONFIG_SOC_I2C_NUM=2); see `sdkconfig` for mappings |
| IMU (QMI8658) | [components/qmi8658](components/qmi8658) | I2C1: SDA=11, SCL=10, INT1=13, INT2=12 (CONFIG_IMU_QMI8658_*) |
| GPS (GNSS) | [components/gps_gt_u8](components/gps_gt_u8) | Typically UART; TX/RX pins configurable in component or `sdkconfig` |
| Display (ST7789) | [components/lcd_st7789](components/lcd_st7789) | SPI: MOSI=45, SCLK=40, MISO=-1, DC=41, CS=42, RST=39, BL=5 (CONFIG_LCD_ST7789_*) |
| Touch controller (CST328) | [components/touch_cst328](components/touch_cst328) | I2C0: SDA=1, SCL=3, RST=2, INT=4, I2C clk=400k (CONFIG_TOUCH_CST328_*) |
| Power button | [components/pwr_key](components/pwr_key) | GPIO pin set at runtime/config — see component config or `main/main.c` setup |
| Battery monitor | [components/battery_drv](components/battery_drv) | ADC or I2C depending on board; see component |
| RTC (PCF85063) | [components/rtc_pcf85063](components/rtc_pcf85063) | On the IMU I2C bus (I2C1 in current config) — see `PCF85063_init` usage |
| SD/MMC storage | [components/sd_mmc_helper](components/sd_mmc_helper) | SDIO or SPI mode; pins configurable in component |
