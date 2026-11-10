# M5Stack TriviaPOD community port

This fork adds the TP-01-A as `triviapod`. It uses the existing round world,
466 × 466 pixels, and a distinct firmware marker so it cannot receive a
Waveshare image as its own update. The original four board configurations
remain available. The port is experimental until the checks below have
been completed on a TriviaPOD.

## Build

The upstream README names ESP-IDF 5.4. This port builds with the
already installed **ESP-IDF 5.5.5**, using its Python 3.14 environment. The
ESP-NOW send callback is selected by IDF version, retaining the 5.4 signature.
No new ESP-IDF installation is required when that environment is available.

Run from `firmware/`:

```sh
../tools/ota_key.sh
idf.py -B build-triviapod -DSDKCONFIG=build-triviapod/sdkconfig \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.triviapod' build
```

The config enables 32 MB flash and 8 MB octal PSRAM. The existing partition
layout is retained in the lower 16 MB, including the model at `0x290000`.
The OTA signing key is local and ignored by git. This community board has
no published OTA manifest; CHECK FOR UPDATES does not change it into a
Waveshare board.

## Hardware

The bring-up reference is the owner's Muse TriviaPOD board port,
`esp32/components/muse/boards/board_m5stack_triviapod.c`, read on 2026-11-10.
It identifies its sources as M5Unified/M5GFX's StopWatch board, the M5PM1
and M5IOE1 drivers, and M5's StopWatch factory firmware. Hardware facts and
register values are adapted to Pocket Tank's ports; no Muse implementation
block is copied verbatim.

| Function | Wiring |
|---|---|
| CO5300 QSPI | CS39, clock40, D0–D3 = 41/42/46/45; x gap 6 |
| Shared I2C | SDA47, SCL48 |
| CST820 touch | 0x15; reset on expander IO4 |
| Panel reset / rail | expander IO5 / IO8 |
| ES8311 audio | 0x18; MCLK18, BCLK17, WS15, ESP output21 |
| Codec rail / amp | expander IO3 / IO10 |
| Motor | expander IO9, PWM1, 2 kHz |
| M5PM1 power | 0x6E; VBAT 0x22, VIN 0x24, key 0x48; active-low IRQ on GPIO12 |
| M5IOE1 expander | 0x4F; output modes 0x03, levels 0x05, open-drain 0x13 |
| Rim keys | blue/right GPIO1, yellow/left GPIO2; active low |

The expander's IO numbers are one-based. Output changes preserve unrelated
pins under a mutex shared by the display, audio and motor paths. Power
register transfers retry three times with 10 ms gaps: the reference board
needed this after a USB reset. The panel uses the StopWatch wake sequence
with brightness held at zero until a complete frame has been sent.

Touch uses CST820's raw 466-pixel reports. It does not inherit the
Waveshare CST9217 calibration or its mirrors. The existing ADVANCED touch
fit and director calibration controls remain available for measurement.

## Controls

- Touch: the usual tank taps, holds, strokes and pages.
- Blue/right: the upstream BOOT control. Double press opens MY TANKS;
  hold five seconds then tap the glass to request a reset, with the
  existing confirmation. On SETTINGS the same chord opens ADVANCED.
- Yellow/left: three pellets at the usual feed spot, once per press,
  while the tank is visible. It does not feed through setup or an open page.
- Power: short click sleeps and wakes; a 1.5-second hold requests shutdown.
  The PMIC retains its factory emergency double-click shutdown. GPIO12's
  active-low click interrupt and the blue key can wake deep sleep.
- Haptics: feeding pellets gives 25 ms, a plant cut 20 ms, CONFIRM 35 ms,
  and the first-run WELCOME 60 ms. Requests are at least 200 ms apart;
  eating, bubbles and letter-wheel ticks produce no vibration.
  Pulse requests are capped at 120 ms; sleep disables further pulses until
  the panel wakes. Failed motor-stop writes are retried with new pulses disabled.

## Current limits

Battery percentage is M5's voltage-derived LiPo estimate, not a fuel-gauge
reading. Charging comes from the charger status input and USB from VIN.
Charge current stays at the factory hardware setting; the AXP2101's
software rail and charge controls do not apply. Audio's codec supply stays
on while the codec is suspended between cues.

BMI270 orientation/motion and the RTC at 0x32 are not brought up by this
port. The picture starts upright. The existing clockless-board path keeps
ESP time through deep sleep and can synchronize over Wi-Fi; time across
a complete power loss needs that synchronization. The mic is not used by
Pocket Tank. Standby current has not been measured; the OLED rail remains
on in sleep, with the panel asleep and touch held in reset.

## Validation

Host-only hardware contracts are checked with no USB device:

```sh
# from firmware/host_test
cc -O2 -std=c11 -Wall -Wextra -Werror -DBOARD_TRIVIAPOD \
  -Itriviapod_stubs -I../main -I../../common \
  triviapod_host.c ../main/board_triviapod.c -o triviapod_host
./triviapod_host
```

These cover transfer retries, unrelated expander pins, motor duration and
sleep gating, raw touch bounds, invalid battery readings, short/long power
presses, clicks between sleep polls, and yellow-key debounce. The existing
host audio and inference harnesses, `make -C sim ARCH= check-all`, and
`tools/board_sheet.py --selftest-card` cover the shared app.

Local validation on 2026-11-10: all five firmware configurations built with
ESP-IDF 5.5.5; all 368 simulator checks (92 per world) passed; host hardware,
audio and inference checks passed. The five card/rename board sheets were
generated and inspected. No serial port or physical device was accessed.
QEMU was not run.

Hardware checks remain pending: full-circle picture and colors, accurate
touch across the panel, both rim controls, sound at multiple volume levels,
battery/USB state, a short motor pulse, and power-key sleep/wake including
the twenty-minute deep-sleep transition. On the shared bench, do not open
a serial port, reset, erase or flash until the device's coordinator grants
exclusive access. Save the current full flash image before the first write.

The active-low wake enum was checked against Espressif's
[IDF 5.4.2 sleep API header](https://github.com/espressif/esp-idf/blob/v5.4.2/components/esp_hw_support/include/esp_sleep.h),
which defines `ESP_EXT1_WAKEUP_ANY_LOW` for the ESP32-S3. An IDF 5.4 build
itself has not been run on this machine.
