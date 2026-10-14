// pins.h — ESP32-C5 DevKit pin map.
// Source of truth: hardware/PINOUT.md. Keep the two in sync.
#pragma once

// --- Inter-board UART link to the CYD (PROVISIONAL — not tested) ---
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
