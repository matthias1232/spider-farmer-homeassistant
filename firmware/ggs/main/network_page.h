#pragma once
#include "esp_http_server.h"

// ============================================================================
// Network page
//
// Who is on the hotspot, who is allowed on it, and who may reach this
// web interface. Three things that are separate in the firmware but
// belong together when diagnosing access problems.
// ============================================================================

esp_err_t network_page_handler(httpd_req_t *req);
esp_err_t network_data_handler(httpd_req_t *req);

// POST /network/act — add or remove a rule, change a mode, name a client.
esp_err_t network_act_handler(httpd_req_t *req);
