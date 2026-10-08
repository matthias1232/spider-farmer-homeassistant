#include <string.h>
#include "mqtt_wire_parser.h"

static int decode_remaining_length(const uint8_t *data, size_t len, size_t offset,
                                    uint32_t *value, size_t *new_offset)
{
    uint32_t multiplier = 1;
    uint32_t v = 0;
    while (true) {
        if (offset >= len) return -1; // unvollständig
        uint8_t byte = data[offset++];
        v += (byte & 0x7F) * multiplier;
        multiplier *= 128;
        if (!(byte & 0x80)) break;
        if (multiplier > 128UL * 128 * 128) return -2; // malformed
    }
    *value = v;
    *new_offset = offset;
    return 0;
}

static void encode_remaining_length(uint32_t value, uint8_t *out, int *out_len)
{
    int n = 0;
    do {
        uint8_t digit = value % 128;
        value /= 128;
        if (value > 0) digit |= 0x80;
        out[n++] = digit;
    } while (value > 0);
    *out_len = n;
}

static void parse_publish_fields(mqtt_packet_t *pkt, uint8_t flags,
                                  const uint8_t *raw, size_t raw_len)
{
    pkt->qos = (flags >> 1) & 0x03;
    pkt->retain = (flags & 0x01) != 0;
    size_t p = 0;
    if (raw_len < 2) return;
    uint16_t topic_len = (raw[p] << 8) | raw[p + 1];
    p += 2;
    if (p + topic_len > raw_len) return;
    size_t copy_len = topic_len < sizeof(pkt->topic) - 1 ? topic_len : sizeof(pkt->topic) - 1;
    memcpy(pkt->topic, raw + p, copy_len);
    pkt->topic[copy_len] = '\0';
    p += topic_len;
    if (pkt->qos > 0) {
        if (p + 2 > raw_len) return;
        pkt->packet_id = (raw[p] << 8) | raw[p + 1];
        p += 2;
    }
    pkt->message = raw + p;
    pkt->message_len = raw_len - p;
}

static void parse_connect_fields(mqtt_packet_t *pkt, const uint8_t *raw, size_t raw_len)
{
    // 2 (Name-Länge) + 4 ("MQTT") + 1 (Level) + 1 (Flags) + 2 (Keepalive) = 10
    size_t p = 10;
    if (p + 2 > raw_len) { pkt->client_id[0] = '\0'; return; }
    uint16_t id_len = (raw[p] << 8) | raw[p + 1];
    p += 2;
    if (p + id_len > raw_len) { pkt->client_id[0] = '\0'; return; }
    size_t copy_len = id_len < sizeof(pkt->client_id) - 1 ? id_len : sizeof(pkt->client_id) - 1;
    memcpy(pkt->client_id, raw + p, copy_len);
    pkt->client_id[copy_len] = '\0';
}

int mqtt_parse_packets(const uint8_t *buf, size_t len,
                        mqtt_packet_t *out_packets, int max_packets,
                        size_t *consumed)
{
    int count = 0;
    size_t offset = 0;

    while (offset < len && count < max_packets) {
        size_t start = offset;
        uint8_t first_byte = buf[offset];
        uint8_t packet_type = (first_byte >> 4) & 0x0F;
        uint8_t flags = first_byte & 0x0F;
        offset += 1;

        uint32_t remaining_length;
        size_t new_offset;
        int rc = decode_remaining_length(buf, len, offset, &remaining_length, &new_offset);
        if (rc != 0) {
            // unvollständig oder malformed -> Rest im Puffer lassen
            break;
        }
        offset = new_offset;

        if (offset + remaining_length > len) {
            offset = start; // Paket noch nicht komplett angekommen
            break;
        }

        mqtt_packet_t *pkt = &out_packets[count];
        memset(pkt, 0, sizeof(*pkt));
        pkt->packet_type = packet_type;
        pkt->flags = flags;
        pkt->payload = buf + offset;
        pkt->payload_len = remaining_length;

        if (packet_type == MQTT_PUBLISH) {
            parse_publish_fields(pkt, flags, pkt->payload, remaining_length);
        } else if (packet_type == MQTT_CONNECT) {
            parse_connect_fields(pkt, pkt->payload, remaining_length);
        }

        offset += remaining_length;
        count++;
    }

    *consumed = offset;
    return count;
}

