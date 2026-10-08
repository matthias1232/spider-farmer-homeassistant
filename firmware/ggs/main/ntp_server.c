#include <string.h>
#include <time.h>
#include <sys/time.h>

#include <lwip/sockets.h>
#include <lwip/netdb.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sb_config.h"
#include "ntp_server.h"

static const char *TAG = "ntp_server";

// NTP counts seconds from 1900; time() counts from 1970.
#define NTP_EPOCH_OFFSET 2208988800ULL

// Just enough of the packet to answer a client. A full implementation
// would track dispersion and jitter; a client only needs a plausible
// timestamp and a stratum that says where it came from.
typedef struct __attribute__((packed)) {
    uint8_t  li_vn_mode;
    uint8_t  stratum;
    uint8_t  poll;
    int8_t   precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint32_t ref_ts_sec;
    uint32_t ref_ts_frac;
    uint32_t orig_ts_sec;
    uint32_t orig_ts_frac;
    uint32_t rx_ts_sec;
    uint32_t rx_ts_frac;
    uint32_t tx_ts_sec;
    uint32_t tx_ts_frac;
} ntp_packet_t;

// Current time as an NTP timestamp.
static void now_ntp(uint32_t *sec, uint32_t *frac)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    *sec = htonl((uint32_t)(tv.tv_sec + NTP_EPOCH_OFFSET));
    // Microseconds scaled to the 32-bit fraction NTP uses.
    *frac = htonl((uint32_t)((double)tv.tv_usec * 4294.967296));
}

static void ntp_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Could not create the NTP socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(123),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Could not bind port 123");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Serving time on port 123");

    ntp_packet_t pkt;
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);

        int n = recvfrom(sock, &pkt, sizeof(pkt), 0,
                         (struct sockaddr *)&from, &from_len);
        if (n < (int)sizeof(pkt)) continue;

        // Keep the client's transmit timestamp: it goes back as the
        // originate timestamp, which is how the client matches the
        // reply to its request.
        uint32_t orig_sec = pkt.tx_ts_sec;
        uint32_t orig_frac = pkt.tx_ts_frac;

        uint32_t rx_sec, rx_frac;
        now_ntp(&rx_sec, &rx_frac);

        time_t now = time(NULL);
        // Before the bridge's own first sync there is no useful time to
        // give. Saying so is better than handing out 1970, which a
        // client would either reject or, worse, believe.
        bool synced = now > 1700000000;

        memset(&pkt, 0, sizeof(pkt));
        // LI 0 (or 3 = unsynchronised), version 4, mode 4 (server).
        pkt.li_vn_mode = synced ? (0 << 6 | 4 << 3 | 4)
                                : (3 << 6 | 4 << 3 | 4);
        // Stratum 2: the bridge is one hop from a real time source.
        pkt.stratum = synced ? 2 : 0;
        pkt.poll = 6;
        pkt.precision = -20;           // about a microsecond
        pkt.root_delay = htonl(0);
        pkt.root_dispersion = htonl(0x0100);
        memcpy(&pkt.ref_id, "SBRG", 4);

        pkt.ref_ts_sec = rx_sec;
        pkt.ref_ts_frac = rx_frac;
        pkt.orig_ts_sec = orig_sec;
        pkt.orig_ts_frac = orig_frac;
        pkt.rx_ts_sec = rx_sec;
        pkt.rx_ts_frac = rx_frac;

        // Via locals: taking the address of a packed member can yield an
        // unaligned pointer, which is undefined behaviour on this
        // architecture even where it happens to work.
        uint32_t tx_sec, tx_frac;
        now_ntp(&tx_sec, &tx_frac);
        pkt.tx_ts_sec = tx_sec;
        pkt.tx_ts_frac = tx_frac;

        sendto(sock, &pkt, sizeof(pkt), 0,
               (struct sockaddr *)&from, from_len);
    }
}

void ntp_server_start(void)
{
    xTaskCreate(ntp_task, "ntp_server", 3072, NULL, 4, NULL);
}
