#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

// ============================================================================
// Restart, factory reset, backup and restore
//
// Factory reset clears every namespace the firmware owns:
//
//   sbcfg    WiFi credentials, broker settings, clock, OTA, CA cert
//   sbdev    device names and topic slugs
//   sbmac    hotspot access list
//   sbdhcp   device labels
//   sbip     web interface IP rules
//
// After it the bridge comes up unconfigured: hotspot running, no uplink,
// the settings page at 192.168.10.1. That is a large step to take by
// accident, so both the web form and the MQTT entity require the exact
// word RESET rather than a single click.
//
// Backup is the counterpart: a JSON file holding the same settings,
// including the passwords, so a restore produces a working device rather
// than one that needs every credential typed in again. The page says so
// plainly — a backup that silently omits secrets is its own trap.
// ============================================================================

// Restarts after a delay, so an HTTP response or MQTT ack goes out first.
void sb_system_reboot(int delay_ms);

// Clears every namespace, then restarts.
void sb_system_factory_reset(void);

// POST /system/reset   — requires confirm=RESET
esp_err_t system_reset_handler(httpd_req_t *req);

// GET  /backup         — downloads the configuration as JSON
esp_err_t backup_get_handler(httpd_req_t *req);

// POST /restore        — applies a backup, then restarts
esp_err_t restore_post_handler(httpd_req_t *req);
