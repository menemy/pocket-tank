/* TriviaPOD register/pin reference: Muse board_m5stack_triviapod.c,
 * M5PM1/M5IOE1 and M5GFX Panel_StopWatch. IO numbering starts at one.
 * The expander is shared by the display, audio and motor tasks: every
 * read/modify/write holds the same lock and preserves unrelated pins. */
#include "board_triviapod.h"
#include "battery_port.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

#define IO_AUDIO  (1u << 2)
#define IO_TOUCH  (1u << 3)
#define IO_LCD    (1u << 4)
#define IO_OLED   (1u << 7)
#define IO_MOTOR  (1u << 8)
#define IO_AMP    (1u << 9)
#define IO_USED   (IO_AUDIO | IO_TOUCH | IO_LCD | IO_OLED | IO_MOTOR | IO_AMP)
static const char *TAG = "triviapod";
static i2c_master_dev_handle_t s_pm, s_io, s_tp;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_motor;
static bool s_ready, s_motor_enabled;
static bool s_key_armed, s_key_down, s_key_candidate, s_key_long;
static int64_t s_key_changed, s_key_at;
static unsigned s_clicks;
static float s_fraction;
static int s_mv, s_bat_state = BAT_ON_BATTERY;
static int64_t s_sample_at;
static bool s_sample_ok;

static bool read_reg(i2c_master_dev_handle_t dev, uint8_t reg, void *out, size_t n) {
    if (!dev) return false;
    for (int i = 0; i < 3; i++) {
        if (i2c_master_transmit_receive(dev, &reg, 1, out, n, 50) == ESP_OK) return true;
        if (i < 2) vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}
static bool write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t value, size_t n) {
    if (!dev) return false;
    uint8_t b[] = { reg, value & 255, value >> 8 };
    for (int i = 0; i < 3; i++) {
        if (i2c_master_transmit(dev, b, n + 1, 50) == ESP_OK) return true;
        if (i < 2) vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}
static bool pm_bits(uint8_t reg, uint8_t mask, uint8_t bits) {
    uint8_t v;
    return read_reg(s_pm, reg, &v, 1) && write_reg(s_pm, reg, (v & ~mask) | (bits & mask), 1);
}
static bool io_bits(uint8_t reg, uint16_t mask, uint16_t bits) {
    uint8_t b[2];
    if (!read_reg(s_io, reg, b, 2)) return false;
    uint16_t value = b[0] | (b[1] << 8);
    return write_reg(s_io, reg, (value & ~mask) | (bits & mask), 2);
}
static bool io_level(uint16_t mask, uint16_t bits) {
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = io_bits(0x05, mask, bits);
    xSemaphoreGive(s_lock);
    return ok;
}
static bool add_device(i2c_master_bus_handle_t bus, int address, int hz, i2c_master_dev_handle_t *dev) {
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = address, .scl_speed_hz = hz };
    return i2c_master_bus_add_device(bus, &cfg, dev) == ESP_OK;
}
static void motor_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t ms;
        xTaskNotifyWait(0, UINT32_MAX, &ms, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool on = s_motor_enabled && write_reg(s_io, 0x1B, 0x8999, 2);
        xSemaphoreGive(s_lock);
        if (on) vTaskDelay(pdMS_TO_TICKS(ms > 120 ? 120 : ms));
        bool off = false;
        while (!off) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            off = write_reg(s_io, 0x1B, 0, 2);
            if (!off) s_motor_enabled = false;
            xSemaphoreGive(s_lock);
            if (!off) { ESP_LOGW(TAG, "motor stop failed: retrying, further pulses disabled"); vTaskDelay(pdMS_TO_TICKS(100)); }
        }
    }
}
void triviapod_vibrate(unsigned ms) {
    if (s_motor && ms) xTaskNotify(s_motor, ms > 120 ? 120 : ms, eSetValueWithoutOverwrite);
}
void triviapod_motor_stop(void) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_motor_enabled = false;
    write_reg(s_io, 0x1B, 0, 2);
    xSemaphoreGive(s_lock);
}
bool triviapod_amp(bool on) { return io_level(IO_AMP, on ? IO_AMP : 0); }

