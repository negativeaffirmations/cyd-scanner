// pins.h — ESP32-C5 DevKit pin map.
// Source of truth: hardware/PINOUT.md. Keep the two in sync.
#pragma once

// --- Inter-board UART link to the CYD (PROVISIONAL — not tested) ---
// Dedicated UART; leaves UART0 (GPIO11/12) free for the USB console and
// USB-Serial/JTAG (GPIO13/14) free for programming.
#define LINK_TX_PIN 5  // GPIO5 (LP_UART_TXD) -> CYD GPIO27 (RX)
#define LINK_RX_PIN 4  // GPIO4 (LP_UART_RXD) <- CYD GPIO22 (TX)

// NOTE: GPIO25 and GPIO26 are UNVERIFIED / possibly swapped on this board.
// Do not use either without testing first. See hardware/PINOUT.md.
