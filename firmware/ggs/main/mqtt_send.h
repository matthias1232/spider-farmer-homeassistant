#pragma once
#include "esp_http_server.h"

// ============================================================================
// Manual MQTT sending
//
// Lets the log view inject commands by hand, which is how undocumented
// fields get discovered: send a request, watch the reply in the same
// view, keep what works.
//
// Three modes, in increasing order of convenience:
//
//   raw       the JSON is injected verbatim on the controller's DOWN
//             topic, exactly as the cloud would send it. Nothing is
//             added or validated, so anything the protocol allows can
//             be tried.
//
//   method    only a method name and an optional params object are
//             given; the envelope the controller expects (pid, uid,
//             message id) is filled in. This is what makes probing
//             getConfigField and similar calls practical.
//
//   ha        the payload goes through the same translation as a Home
//             Assistant command, so the web interface and HA cannot
//             drift apart.
//
// Everything sent and received appears in the live log, so a request and
// its answer sit next to each other.
// ============================================================================

// POST /log/send
esp_err_t mqtt_send_handler(httpd_req_t *req);
