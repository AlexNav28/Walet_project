# Tests

Every test here is a flashable image, not a host-side unit test. The failures
that matter on this board are electrical and temporal, and neither survives
being mocked. Each image runs on the real hardware and answers one question.

The layout follows the order the board was brought up in: one peripheral at a
time, then all of them together.

```
test/
├── unit/                      one peripheral each, no RTOS
│   ├── uart_loopback/         is the ESP32 UART pin pair good?
│   ├── fingerprint_uart/      FPC2534: enrol, delete, validate
│   ├── imu_i2c/               LSM6DSO: WHO_AM_I, block read, threshold tuning
│   ├── servo_lock/            servo latch: end-stop discovery
│   ├── buzzer/                piezo + N-MOS (historical, see below)
│   └── ble_link/              GATT service alone, for the phone side
└── integration/
    └── full_system_rtos/      all drivers under FreeRTOS, radios excluded
```

## Running them

One PlatformIO environment per image, so exactly one sketch is ever compiled
into a build. `build_src_filter` in `platformio.ini` swaps `src/` out for the
chosen test directory.

```bash
pio run                                          # the shipped firmware
pio run -e test_fingerprint_uart -t upload -t monitor
pio run -e test_imu_i2c -t upload -t monitor
pio run -e test_full_system   -t upload -t monitor
pio run -e test_uart_loopback -e test_imu_i2c    # build several without flashing
```

The fingerprint test and the full firmware run the USB console at 921600 to
match the sensor link; every other test uses 115200. `platformio.ini` sets
`monitor_speed` per environment, so `-t monitor` always picks the right rate.

## What each one covers

| Environment | Directory | Question it answers | Notes |
|---|---|---|---|
| `test_uart_loopback` | `unit/uart_loopback/` | Is the MCU's UART pin pair good? | Jumper TX→RX, send five bytes, check they return. A PASS proved the ESP32 side was fine, which made the unpowered ZIF connector the next suspect. |
| `test_fingerprint_uart` | `unit/fingerprint_uart/` | Does the FPC2534 enrol, list and match? | Interactive menu over the console. The frame synchroniser was developed against this test — see [docs/04](../docs/04-fpc2534-uart-debugging.md). |
| `test_imu_i2c` | `unit/imu_i2c/` | Is the IMU at 0x6A, does `WHO_AM_I` read 0x6C, are the readings physical? | Also the tuning instrument: the `PEAK` line is where `ACCEL_JERK_THRESHOLD` (0.75) and `GYRO_ROTATION_THRESHOLD` (180.0) in `TaskIMUTelemetry` came from. |
| `test_servo_lock` | `unit/servo_lock/` | Does the latch reach both end stops on the reworked GPIO? | Type an angle, the servo goes there. `LOCKED_ANGLE` (180°) and `UNLOCKED_ANGLE` (120°) were found with this. |
| `test_buzzer` | `unit/buzzer/` | Does the piezo drive cleanly through its N-MOS? | Historical. It found the transposed gate/source on rev A; the N-MOS was then removed so the servo could have its GPIO. It cannot pass on the reworked board. |
| `test_ble_link` | `unit/ble_link/` | Does the GATT service advertise, notify and accept commands? | Same UUIDs and byte encodings as the firmware — including all four command opcodes `0x01`–`0x04` — so the phone side can be developed against a device with no sensors attached. Works with nRF Connect. |
| `test_full_system` | `integration/full_system_rtos/` | Do all the drivers coexist under one scheduler? | The real three-task layout and four-state machine, without BLE, WiFi or the phone. Still the fastest way to tell a firmware bug from a radio bug. |

## Why the order matters

Each unit test removes one unknown permanently. By the time the integration
build ran, every driver had been proven alone, so the only new failure mode it
could introduce was interaction: task scheduling, stack sizes, bus ownership.
The problems that showed up there were unambiguously scheduling problems.

The loopback test is the clearest case. When the fingerprint sensor returned
nothing there were four suspects: the sensor, the ribbon, the ZIF connector and
the ESP32's UART pin mapping. One jumper wire and a five-byte round trip
eliminated the fourth and pointed at the connector, which turned out to have no
3V3 on it.

## Two tests kept despite no longer passing cleanly

- `unit/buzzer/` targets hardware that was reworked out of existence. It stays
  because it caught a real footprint bug.
- `integration/full_system_rtos/` still carries the as-designed pin map
  (`SERVO_PIN 0`, `BUZZER_PIN 1`) and has its servo lines commented out, because
  when it was written the motor GPIO rework had not happened yet. It records
  what was proven on the bench on a particular day; refactoring it would remove
  the only reason to keep it.

Both say so in their file headers.

## What this ladder cannot reach

Every image here runs *without the radios*. That is what makes them clean
instruments, and it is also their ceiling: the BLE and WiFi/TLS stacks exist
only in `wallet_firmware`, so every bug involving them survived this whole
ladder — the radios taking the LEDC timer the servo needed, mbedTLS overflowing
a 6 KB task stack, a BLE write landing in a state that was not waiting for it.
Those are in
[docs/05 — System integration debugging](../docs/05-system-integration-debugging.md).

`test_ble_link` is the partial exception and the reason it exists: the one image
that runs the BLE stack, deliberately with nothing else in it.

## Related

- [docs/01 — Firmware architecture](../docs/01-system-architecture.md) — what the shipped image does with these drivers
- [docs/03 — Board bring-up and debugging](../docs/03-board-bringup-and-debugging.md) — the ladder these tests sit at the top of
- [docs/04 — FPC2534 UART debugging](../docs/04-fpc2534-uart-debugging.md) — what `test_fingerprint_uart` was fighting
- [docs/05 — System integration debugging](../docs/05-system-integration-debugging.md) — the bugs that only appear once the radios are in the image
