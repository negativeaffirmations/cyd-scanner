// pins.h — Seeed XIAO ESP32-S3 Sense (headless host) pin map.
// Source of truth: docs/mini-build.md §2.1/§2.6 (+ hardware/PINOUT.md). Keep in sync.
#pragma once

// --- Inter-board UART link to the XIAO ESP32-C5 (UART1 matrixed onto the UART0 pads D6/D7) ---
#define LINK_TX_PIN 43  // D6 -> C5 GPIO24 (RX)
#define LINK_RX_PIN 44  // D7 <- C5 GPIO23 (TX)

// --- Sense onboard microSD (SPI, shared B2B bus with the camera group) ---
#define SD_SCK_PIN  7   // D8
#define SD_MISO_PIN 8   // D9
#define SD_MOSI_PIN 9   // D10
#define SD_CS_PIN   21  // per Seeed's Sense wiki/examples (SD.begin(21)); NOT verifiable from the repo's pinout JPG

// --- Status LED: the onboard user LED is GPIO21 (active low) == SD_CS_PIN, so the SD chip-select owns
// the pin and the LED function is a no-op (the LED just flickers with SD traffic). The host is headless
// and the LED is cosmetic (docs/mini-build.md §2.1). Set to a spare pad (e.g. 1 = D0) to re-enable.
#define SCAN_LED_PIN -1

// --- DS3231 RTC (Phase 3) ---
#define RTC_SDA_PIN 5   // D4
#define RTC_SCL_PIN 6   // D5
