// pins.h — ESP32-2432S028R "CYD" pin map.
// Source of truth: hardware/PINOUT.md. Keep the two in sync.
#pragma once

// --- Display (ILI9341/ST7789, SPI) ---
#define TFT_MOSI_PIN 13
#define TFT_MISO_PIN 12
#define TFT_SCLK_PIN 14
#define TFT_CS_PIN   15
#define TFT_DC_PIN    2
#define TFT_RST_PIN  -1  // tied to board reset on most units
#define TFT_BL_PIN   21  // backlight, active high

// --- Touch (XPT2046, separate SPI bus) ---
#define TOUCH_CLK_PIN  25
#define TOUCH_CS_PIN   33
#define TOUCH_MOSI_PIN 32
#define TOUCH_MISO_PIN 39  // input-only
#define TOUCH_IRQ_PIN  36  // input-only

// --- microSD (TF slot, shared SPI) ---
#define SD_CS_PIN   5
#define SD_MOSI_PIN 23
#define SD_SCK_PIN  18
#define SD_MISO_PIN 19

// --- On-board RGB LED (active low) ---
#define LED_R_PIN 4
#define LED_G_PIN 16
#define LED_B_PIN 17

// --- Audio / speaker (DAC2 -> on-board amp + 2p speaker header) ---
#define AUDIO_PIN 26

// --- Inter-board UART link to the ESP32-C5 (PROVISIONAL — not tested) ---
// Dedicated UART on free expansion pins; keeps UART0/USB console free.
#define LINK_TX_PIN 22  // -> C5 GPIO4 (RX)
#define LINK_RX_PIN 27  // <- C5 GPIO5 (TX)
