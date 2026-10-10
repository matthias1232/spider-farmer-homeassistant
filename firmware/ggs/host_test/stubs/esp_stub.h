// Minimal ESP-IDF stand-ins so main/improv_serial.c compiles and runs on a PC.
// Only what improv_serial.c touches. Not part of the firmware build.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_LOGI(t, f, ...) printf("I %s: " f "\n", t, ##__VA_ARGS__)
#define ESP_LOGW(t, f, ...) printf("W %s: " f "\n", t, ##__VA_ARGS__)
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(a) (int)((a)->addr & 255), (int)(((a)->addr >> 8) & 255), \
                  (int)(((a)->addr >> 16) & 255), (int)(((a)->addr >> 24) & 255)

typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct esp_netif_obj esp_netif_t;
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *i);

typedef struct { const char *version; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
void esp_restart(void);

typedef unsigned TickType_t;
#define pdMS_TO_TICKS(x) (x)
#define pdPASS 1
#define portMAX_DELAY 0
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t t);
void vTaskDelete(void *h);
int xTaskCreate(void (*f)(void *), const char *n, int s, void *a, int p, void *h);

typedef int uart_port_t;
#define UART_NUM_0 0
int uart_write_bytes(uart_port_t p, const char *b, size_t n);
int uart_read_bytes(uart_port_t p, void *b, uint32_t n, TickType_t t);
bool uart_is_driver_installed(uart_port_t p);
esp_err_t uart_driver_install(uart_port_t p, int a, int b, int c, void *d, int e);
esp_err_t uart_driver_delete(uart_port_t p);
