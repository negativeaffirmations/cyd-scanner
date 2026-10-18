// pins.h — ESP32-C5 scanner pin map (DevKit + XIAO).
// Source of truth: hardware/PINOUT.md. Keep the two in sync.
//
// One header, two boards: the XIAO ESP32-C5 board def (-DARDUINO_XIAO_ESP32C5)
// selects the mini-build pins; the DevKit (no such macro) keeps the originals.
// Only the link UART and the GPS RX move — the scanner firmware is otherwise
// identical across both envs. See docs/mini-build.md §2.2.
#pragma once

#if defined(ARDUINO_XIAO_ESP32C5)

// --- XIAO ESP32-C5 (mini build) ---
// The XIAO only breaks out GPIO 0,1,7,8,9,10,11,12,23,24,25 (8/9 strapping),
// so the DevKit's link pins (GPIO4/5) and GPS RX (GPIO15) are not available
// and move to broken-out GPIO. 5/23/24 are plain GPIO matrixed to the UART.
#define LINK_TX_PIN 23  // GPIO23 -> XIAO-S3 host RX (GPIO44 / D7)
#define LINK_RX_PIN 24  // GPIO24 <- XIAO-S3 host TX (GPIO43 / D6)

// --- Onboard GPS (GY-NEO6MV2 NEO-6M) on UART0 (console is USB-CDC, UART0 free) ---
// Module TXD -> C5 GPIO1 (NMEA data in, 9600 8N1). Receive-only (gps.cpp passes TX=-1),
// so the NEO-6M RXD pin is left unconnected. GPIO0/1 double as LP/XTAL_32K — fine as
// plain GPIO with no 32 kHz crystal fitted; verify the pin with known traffic (risk #4).
#define GPS_RX_PIN 1   // C5 UART0 RX  <- NEO-6M TXD
#define GPS_TX_PIN -1  // unused (read-only); NEO-6M RXD left floating

#else

// --- ESP32-C5 DevKit (original build) ---
// --- Inter-board UART link to the CYD ---
// Dedicated UART; leaves UART0 (GPIO11/12) free for the USB console and
// USB-Serial/JTAG (GPIO13/14) free for programming.
#define LINK_TX_PIN 5  // GPIO5 (LP_UART_TXD) -> CYD GPIO27 (RX)
#define LINK_RX_PIN 4  // GPIO4 (LP_UART_RXD) <- CYD GPIO22 (TX)

// --- Onboard GPS (GY-NEO6MV2 NEO-6M) on UART0 (console is USB-CDC, so UART0 is free) ---
// Module TXD -> C5 GPIO15 (NMEA data in, 9600 8N1). Module RXD -> C5 GPIO23 (config only; unused, read-only). See hardware/PINOUT.md §5b.
#define GPS_RX_PIN 15  // C5 UART0 RX  <- NEO-6M TXD
#define GPS_TX_PIN 23  // C5 UART0 TX  -> NEO-6M RXD (unused; read-only uses TX=-1)

// NOTE: GPIO25 and GPIO26 are UNVERIFIED / possibly swapped on this board.
// Do not use either without testing first. See hardware/PINOUT.md.

#endif
