#pragma once
#include <stddef.h>

// ============================================================================
// Home-Assistant-Discovery-Tabelle
//
// URSPRUENGLICH AUTOMATISCH ERZEUGT, inzwischen von Hand gepflegt.
// Quelle war: die 72 retained Discovery-Payloads der urspruenglichen
// Python-Bridge, direkt vom MQTT-Broker ausgelesen.
//
// Die Tabelle ist KEINE 1:1-Kopie mehr. Zwei bewusste Abweichungen:
//
//   - Mehrfach angemeldete Felder wurden auf je eine Entity reduziert.
//     Natural Wind, Speed, Standby Speed, Oscillation sowie Dim- und
//     Off-Threshold waren einmal pro Modus-Karte angemeldet, alle auf
//     demselben state- und command-Topic. Auf dem Draht ist jedes davon
//     ein einziges Feld, die Kopien konnten sich nur widersprechen. Die
//     abgeloesten Topics stehen in STALE_DISCOVERY_TOPICS in ha_mqtt.c
//     und werden beim Veroeffentlichen geleert.
//   - Neu: blower_close_co2 (Feld closeCO2), in der Python-Bridge nicht
//     vorhanden.
//
// Wer hier neu generiert, macht beides rueckgaengig.
//
// Platzhalter in topic und payload:
//   $D -> device_id   (z.B. "ggs_1")
//   $N -> Anzeigename (z.B. "GGS")
// ============================================================================

typedef struct {
    const char *topic;      // mit $D-Platzhaltern
    const char *payload;    // mit $D/$N-Platzhaltern
} ha_discovery_entry_t;

extern const ha_discovery_entry_t HA_DISCOVERY_TABLE[];
extern const size_t HA_DISCOVERY_COUNT;

// Groesste Payload dieser Tabelle, vor dem Einsetzen der Platzhalter.
//
// Laengste Eintraege sind die beiden fan-Entities mit ihren acht
// preset_modes. publish_discovery_for() leitet daraus seine
// Puffergroesse ab: ist der Wert zu klein, schlaegt
// expand_placeholders_for() fehl und die betroffene Entity wird
// stillschweigend uebersprungen. Bei jeder Aenderung an der Tabelle
// nachrechnen.
#define HA_DISCOVERY_MAX_PAYLOAD 1402
