#ifndef TRIVIAPOD_HOST_ESP_H
#define TRIVIAPOD_HOST_ESP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
typedef int esp_err_t;
typedef void *i2c_master_bus_handle_t;
typedef struct host_i2c *i2c_master_dev_handle_t;
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en; } gpio_config_t;
#define ESP_OK 0
#define I2C_ADDR_BIT_LEN_7 0
#define GPIO_MODE_INPUT 0
#define GPIO_PULLUP_ENABLE 1
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define eSetValueWithoutOverwrite 0
static inline void host_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
#define ESP_LOGI(...) host_log(__VA_ARGS__)
#define ESP_LOGW(...) host_log(__VA_ARGS__)
#define ESP_LOGE(...) host_log(__VA_ARGS__)
int i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t *, i2c_master_dev_handle_t *);
int i2c_master_transmit_receive(i2c_master_dev_handle_t, const void *, size_t, void *, size_t, int);
int i2c_master_transmit(i2c_master_dev_handle_t, const void *, size_t, int);
int gpio_config(const gpio_config_t *);
int gpio_get_level(int);
int64_t esp_timer_get_time(void);
void vTaskDelay(unsigned);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, int, TaskHandle_t *);
int xTaskNotify(TaskHandle_t, uint32_t, int);
int xTaskNotifyWait(uint32_t, uint32_t, uint32_t *, unsigned);
#endif
