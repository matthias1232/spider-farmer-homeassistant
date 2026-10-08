idf_component_register(
    SRCS
        "app_main.c"
        "wifi_apsta.c"
        "dns_hijack.c"
        "wan_gate.c"
        "mqtt_wire_parser.c"
        "mitm_proxy.c"
        "ha_mqtt.c"
        "provisioning.c"
        "config_portal.c"
        "live_log.c"
        "device_cache.c"
        "device_registry.c"
        "sys_log.c"
        "syslog_fwd.c"
        "sf_alarm.c"
        "ggs_ble.c"
        "time_sync.c"
        "ota_update.c"
        "status_page.c"
        "mqtt_send.c"
        "sb_system.c"
        "mac_filter.c"
        "dhcp_leases.c"
        "ip_filter.c"
        "network_page.c"
        "tz_table.c"
        "ntp_server.c"
        "nav.c"
        "sf_normalizer.c"
        "sf_command_handler.c"
        "ha_discovery_table.c"
        "ha_discovery_stale.c"
        "config_poll.c"
        "web_auth.c"
        "control_page.c"
        "improv_serial.c"
        "plan_text.c"
        "plan_templates.c"
        "plan_store.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES
        "../certs/sf_ca.pem"
        "../certs/proxy_cert.pem"
        "../certs/proxy_key.pem"
    REQUIRES
        esp_wifi esp_netif esp_event nvs_flash mbedtls
        mqtt spi_flash json
        lwip esp_timer
        esp_http_server esp-tls esp_app_format
        app_update esp_https_ota esp_http_client
        bt esp_driver_uart
)