bool triviapod_panel_reset(void) {
    if (!io_level(IO_OLED | IO_LCD | IO_TOUCH, IO_OLED)) return false;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (!io_level(IO_LCD | IO_TOUCH, IO_LCD | IO_TOUCH)) return false;
    vTaskDelay(pdMS_TO_TICKS(120));
    if (s_motor) { xSemaphoreTake(s_lock, portMAX_DELAY); s_motor_enabled = true; xSemaphoreGive(s_lock); }
    return triviapod_touch_init();
}
void triviapod_panel_sleep(void) {
    triviapod_motor_stop();
    triviapod_touch_sleep();
    io_level(IO_TOUCH, 0);
}
bool triviapod_touch_init(void) { return write_reg(s_tp, 0xFE, 1, 1); }
bool triviapod_touch_sleep(void) { return write_reg(s_tp, 0xE5, 3, 1); }
bool triviapod_touch_read(uint16_t *x, uint16_t *y, bool *down) {
    uint8_t b[5], reg = 2;
    *down = false;
    if (!s_tp || i2c_master_transmit_receive(s_tp, &reg, 1, b, sizeof b, 20) != ESP_OK) return false;
    if ((b[0] & 15) != 0) {
        uint16_t rx = ((b[1] & 15) << 8) | b[2], ry = ((b[3] & 15) << 8) | b[4];
        if (rx >= R_PANEL || ry >= R_PANEL) return false;
        *x = rx; *y = ry; *down = true;
    }
    return true;
}
bool triviapod_init(i2c_master_bus_handle_t bus) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock || !add_device(bus, 0x6E, 100000, &s_pm) || !add_device(bus, 0x4F, 100000, &s_io) ||
        !add_device(bus, 0x15, 400000, &s_tp)) return false;
    if (!write_reg(s_pm, 0x09, 0, 1) || !write_reg(s_pm, 0x0A, 0, 1) ||
        !pm_bits(0x16, 0xF0, 0) || !pm_bits(0x13, 8, 0) || !pm_bits(0x11, 8, 0) ||
        !pm_bits(0x10, 12, 8) || !pm_bits(0x06, 8, 0) || !pm_bits(0x49, 0x7F, 0x39) ||
        !pm_bits(0x43, 0x1F, 0x1F) || !pm_bits(0x44, 0x3F, 0x3F) || !pm_bits(0x45, 7, 2) ||
        !pm_bits(0x13, 2, 2) || !pm_bits(0x16, 12, 4) ||
        !write_reg(s_pm, 0x40, 0, 1) || !write_reg(s_pm, 0x41, 0, 1) || !write_reg(s_pm, 0x42, 0, 1)) return false;
    if (!write_reg(s_io, 0x23, 0, 1) || !write_reg(s_io, 0x1B, 0, 2) || !io_bits(0x13, IO_USED, 0) ||
        !io_bits(0x05, IO_USED, IO_AUDIO | IO_LCD | IO_TOUCH | IO_OLED) || !io_bits(0x03, IO_USED, IO_USED)) return false;
    if (!triviapod_panel_reset()) return false;
    gpio_config_t keys = { .pin_bit_mask = (1ULL << T_PIN_BLUE) | (1ULL << T_PIN_YELLOW) | (1ULL << T_PIN_PWR_IRQ), .mode = GPIO_MODE_INPUT,
                          .pull_up_en = GPIO_PULLUP_ENABLE };
    if (gpio_config(&keys) != ESP_OK) return false;
    if (write_reg(s_io, 0x25, 2000, 2) && xTaskCreate(motor_task, "motor", 2048, NULL, 5, &s_motor) == pdPASS)
        s_motor_enabled = true;
    s_ready = true;
    ESP_LOGI(TAG, "M5PM1/M5IOE1 ready, motor %s", s_motor ? "ready" : "unavailable");
    return true;
}
bool triviapod_feed_pressed(int64_t now) {
    static bool armed, down, candidate;
    static int64_t changed;
    bool level = !gpio_get_level(T_PIN_YELLOW);
    if (level != candidate) { candidate = level; changed = now; }
    if (now - changed < 30000) return false;
    if (!level) armed = true;
    bool pressed = armed && level && !down;
    down = level;
    return pressed;
}

