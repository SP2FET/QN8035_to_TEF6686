#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
#define pdMS_TO_TICKS(x) (x)
#define portTICK_PERIOD_MS 1
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdPASS 1
