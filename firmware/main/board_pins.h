#pragma once

/* Waveshare ESP32-S3-Touch-LCD-3.49 pin map (from RSVP Nano / Waveshare wiki). */

#define HERMES_LCD_CS     9
#define HERMES_LCD_SCLK   10
#define HERMES_LCD_D0     11
#define HERMES_LCD_D1     12
#define HERMES_LCD_D2     13
#define HERMES_LCD_D3     14
#define HERMES_LCD_RST    21
/* Rev1 backlight PWM; Rev2 boards use 42. */
#define HERMES_LCD_BL_REV1 8
#define HERMES_LCD_BL_REV2 42

/* Native panel scan order (portrait controller memory). */
#define HERMES_PANEL_W    172
#define HERMES_PANEL_H    640

/* Logical UI — RSVP LandscapeFlipped (hold device like a bookmark). */
#define HERMES_UI_W       640
#define HERMES_UI_H       172

#define HERMES_I2C_SDA    47
#define HERMES_I2C_SCL    48
#define HERMES_TOUCH_SDA  17
#define HERMES_TOUCH_SCL  18
#define HERMES_TCA_ADDR   0x20
#define HERMES_TCA_BL_PIN 1
#define HERMES_TCA_SYS_PIN 6 /* battery power-hold — required off-USB */
#define HERMES_TCA_AUDIO_PIN 7
#define HERMES_TOUCH_ADDR 0x3B

#define HERMES_BOOT_PIN   0
#define HERMES_PWR_PIN    16
#define HERMES_BATT_ADC   4

/* Waveshare TF slot — 1-line SDMMC (official demo pins). */
#define HERMES_SD_CLK     41
#define HERMES_SD_CMD     39
#define HERMES_SD_D0      40
