/* Host contract checks for TriviaPOD power, expander, keys and touch.
 * Build from firmware/host_test (no ESP-IDF or attached device required):
 * cc -O2 -std=c11 -DBOARD_TRIVIAPOD -Itriviapod_stubs -I../main -I../../common \
 *   triviapod_host.c ../main/board_triviapod.c -o triviapod_host
 */
#include "host_esp.h"
#include "board_triviapod.h"
#include "battery_port.h"
#include <assert.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
struct host_i2c { int addr; uint8_t reg[256]; };
static struct host_i2c chips[128];
static int64_t clock_us = 1000;
static int pm_failures = 2, pm_writes, failures, lock_depth, notifications;
static unsigned requested_ms, motor_delay;
static uint64_t configured_pins;
static int keys[49];
static void (*motor_fn)(void *);
static jmp_buf motor_return;
static int motor_waits;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
int64_t esp_timer_get_time(void) { return clock_us; }
void vTaskDelay(unsigned ms) { clock_us += ms * 1000LL; if (motor_waits) motor_delay = ms; }
int gpio_config(const gpio_config_t *c) { configured_pins |= c->pin_bit_mask; return ESP_OK; }
int gpio_get_level(int pin) { assert(pin >= 0 && pin < 49); return keys[pin]; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t s, unsigned wait) { (void)s; (void)wait; assert(!lock_depth); lock_depth++; return 1; }
int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; assert(lock_depth == 1); lock_depth--; return 1; }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, int priority, TaskHandle_t *out) {
    (void)name; (void)stack; (void)arg; (void)priority; motor_fn = fn; *out = (void *)2; return pdPASS;
}
int xTaskNotify(TaskHandle_t task, uint32_t value, int action) {
    (void)task; (void)action; requested_ms = value; notifications++; return 1;
}
int xTaskNotifyWait(uint32_t clear, uint32_t exit_bits, uint32_t *out, unsigned wait) {
    (void)clear; (void)exit_bits; (void)wait;
    if (motor_waits++) longjmp(motor_return, 1);
    *out = requested_ms; return 1;
}
int i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *c, i2c_master_dev_handle_t *out) {
    (void)bus; assert(c->device_address < 128); *out = &chips[c->device_address]; (*out)->addr = c->device_address; return 0;
}
int i2c_master_transmit_receive(i2c_master_dev_handle_t d, const void *reg, size_t nreg, void *out, size_t n, int timeout) {
    (void)timeout; assert(nreg == 1); memcpy(out, d->reg + *(const uint8_t *)reg, n); return 0;
}
int i2c_master_transmit(i2c_master_dev_handle_t d, const void *buf, size_t n, int timeout) {
    (void)timeout; const uint8_t *b = buf;
    if (d->addr == 0x6E && b[0] == 9) { pm_writes++; if (pm_failures-- > 0) return -1; }
    if (d->addr == 0x4F && b[0] == 5) assert(lock_depth || !motor_fn);
    memcpy(d->reg + b[0], b + 1, n - 1); return 0;
}
static void mv(int reg, int n) { chips[0x6E].reg[reg] = n & 255; chips[0x6E].reg[reg+1] = n >> 8; }
static int poll(int ms, bool down, int clicks) {
    clock_us += ms * 1000LL; chips[0x6E].reg[0x48] = down; chips[0x6E].reg[0x42] |= clicks;
    return battery_port_key_poll();
}
static void pulse(void) { motor_waits = 0; motor_delay = 0; if (!setjmp(motor_return)) motor_fn(NULL); }
int main(void) {
    memset(keys, 1, sizeof keys);
    chips[0x4F].reg[5] = 1; chips[0x4F].reg[6] = 0x80;
    CHECK(triviapod_init((void *)1));
    CHECK(pm_writes == 3);
    CHECK(clock_us >= 161000);  /* reset delays plus the two 10 ms retries */
    CHECK(configured_pins == ((1ULL << 1) | (1ULL << 2) | (1ULL << 12)));
    CHECK((chips[0x4F].reg[5] & 1) && (chips[0x4F].reg[6] & 0x80));
    CHECK(!(chips[0x4F].reg[6] & 3)); /* motor + amp off at boot */
    CHECK(chips[0x15].reg[0xFE] == 1);
    CHECK(triviapod_amp(true)); CHECK(chips[0x4F].reg[6] & 2);
    triviapod_vibrate(500); CHECK(requested_ms == 120 && notifications == 1);
    pulse(); CHECK(motor_delay == 120 && chips[0x4F].reg[0x1B] == 0 && chips[0x4F].reg[0x1C] == 0);
    CHECK(chips[0x4F].reg[6] & 2); /* motor does not change the amplifier */
    triviapod_panel_sleep(); pulse(); CHECK(motor_delay == 0);
    CHECK(triviapod_panel_reset()); pulse(); CHECK(motor_delay == 120);
    mv(0x22, 3900); mv(0x24, 5000); chips[0x6E].reg[0x12] = 0;
    float frac; bool charging;
    CHECK(battery_port_read(&frac, &charging) && charging && frac > 0 && frac < 1);
    CHECK(battery_port_state() == BAT_CHARGING && battery_port_vbat_mv() == 3900);
    clock_us += 1000001; mv(0x24, 0);
    CHECK(battery_port_read(&frac, &charging) && !charging && battery_port_state() == BAT_ON_BATTERY);
    clock_us += 1000001; mv(0x22, 65535);
    CHECK(!battery_port_read(&frac, &charging) && battery_port_vbat_mv() == 0);
    uint16_t x = 0, y = 0; bool down;
    uint8_t *tp = chips[0x15].reg;
    tp[2] = 1; tp[3] = 1; tp[4] = 100; tp[5] = 0; tp[6] = 200;
    CHECK(triviapod_touch_read(&x, &y, &down) && down && x == 356 && y == 200);
    tp[3] = 2; CHECK(!triviapod_touch_read(&x, &y, &down) && !down && x == 356 && y == 200);
    tp[2] = 0; CHECK(triviapod_touch_read(&x, &y, &down) && !down);
    battery_port_key_init(); CHECK(poll(40, false, 0) == 0);
    CHECK(poll(10, true, 0) == 0); CHECK(poll(40, true, 0) == 0);
    CHECK(poll(10, false, 0) == 0); CHECK(poll(40, false, 0) == 0);
    CHECK(poll(400, false, 1) == 1); CHECK(poll(1000, false, 0) == 0);
    CHECK(poll(1000, false, 1) == 1); /* complete tap between sleep polls */
    CHECK(poll(1000, false, 0) == 0);
    CHECK(poll(10, true, 0) == 0); CHECK(poll(40, true, 0) == 0);
    CHECK(poll(1500, true, 0) == 2); CHECK(poll(100, true, 0) == 0);
    CHECK(poll(10, false, 0) == 0); CHECK(poll(40, false, 0) == 0);
    keys[2] = 0; CHECK(!triviapod_feed_pressed(clock_us)); clock_us += 40000;
    CHECK(!triviapod_feed_pressed(clock_us)); /* held at boot cannot feed */
    keys[2] = 1; CHECK(!triviapod_feed_pressed(clock_us)); clock_us += 40000; CHECK(!triviapod_feed_pressed(clock_us));
    keys[2] = 0; CHECK(!triviapod_feed_pressed(clock_us)); clock_us += 40000; CHECK(triviapod_feed_pressed(clock_us));
    clock_us += 40000; CHECK(!triviapod_feed_pressed(clock_us));
    CHECK(!lock_depth);
    printf("triviapod_host: %s\n", failures ? "FAILED" : "all checks passed");
    return failures != 0;
}
