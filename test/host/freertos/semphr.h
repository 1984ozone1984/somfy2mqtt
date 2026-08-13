#pragma once
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){ return (SemaphoreHandle_t)1; }
static inline int xSemaphoreTake(SemaphoreHandle_t s, unsigned t){ (void)s;(void)t; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t s){ (void)s; return 1; }
