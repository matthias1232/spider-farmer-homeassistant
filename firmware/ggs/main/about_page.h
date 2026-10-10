#pragma once
#include "esp_http_server.h"

// ============================================================================
// About page
//
// Shows information about the SpiderBridge project, links to the GitHub
// repository, documentation, Web Installer, and Buy Me a Coffee support.
// ============================================================================

esp_err_t about_page_handler(httpd_req_t *req);
