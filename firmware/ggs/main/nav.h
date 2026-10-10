#pragma once
#include "esp_http_server.h"

// ============================================================================
// Shared top navigation
//
// Every page previously wrote its own ad-hoc list of links, by hand, in
// whatever order happened to be convenient when that page was added.
// Two pages (control, log) were even missing links to Status and
// Firmware entirely — found only by grepping every page for href= and
// comparing the lists side by side.
//
// One function now emits the same six links everywhere, in the same
// order, with the current page marked so it is obvious where you are.
// Adding a seventh page means changing this list once, not auditing six
// files for one that got missed.
// ============================================================================

typedef enum {
    NAV_SETTINGS,
    NAV_CONTROL,
    NAV_STATUS,
    NAV_NETWORK,
    NAV_LOG,
    NAV_FIRMWARE,
    NAV_ABOUT,
} nav_page_t;

// Sends the nav bar as one or more HTTP chunks. Call right after the
// opening <body> (or after an <h1>, if the page has one) and before the
// page's own content.
void nav_send(httpd_req_t *req, nav_page_t current);

// The CSS the nav bar needs. Each page's own <style> block should
// include this once; it is deliberately not a full page stylesheet, so
// it does not fight with what each page already defines for itself.
extern const char NAV_STYLE[];
