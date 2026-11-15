#pragma once
#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
#ifdef DGX_HOST_WINDOW
/* a program with the window on the computer tells what it does, as on a board */
#define ESP_LOGI(tag, fmt, ...) fprintf(stderr, "I %s: " fmt "\n", tag, ##__VA_ARGS__)
#else
#define ESP_LOGI(tag, fmt, ...) ((void)(tag))
#endif
#define ESP_LOGD(tag, fmt, ...) ((void)(tag))
#define ESP_LOGV(tag, fmt, ...) ((void)(tag))
/* a dump of bytes at a level: nothing on the computer */
#define ESP_LOG_DEBUG 4
#define ESP_LOG_BUFFER_HEXDUMP(tag, buffer, len, level) ((void)(tag), (void)(buffer), (void)(len))
