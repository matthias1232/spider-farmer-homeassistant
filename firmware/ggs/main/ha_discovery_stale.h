#pragma once
#include <stddef.h>

// GENERATED FILE -- do not edit by hand.
// Source: tools/gen_discovery.py
//
// The retained discovery topics of earlier firmware that must be cleared with
// an empty payload, so Home Assistant stops showing entities this firmware
// no longer publishes.
//
// Regenerate with:  python3 tools/gen_discovery.py

extern const char *const HA_STALE_DISCOVERY_TOPICS[];
extern const size_t HA_STALE_DISCOVERY_COUNT;
