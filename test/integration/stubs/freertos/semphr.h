#pragma once
#include "FreeRTOS.h"
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
static inline int xSemaphoreTake(SemaphoreHandle_t s,TickType_t t) { return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t s) { return 1; }
