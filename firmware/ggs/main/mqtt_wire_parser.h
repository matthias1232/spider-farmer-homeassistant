#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 1:1-Portierung von proxy/mqtt_parser.py — reines Byte-Parsing des
// MQTT-3.1.1-Wire-Formats, keine Python-Spezifika, daher direkt übertragbar.

#define MQTT_CONNECT     1
#define MQTT_CONNACK     2
#define MQTT_PUBLISH     3
#define MQTT_PUBACK      4
#define MQTT_SUBSCRIBE   8
#define MQTT_SUBACK      9
#define MQTT_PINGREQ     12
#define MQTT_PINGRESP    13
#define MQTT_DISCONNECT  14

typedef struct {
    uint8_t packet_type;
    uint8_t flags;
    const uint8_t *payload;
    size_t payload_len;

    // PUBLISH
    char topic[256];
    const uint8_t *message;
    size_t message_len;
    uint8_t qos;
    bool retain;
    uint16_t packet_id;

    // CONNECT
    char client_id[64];
} mqtt_packet_t;

// Parst so viele vollständige Pakete wie möglich aus buf[0..len).
// out_packets: Array mit Platz für max_packets Einträge.
// Rückgabe: Anzahl geparster Pakete.
// *consumed wird auf die Anzahl verarbeiteter Bytes gesetzt — der Aufrufer
// muss den Rest (len - *consumed) im Puffer für den nächsten Aufruf behalten
// (unvollständiges letztes Paket, TCP kann mitten in einem Frame schneiden).
int mqtt_parse_packets(const uint8_t *buf, size_t len,
                        mqtt_packet_t *out_packets, int max_packets,
                        size_t *consumed);

// Baut ein PUBLISH-Paket. out muss mindestens strlen(topic)+len(message)+8
// Bytes groß sein. Rückgabe: Länge des gebauten Pakets, oder -1 bei
// Pufferüberlauf.
int mqtt_build_publish(const char *topic, const uint8_t *message, size_t message_len,
                        uint8_t qos, bool retain, uint16_t packet_id,
                        uint8_t *out, size_t out_sz);

// ---------------------------------------------------------------------------
// Antwortpakete für den Local-Only-Betrieb (WAN aus).
//
// Ohne Cloud-Verbindung muss die Bridge dem Controller selbst antworten,
// sonst wertet er die Sitzung als tot und verbindet sich im Sekundentakt neu.
// Die folgenden Helfer bauen genau die Pakete, die der Controller erwartet.
// ---------------------------------------------------------------------------

// CONNACK mit Return-Code 0 (Verbindung akzeptiert), kein Session-Present.
int mqtt_build_connack(uint8_t *out, size_t out_sz);

// PINGRESP — Antwort auf den Keepalive des Controllers.
int mqtt_build_pingresp(uint8_t *out, size_t out_sz);

// PUBACK für ein QoS-1-PUBLISH.
int mqtt_build_puback(uint16_t packet_id, uint8_t *out, size_t out_sz);

// SUBACK. granted_qos gilt für alle angefragten Topics; topic_count muss der
// Anzahl der Topics im SUBSCRIBE entsprechen, sonst verwirft der Controller
// die Antwort.
int mqtt_build_suback(uint16_t packet_id, int topic_count, uint8_t granted_qos,
                       uint8_t *out, size_t out_sz);

// Zählt die Topic-Filter in einem SUBSCRIBE-Payload (nach der Packet-ID).
// Wird für die passende Anzahl Return-Codes im SUBACK gebraucht.
int mqtt_count_subscribe_topics(const uint8_t *payload, size_t payload_len);

// Liest die Packet-ID aus SUBSCRIBE/UNSUBSCRIBE (die ersten zwei Bytes des
// Variable Headers). Gibt 0 zurück, wenn das Paket zu kurz ist.
uint16_t mqtt_packet_id_of(const uint8_t *payload, size_t payload_len);