bool battery_port_init(i2c_master_bus_handle_t bus) { (void)bus; return s_ready; }
bool battery_port_read(float *frac, bool *charging) {
    int64_t now = esp_timer_get_time();
    if (!s_ready) return false;
    if (!s_sample_at || now - s_sample_at >= 1000000) {
        uint8_t vbat[2], vin[2], in;
        s_sample_at = now;
        s_sample_ok = read_reg(s_pm, 0x22, vbat, 2) && read_reg(s_pm, 0x24, vin, 2) && read_reg(s_pm, 0x12, &in, 1);
        if (s_sample_ok) {
            s_mv = vbat[0] | (vbat[1] << 8);
            bool usb = (vin[0] | (vin[1] << 8)) > 4000;
            s_bat_state = usb ? (in & 4 ? BAT_PLUGGED : BAT_CHARGING) : BAT_ON_BATTERY;
            /* M5's voltage-derived LiPo estimate, not a coulomb counter. */
            int pct = (-(int64_t)s_mv * s_mv + 9016LL * s_mv - 19189000) / 10000;
            if (s_bat_state == BAT_PLUGGED && pct >= 95) s_bat_state = BAT_FULL;
            s_fraction = (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100.0f;
            s_sample_ok = s_mv >= 2500 && s_mv <= 4500;
            pm_bits(0x06, 0x10, usb ? 0x10 : 0);
        }
    }
    *frac = s_fraction; *charging = s_bat_state == BAT_CHARGING;
    return s_sample_ok;
}
int battery_port_state(void) { return s_bat_state; }
int battery_port_vbat_mv(void) { return s_sample_ok ? s_mv : 0; }
void battery_port_key_clear(void) { battery_port_key_init(); }
void battery_port_key_init(void) {
    s_key_armed = s_key_down = s_key_candidate = s_key_long = false;
    s_key_changed = esp_timer_get_time(); s_key_at = 0;
    s_clicks = 0;
    write_reg(s_pm, 0x42, 0, 1);
}
int battery_port_key_poll(void) {
    uint8_t state, irq;
    int64_t now = esp_timer_get_time();
    if (!s_ready || !read_reg(s_pm, 0x48, &state, 1)) { s_key_changed = now; s_key_at = 0; return 0; }
    if (read_reg(s_pm, 0x42, &irq, 1) && (irq & 5) && write_reg(s_pm, 0x42, 0, 1))
        s_clicks += (irq & 1 ? 1 : 0) + (irq & 4 ? 2 : 0);
    bool down = state & 1;
    if (down != s_key_candidate) { s_key_candidate = down; s_key_changed = now; }
    if (!s_key_armed) {
        s_clicks = 0;
        if (!down && now - s_key_changed >= 30000) s_key_armed = true;
        return 0;
    }
    int event = 0;
    if (down != s_key_down && now - s_key_changed >= 30000) {
        s_key_down = down;
        if (down) { s_key_at = now; s_key_long = false; }
        else s_key_at = 0;
    }
    if (down && s_key_down && !s_key_at) s_key_at = now;
    if (down && s_key_down && s_key_at && !s_key_long && now - s_key_at >= 1500000) {
        s_key_long = true; event = 2;
    }
    /* M5PM1 latches clicks after release, even between sleep polls.
     * Short presses come only from this latch, never twice from the edges. */
    if (!down && !s_key_down && !event && s_clicks) { s_clicks--; event = 1; }
    return event;
}
bool battery_port_poweroff(void) {
    triviapod_motor_stop();
    if (!s_ready || !write_reg(s_pm, 0x0C, 0xA1, 1)) return false;
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGE(TAG, "M5PM1 shutdown returned with power still on");
    return false;
}
bool battery_port_set_rail(const char *name, bool on) {
    /* Keep AU_L3B powered: the codec state survives the player's idle path.
     * There is no AXP2101 rail map or programmable charge current here. */
    return s_ready && name && !strcmp(name, "aldo1") && (!on || io_level(IO_AUDIO, IO_AUDIO));
}
void battery_port_trim_rails(void) { }
void battery_port_pin_rail(const char *name) { (void)name; }
int battery_port_charge_ma(void) { return 0; }
bool battery_port_set_charge_ma(int ma) { (void)ma; return false; }
bool battery_port_set_cell(int mah) { (void)mah; return false; }
void battery_port_charge_report(void) { ESP_LOGI(TAG, "charge current: factory hardware setting (not software adjustable)"); }
void battery_port_dump(void) {
    float frac; bool charging;
    bool ok = battery_port_read(&frac, &charging);
    ESP_LOGI(TAG, "M5PM1 battery: %s, %d mV, voltage estimate %d%%, state %d", ok ? "ok" : "unavailable", s_mv, (int)(s_fraction * 100), s_bat_state);
}
void battery_port_key_trace(int seconds) {
    for (int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000; esp_timer_get_time() < end;) {
        int key = battery_port_key_poll();
        if (key) ESP_LOGI(TAG, "power key: %s", key == 2 ? "long" : "short");
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
