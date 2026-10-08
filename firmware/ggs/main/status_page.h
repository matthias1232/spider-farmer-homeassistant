#pragma once
#include "esp_http_server.h"

// ============================================================================
// Status page
//
// Everything needed to work out why the bridge is misbehaving, without a
// serial cable: which network it joined and how strong the signal is,
// which addresses it holds, whether the broker connection stands, which
// controllers are talking, what the clock says, and how much memory is
// left.
// ============================================================================

esp_err_t status_page_handler(httpd_req_t *req);
esp_err_t status_data_handler(httpd_req_t *req);

// Recent firmware log lines, for the log view.
esp_err_t syslog_data_handler(httpd_req_t *req);

// POST /status/dst — switches daylight saving for the bridge, and for
// every controller when the sync setting is on.
esp_err_t status_dst_handler(httpd_req_t *req);