int mqtt_build_publish(const char *topic, const uint8_t *message, size_t message_len,
                        uint8_t qos, bool retain, uint16_t packet_id,
                        uint8_t *out, size_t out_sz)
{
    size_t topic_len = strlen(topic);
    size_t var_header_len = 2 + topic_len + (qos > 0 ? 2 : 0);
    size_t remaining_len = var_header_len + message_len;

    uint8_t rl_bytes[4];
    int rl_len;
    encode_remaining_length((uint32_t)remaining_len, rl_bytes, &rl_len);

    size_t total = 1 + rl_len + remaining_len;
    if (total > out_sz) return -1;

    size_t p = 0;
    uint8_t flags = (qos << 1) | (retain ? 0x01 : 0x00);
    out[p++] = (MQTT_PUBLISH << 4) | flags;
    memcpy(out + p, rl_bytes, rl_len);
    p += rl_len;

    out[p++] = (topic_len >> 8) & 0xFF;
    out[p++] = topic_len & 0xFF;
    memcpy(out + p, topic, topic_len);
    p += topic_len;

    if (qos > 0) {
        out[p++] = (packet_id >> 8) & 0xFF;
        out[p++] = packet_id & 0xFF;
    }

    memcpy(out + p, message, message_len);
    p += message_len;

    return (int)p;
}

// ---------------------------------------------------------------------------
// Antwortpakete für den Local-Only-Betrieb
// ---------------------------------------------------------------------------

int mqtt_build_connack(uint8_t *out, size_t out_sz)
{
    if (out_sz < 4) return -1;
    out[0] = (MQTT_CONNACK << 4);
    out[1] = 0x02;   // Remaining Length
    out[2] = 0x00;   // Connect Acknowledge Flags: Session Present = 0
    out[3] = 0x00;   // Return Code: 0 = Connection Accepted
    return 4;
}

int mqtt_build_pingresp(uint8_t *out, size_t out_sz)
{
    if (out_sz < 2) return -1;
    out[0] = (MQTT_PINGRESP << 4);
    out[1] = 0x00;
    return 2;
}

int mqtt_build_puback(uint16_t packet_id, uint8_t *out, size_t out_sz)
{
    if (out_sz < 4) return -1;
    out[0] = (MQTT_PUBACK << 4);
    out[1] = 0x02;
    out[2] = (packet_id >> 8) & 0xFF;
    out[3] = packet_id & 0xFF;
    return 4;
}

int mqtt_build_suback(uint16_t packet_id, int topic_count, uint8_t granted_qos,
                       uint8_t *out, size_t out_sz)
{
    if (topic_count < 1) topic_count = 1;
    // Remaining Length = 2 (Packet-ID) + ein Return-Code je Topic
    size_t remaining = 2 + (size_t)topic_count;
    uint8_t rl[4];
    int rl_len;
    encode_remaining_length((uint32_t)remaining, rl, &rl_len);

    size_t total = 1 + (size_t)rl_len + remaining;
    if (total > out_sz) return -1;

    size_t p = 0;
    out[p++] = (MQTT_SUBACK << 4);
    memcpy(out + p, rl, rl_len);
    p += rl_len;
    out[p++] = (packet_id >> 8) & 0xFF;
    out[p++] = packet_id & 0xFF;
    for (int i = 0; i < topic_count; i++) {
        out[p++] = granted_qos;   // 0x00/0x01/0x02 = gewährt, 0x80 = abgelehnt
    }
    return (int)p;
}

int mqtt_count_subscribe_topics(const uint8_t *payload, size_t payload_len)
{
    // Aufbau: [Packet-ID 2B] dann je Topic [Länge 2B][Topic][QoS 1B]
    if (payload_len < 3) return 0;
    size_t p = 2;
    int count = 0;
    while (p + 2 <= payload_len) {
        uint16_t tlen = (uint16_t)((payload[p] << 8) | payload[p + 1]);
        p += 2;
        if (p + tlen + 1 > payload_len) break;
        p += tlen + 1;   // Topic + angefragtes QoS-Byte
        count++;
    }
    return count;
}

uint16_t mqtt_packet_id_of(const uint8_t *payload, size_t payload_len)
{
    if (payload_len < 2) return 0;
    return (uint16_t)((payload[0] << 8) | payload[1]);
}
