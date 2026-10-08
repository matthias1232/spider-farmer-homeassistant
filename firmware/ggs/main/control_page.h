#pragma once
#include "esp_http_server.h"

// ============================================================================
// Control page
//
// Everything the grow controller exposes, operable from the browser:
// fans, lights, outlets, climate accessories, schedules, cycles and PPFD.
//
// It uses the same translation path as Home Assistant — a click here and
// an MQTT command produce the identical setConfigField. That keeps one
// code path instead of two, so a fix benefits both.
//
// Live values come from the device cache, which is fed by the
// controller's own status frames.
// ============================================================================

esp_err_t control_page_handler(httpd_req_t *req);
esp_err_t control_data_handler(httpd_req_t *req);
// GET /control/ver: change counter, polled by the open page.
esp_err_t control_ver_handler(httpd_req_t *req);
esp_err_t control_set_handler(httpd_req_t *req);

// Renames or forgets a controller. Affects the Home Assistant side only;
// the MAC the controller and the vendor cloud use never changes.
esp_err_t control_device_handler(httpd_req_t *req);

// POST /control/plan: grow plan start/stop/save/delete (large body).
esp_err_t control_plan_handler(httpd_req_t *req);

// GET /control/templates, POST /control/template: plan templates.
esp_err_t control_templates_handler(httpd_req_t *req);
esp_err_t control_template_handler(httpd_req_t *req);

// GET/POST /control/lplan: long grow plans kept on the bridge.
esp_err_t control_lplan_get_handler(httpd_req_t *req);
esp_err_t control_lplan_post_handler(httpd_req_t *req);

// The time zone names the firmware knows, for the per-device picker.
esp_err_t control_zones_handler(httpd_req_t *req);
