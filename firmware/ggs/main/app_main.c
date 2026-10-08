#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

#include "sb_config.h"
#include "provisioning.h"
#include "wifi_apsta.h"
#include "dns_hijack.h"
#include "wan_gate.h"
#include "ha_mqtt.h"
#include "mitm_proxy.h"
#include "config_portal.h"
#include "live_log.h"
#include "device_cache.h"
#include "device_registry.h"
#include "sys_log.h"
#include "syslog_fwd.h"
#include "ggs_ble.h"
#include "time_sync.h"
#include "ntp_server.h"
#include "ota_update.h"
#include "mac_filter.h"
#include "dhcp_leases.h"
#include "ip_filter.h"
#include "sf_command_handler.h"
#include "improv_serial.h"

static const char *TAG = "app_main";

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "SpiderBridge-ESP32 v2 %s starting (IDF %s)",
             app->version, app->idf_ver);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // A requested GGS setup over Bluetooth runs in a boot of its own: no
    // Wi-Fi, no TLS, nothing else -- Bluetooth gets all the memory, does
    // its job and restarts the bridge into normal operation. On a normal
    // boot this releases every byte Bluetooth would have reserved.
    if (ggs_ble_boot_check()) {
        sys_log_init();
        device_registry_init();   // names and the account uid for binding
        ggs_ble_run_boot();       // does not return
    }

    // Log and caches first, so the web interface has something to show
    // even if a later step fails.
    // First, so the lines the rest of startup produces are captured and
    // visible in the web interface.
    sys_log_init();

    live_log_init();
    device_cache_init();
    // Before anything that publishes: the registry holds the names that
    // every MQTT topic is built from.
    device_registry_init();

    // Access rules before the radio and the web server come up, so a
    // blocked client is refused from the first association rather than
    // after a brief window of being allowed.
    mac_filter_init();
    dhcp_leases_init();
    ip_filter_init();

    // 1) Networking: AP (controller hotspot) + optional STA (uplink)
    wifi_apsta_init();

    // Remote syslog only now: it opens sockets, which need the network
    // stack wifi_apsta_init() just created. Lines logged before this are
    // already in the ring and are not resent.
    syslog_fwd_start();

    // 2) Configuration portal, always running on the AP interface.
    //
    // If it does not come up, no OTA can ever reach this firmware: restart
    // without confirming it, so the bootloader rolls back to the previous
    // image after its retry limit (1.13.0 shipped exactly that way).
    if (!config_portal_start()) {
        ESP_LOGE(TAG, "Web server failed -- restarting without confirming this firmware");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    // Wi-Fi setup from the web installer over USB (Improv serial), like
    // Tasmota. Started before the "not configured" early return below.
    improv_serial_start();


    // Confirms this boot to the bootloader's rollback mechanism.
    //
    // Must happen even in the unconfigured path below (no WiFi set up
    // yet) — that is the normal state right after a factory reset or a
    // first flash, not a failure, and the device must still accept a
    // corrective OTA in that state. The web server being up is what
    // actually matters for that, so this is placed right after it
    // starts rather than at the end of app_main(), which the early
    // return a few lines down would skip entirely.
    //
    // Without this call, CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE arms but
    // is never satisfied, and every OTA would revert on its own retry
    // limit regardless of whether the new firmware was actually fine —
    // the opposite problem from having no rollback at all.
    esp_ota_mark_app_valid_cancel_rollback();

    // On the heap: this function stays on the stack for the whole of
    // startup, and the struct is close to a kilobyte. Keeping it here
    // once overflowed the main task inside wifi_apsta_init.
    sb_prov_cfg_t *prov = malloc(sizeof(*prov));
    if (!prov) {
        ESP_LOGE(TAG, "Out of memory loading the configuration");
        return;
    }
    sb_prov_load(prov);

    // Without home network credentials there is no uplink, no cloud
    // mirror, no broker. Stay in portal mode rather than starting
    // subsystems that would only fail in a loop.
    if (prov->sta_ssid[0] == '\0') {
        ESP_LOGW(TAG, "========================================================");
        ESP_LOGW(TAG, " Not configured yet.");
        ESP_LOGW(TAG, " Connect to the '%s' hotspot and open", prov->ap_ssid);
        ESP_LOGW(TAG, " http://%s/ in a browser.", prov->ap_ip);
        ESP_LOGW(TAG, "========================================================");
        free(prov);
        return;
    }

    // 3) Wait for the uplink — the cloud mirror and NAT both need it.
    ESP_LOGI(TAG, "Waiting for uplink connection to '%s'...", prov->sta_ssid);
    if (!wifi_apsta_wait_uplink(30000)) {
        ESP_LOGW(TAG, "No uplink after 30 s — continuing anyway, Wi-Fi keeps retrying");
    }

    // 4) Name and time service for hotspot clients. Configured before the
    //    DNS proxy starts so the first lookup already gets the right answer.
    dns_set_target(prov->dns_target, prov->dns_redirect);
    dns_set_ntp_target(prov->ntp_target, prov->ntp_redirect);
    dns_set_offline_exceptions(prov->allow_dns_offline, prov->allow_ntp_offline);

    // 5) Internet access. Applies NAT according to the stored setting.
    wan_gate_init();

    // 6) DNS proxy: redirects the cloud hostname to us, optionally
    //    redirects time servers, enforces the offline exceptions.
    //    Runs regardless of the WAN gate — the controller must always be
    //    able to resolve the cloud hostname, otherwise it cannot reach
    //    the bridge at all.
    dns_hijack_start();

    // 7) Clock. Before MQTT and the proxy, because TLS rejects every
    //    certificate while the device still believes it is 1970, and
    //    because schedule times are only meaningful in the right zone.
    time_sync_start();

    //    And serve that time to hotspot clients.
    //
    //    The DNS hijack can point a client's time server at the bridge,
    //    and without something listening the client never sets its
    //    clock. A controller with no clock cannot complete a TLS
    //    handshake, and the symptom is baffling: it associates, gets an
    //    address, resolves the cloud hostname, then never connects.
    //
    //    Only when that redirect is on: without it no client is ever sent
    //    here, and the server's task (3 KB) would idle forever.
    if (prov->ntp_redirect) ntp_server_start();

    // 8) Home Assistant connection
    bool has_broker = (prov->ha_mqtt_uri[0] != '\0');
    // Everything from here on reads its own configuration, so the copy
    // is no longer needed and should not stay resident.
    free(prov);
    prov = NULL;

    if (has_broker) {
        ha_mqtt_start();
    } else {
        ESP_LOGW(TAG, "No MQTT broker configured — MQTT stays off");
    }

    // 9) The TLS MITM proxy for the grow controller
    mitm_proxy_start();

    // 10) Firmware update check. Waits for the uplink on its own task,
    //     so a slow or absent network does not hold up startup.
    ota_remote_start();

    // 10b) Clock sync to the controllers, when that is enabled. Also a
    //      task: it keeps re-applying the setting, so a zone changed
    //      from the vendor app comes back within one interval.
    sf_timezone_sync_start();

    // 11) Connection watchdog. The Wi-Fi driver and the MQTT client both
    //    retry on their own, but neither recovers from every failure
    //    mode. This device runs unattended, so a stuck uplink has to
    //    heal without someone walking over to it.
    wifi_apsta_start_watchdog();

    ESP_LOGI(TAG, "All subsystems started. Free internal heap: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
