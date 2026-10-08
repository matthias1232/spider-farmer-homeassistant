#include <string.h>
#include <stdio.h>

#include "nav.h"

const char NAV_STYLE[] =
".topnav{display:flex;gap:1.1rem;align-items:center;flex-wrap:wrap;"
"padding:.6rem 0 .9rem;margin-bottom:.8rem;border-bottom:1px solid #2c303a}"
".topnav a{color:#9aa0aa;text-decoration:none;font-size:.85rem;"
"padding:.2rem 0;border-bottom:2px solid transparent}"
".topnav a:hover{color:#c3c8d0}"
".topnav a.cur{color:#8ab4f8;border-bottom-color:#8ab4f8;font-weight:600}"
".topnav .brand{color:#e6e6e6;font-weight:600;font-size:.95rem;"
"margin-right:.3rem}";

typedef struct {
    nav_page_t  id;
    const char *href;
    const char *label;
} nav_entry_t;

static const nav_entry_t ENTRIES[] = {
    { NAV_SETTINGS, "/",        "Settings" },
    { NAV_CONTROL,  "/control", "Control"  },
    { NAV_STATUS,   "/status",  "Status"   },
    { NAV_NETWORK,  "/network", "Network"  },
    { NAV_LOG,      "/log",     "MQTT log" },
    { NAV_FIRMWARE, "/update",  "Firmware" },
};
#define NAV_COUNT (sizeof(ENTRIES) / sizeof(ENTRIES[0]))

void nav_send(httpd_req_t *req, nav_page_t current)
{
    char buf[700];
    int n = snprintf(buf, sizeof(buf),
                     "<div class=\"topnav\"><span class=\"brand\">"
                     "SpiderBridge</span>");

    for (size_t i = 0; i < NAV_COUNT && n < (int)sizeof(buf) - 80; i++) {
        bool is_cur = (ENTRIES[i].id == current);
        n += snprintf(buf + n, sizeof(buf) - n,
                     "<a href=\"%s\"%s>%s</a>",
                     ENTRIES[i].href, is_cur ? " class=\"cur\"" : "",
                     ENTRIES[i].label);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "</div>");

    httpd_resp_send_chunk(req, buf, n);
}
