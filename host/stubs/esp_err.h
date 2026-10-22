#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_ARG 0x102
#ifndef ESP_ERR_NO_MEM
#define ESP_ERR_NO_MEM 0x101
#endif
#ifndef ESP_ERR_INVALID_STATE
#define ESP_ERR_INVALID_STATE 0x103
#endif
#ifndef ESP_ERR_NOT_FOUND
#define ESP_ERR_NOT_FOUND 0x105
#endif
#define ESP_ERR_NOT_SUPPORTED 0x106
static inline const char *esp_err_to_name(esp_err_t code) { return code == ESP_OK ? "ESP_OK" : "error"; }
