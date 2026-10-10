#pragma once
#include "esp_stub.h"
#define GGS_BLE_MAX_FOUND 8
typedef enum { GGS_JOB_NONE = 0, GGS_JOB_PENDING = 1, GGS_JOB_OK = 2, GGS_JOB_FAILED = 3 } ggs_job_t;
typedef struct { char addr[18]; uint8_t addr_type; char name[24]; int8_t rssi; char wifi_mac[13]; uint16_t pcode; uint8_t flags; bool has_flags; } ggs_ble_dev_t;
typedef struct { bool pending; bool scanned; int count; ggs_ble_dev_t dev[GGS_BLE_MAX_FOUND]; char busy_addr[18]; char last_result[160]; char trace[512]; uint8_t job; char job_addr[18]; } ggs_ble_status_t;
void ggs_ble_get_status(ggs_ble_status_t *out);
bool ggs_ble_request_scan(uint32_t delay_ms);
bool ggs_ble_request_provision(const char *addr, bool bind, uint32_t delay_ms);
