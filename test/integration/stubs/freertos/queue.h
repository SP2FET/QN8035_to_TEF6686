#pragma once
#include "FreeRTOS.h"
static inline QueueHandle_t xQueueCreate(unsigned n,unsigned size) { return (void *)1; }
int xQueueSend(QueueHandle_t q,const void *p,TickType_t t);
static inline int xQueueReceive(QueueHandle_t q,void *p,TickType_t t) { return 0; }
