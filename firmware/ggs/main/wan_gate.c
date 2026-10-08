#include "esp_log.h"

#include "sb_config.h"
#include "provisioning.h"
#include "dns_hijack.h"
#include "mitm_proxy.h"
#include "wan_gate.h"

static const char *TAG = "wan_gate";

static bool s_open = SB_WAN_OPEN_DEFAULT;
static bool s_cloud_forward = SB_CLOUD_FORWARD_DEFAULT;

// Persists one flag without rewriting the whole configuration.
static void persist(const char *key, bool value)
{
    sb_prov_cfg_t prov;
    sb_prov_load(&prov);
    if (key[0] == 'w') prov.wan_open = value;
    else               prov.cloud_forward = value;
    sb_prov_save(&prov);
}

void wan_gate_init(void)
{
    sb_prov_cfg_t prov;
    sb_prov_load(&prov);

    s_open = prov.wan_open;
    s_cloud_forward = prov.cloud_forward;

    napt_set_enabled(s_open);

    ESP_LOGI(TAG, "===============================================");
    if (s_open && s_cloud_forward) {
        ESP_LOGI(TAG, " Internet: ON, cloud mirroring: ON");
        ESP_LOGI(TAG, " The Spider Farmer app works as usual.");
    } else if (!s_open && !s_cloud_forward) {
        ESP_LOGI(TAG, " Internet: OFF, cloud mirroring: OFF");
        ESP_LOGI(TAG, " Fully local — no controller data leaves the network.");
    } else if (s_open && !s_cloud_forward) {
        ESP_LOGI(TAG, " Internet: ON, cloud mirroring: OFF");
        ESP_LOGI(TAG, " The controller can reach the internet, but its MQTT");
        ESP_LOGI(TAG, " session is answered locally and not relayed.");
    } else {
        ESP_LOGW(TAG, " Internet: OFF, cloud mirroring: ON");
        ESP_LOGW(TAG, " The proxy still reaches the cloud over the uplink,");
        ESP_LOGW(TAG, " so controller data continues to leave the network.");
        ESP_LOGW(TAG, " Switch cloud mirroring off as well for local-only.");
    }
    ESP_LOGI(TAG, "===============================================");
}

bool wan_gate_is_open(void)
{
    return s_open;
}

void wan_gate_set(bool open)
{
    if (open == s_open) return;
    s_open = open;
    napt_set_enabled(open);
    persist("wan", open);
    ESP_LOGI(TAG, "Internet access switched %s", open ? "ON" : "OFF");
}

bool wan_gate_cloud_forward(void)
{
    return s_cloud_forward;
}

void wan_gate_set_cloud_forward(bool enabled)
{
    if (enabled == s_cloud_forward) return;
    s_cloud_forward = enabled;
    persist("cloud", enabled);
    ESP_LOGI(TAG, "Cloud mirroring switched %s", enabled ? "ON" : "OFF");

    // Apply it now rather than at some unpredictable later point.
    //
    // Whether the cloud leg is opened is decided once, when a session
    // starts, so a change here used to sit pending until the controller
    // happened to reconnect — which could be minutes, or not until a
    // reboot. Ending the session makes it reconnect within seconds in
    // the new mode, so the switch does what it says immediately.
    //
    // Note this deliberately does not touch the MQTT broker connection:
    // that is the bridge's own uplink to the smart-home system and has
    // nothing to do with the vendor cloud, so Home Assistant keeps
    // working either way.
    mitm_proxy_drop_sessions(enabled ? "cloud mirroring switched on"
                                     : "cloud mirroring switched off");
}
