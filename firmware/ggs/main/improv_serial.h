#pragma once

// ============================================================================
// Improv Wi-Fi over serial (https://www.improv-wifi.com/serial/)
//
// What the Tasmota web installer / ESP Web Tools speak after flashing: they
// ask the device for its name and version, list the networks it sees and
// send the chosen SSID and password -- all over the USB serial port. With
// this the bridge can be put on the home network right from the installer,
// like a Tasmota device, without touching its hotspot.
//
// Listens on UART0 (the USB port, 115200 baud) alongside the log output.
// ============================================================================

void improv_serial_start(void);
