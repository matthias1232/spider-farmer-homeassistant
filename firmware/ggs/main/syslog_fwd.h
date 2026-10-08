#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Remote syslog forwarding
//
// Every system log line is handed to a small queue and sent to a syslog
// server by its own task, in parallel with the short ring the web page
// shows. The ring only keeps the last few lines; the syslog server keeps
// everything.
//
// Transports: UDP (RFC 5426), TCP (RFC 6587, octet counting) and TLS
// (RFC 5425). TLS verifies the server with a stored CA certificate, or
// with the built-in bundle of public CAs, or not at all when "accept any
// certificate" is set.
//
// The logging hook never touches the network: syslog_fwd_push() is a
// non-blocking queue send. A full queue (server slow or unreachable) costs
// a counted drop, never a stall of the task that logged.
// ============================================================================

#define SYSLOG_PROTO_UDP 0
#define SYSLOG_PROTO_TCP 1
#define SYSLOG_PROTO_TLS 2

// Reads the stored settings and starts forwarding if a host is set.
void syslog_fwd_start(void);

// Applies new settings at runtime (Settings page). An empty host stops
// forwarding; the open connection is closed by the task.
void syslog_fwd_configure(const char *host, int port, int proto, bool insecure);

// Queues one cleaned log line. Called from the logging hook: never blocks.
void syslog_fwd_push(const char *line);

// Copy the MQTT log into the stream too: 0 off, 1 raw controller traffic,
// 2 raw plus Home Assistant traffic. Applied at once.
void syslog_fwd_set_mqtt(int mode);

// Queues one MQTT packet (called from live_log_add for every packet, also
// while no viewer is open). dir is a log_dir_t. Never blocks; heap only
// while the option is on.
void syslog_fwd_push_mqtt(int dir, const char *type, const char *topic,
                          const uint8_t *payload, size_t len);

typedef struct {
    bool     enabled;         // a host is configured
    bool     connected;       // TCP/TLS session up, or UDP socket ready
    int      proto;
    char     host[64];
    int      port;
    uint32_t sent;
    uint32_t dropped;         // queue full, or lost to a failed send
    uint32_t reconnects;
    char     last_error[64];
} syslog_fwd_status_t;

void syslog_fwd_status(syslog_fwd_status_t *out);
