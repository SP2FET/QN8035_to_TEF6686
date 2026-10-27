#pragma once
#include "FreeRTOS.h"
extern TickType_t mock_tick;
static inline TickType_t xTaskGetTickCount(void) { return mock_tick; }
#ifdef MOCK_TASK_DELAY_HOOK
void mock_task_delay(TickType_t t);
static inline void vTaskDelay(TickType_t t) { mock_task_delay(t); }
#else
static inline void vTaskDelay(TickType_t t) { mock_tick += t; }
#endif
static inline int xTaskCreate(void (*f)(void *),const char *n,unsigned st,void *a,unsigned p,void *h) { return 1; }
