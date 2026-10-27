#pragma once
typedef void *esp_timer_handle_t;
typedef struct {void (*callback)(void *);const char *name;} esp_timer_create_args_t;
static inline int esp_timer_create(const esp_timer_create_args_t *a,esp_timer_handle_t *t){*t=(void *)1;return 0;}
static inline int esp_timer_stop(esp_timer_handle_t t){return 0;}
static inline int esp_timer_start_once(esp_timer_handle_t t,unsigned us){return 0;}
static inline int esp_timer_start_periodic(esp_timer_handle_t t,unsigned us){return 0;}
