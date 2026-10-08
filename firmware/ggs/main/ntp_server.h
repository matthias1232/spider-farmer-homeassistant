#pragma once

// ============================================================================
// NTP server for hotspot clients
//
// Needed because the DNS hijack can point a client's time server at the
// bridge. Without something listening there, the client asks for the
// time, gets nothing, and never sets its clock — and a controller with
// no clock cannot complete a TLS handshake, because certificate
// validity dates cannot be checked.
//
// The failure is quiet and easy to misread: the controller associates,
// gets an address, resolves the cloud hostname correctly, and then
// simply never connects. Nothing in the log says why.
//
// The bridge's own clock comes from a real NTP server upstream, so what
// is served here is as good as the bridge's time. Until that first sync
// the server reports itself unsynchronised, which is the honest answer
// and tells the client to wait rather than trust a wrong time.
// ============================================================================

void ntp_server_start(void);
