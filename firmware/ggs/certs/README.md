# TLS certificates for the MITM proxy

These files are embedded into the firmware via `EMBED_TXTFILES` (see
`firmware/ggs/main/CMakeLists.txt`) and used by `firmware/ggs/main/mitm_proxy.c`:

| File | Role |
|---|---|
| `sf_ca.pem` | CA certificate used to verify the real Spider Farmer cloud when the bridge talks to it. |
| `proxy_cert.pem` | Server certificate the bridge shows to the GGS controller, and at the same time the client certificate used for mTLS against the real Spider Farmer cloud. |
| `proxy_key.pem` | Private key matching `proxy_cert.pem`. |

## Where these files come from

The certificate pair is **not freshly generated for this project**. It originates
from the original SpiderBridge reverse-engineering work and ships (identically)
with the Python bridge this ESP32 port is based on:

- https://github.com/iceboerg00/spiderfarmer-bridge — `certs/` (TLS certificates)

The material was extracted while reverse-engineering the official Spider Farmer
app's communication with the GGS controller and its cloud, so that the bridge can
terminate the controller's TLS session locally (the same technique the original
Python bridge uses).

> **Note:** publishing a private key is normally bad practice. In this specific
> case the "private key" is not a personal credential — it is the shared client
> certificate material of the Spider Farmer GGS ecosystem, already public in the
> repository linked above, and required for the WLAN relay path (controller →
> bridge → real cloud) to work at all. Do not reuse it for anything else.

## Regenerating

The build works with the committed files as-is. If you want your own
self-signed certificate pair for local experiments, generate a matching pair
(they must match each other):

```bash
openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes \
  -keyout proxy_key.pem -out proxy_cert.pem \
  -subj "/CN=spiderbridge.local" \
  -addext "subjectAltName=DNS:spiderbridge.local"
```

A self-signed pair will be rejected by the real Spider Farmer cloud (mTLS
client authentication) — it only makes sense for offline/local testing of the
bridge's own TLS endpoints.
