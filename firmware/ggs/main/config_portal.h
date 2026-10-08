#pragma once

// Starts the HTTP configuration portal on the AP interface, port 80.
// One page: home network SSID/password, hotspot SSID/password, Home
// Assistant broker URI/user/password, device ID. Saves to NVS and
// restarts the device.
// Returns false when the HTTP server could not start: without it no OTA
// can reach the device, so the caller must not confirm this firmware.
bool config_portal_start(void);
