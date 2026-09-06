# Smart Wallet — Biometric Anti-Theft Card Holder

**Custom ESP32-S3 PCB · FreeRTOS Firmware · On-Sensor Biometric Matching · IMU Snatch Detection · BLE Control · Out-of-Band WiFi Alerts**

A physical card wallet engineered to stay locked until it authenticates the owner's fingerprint. It continuously monitors an onboard IMU for the high-acceleration, high-rotation signature of a snatch theft, tracks proximity via BLE connection to the owner's phone, and fires an alert webhook over WiFi if triggered.

Designed and brought up from scratch: state machine planning → component selection → schematic design → 4-layer PCB layout → hand soldering → board bring-up & rework → driver unit tests → multi-threaded FreeRTOS firmware.

---

## Overview

Traditional RFID-blocking wallets only shield against wireless skimming; they offer zero physical defense if snatched off a table or pulled from a pocket. Bluetooth tags notify you long after the wallet is gone, but do not stop anyone from opening it immediately.

This project secures the physical access layer:
* Cards are secured behind a micro-servo latch mechanism that only retracts upon an authenticated fingerprint match or an authenticated BLE override.
* An onboard 6-DoF IMU continuously samples motion to detect snatch signatures in real time.
* If a theft event occurs or the BLE link drops while armed, the wallet dispatches an alert over WiFi via an out-of-band Discord webhook.

---

## Hardware Specifications & Architecture

| Category | Component / Choice | Design Rationale |
|---|---|---|
| **MCU** | ESP32-S3-MINI-1 | Dual-core processing to separate timing-critical tasks from radio stacks; built-in BLE 5.0 and 2.4 GHz Wi-Fi. |
| **Fingerprint Module** | SparkFun FPC2534 (UART @ 921600)[cite: 1, 2] | On-sensor template storage and 1:N matching (`ID_TYPE_ALL`)[cite: 1]. Raw fingerprint image data never leaves the sensor module[cite: 1]. Chosen over SPI to minimize bus trace congestion and preserve MCU deep-sleep options[cite: 2]. |
| **Motion Tracking** | LSM6DSO 6-DoF IMU (I2C)[cite: 2] | Continuous 104 Hz sampling. Used for threshold-based theft and snatch detection while keeping pin count low[cite: 2]. |
| **Physical Latch** | Micro Servo (PWM via Core 1) | 180° locked / 120° retracted stroke. Automatically re-locks after a 5-second window. |
| **PCB Layout** | 4-Layer Custom Board (KiCad)[cite: 2] | Designed with tight decoupling capacitor placement per manufacturer layout guidelines, minimized trace lengths between the MCU and sensors to drop high-frequency bus noise, and internal ground/power planes[cite: 2]. |

---

## Features

| | |
|---|---|
| **Physical lock** | Micro servo latch, 180° locked / 120° open, 800 ms travel, auto-relock after 5 s. |
| **On-sensor matching** | FPC2534 armed in the background with `requestIdentify(ID_TYPE_ALL)` whenever locked. Templates and the match decision stay on the sensor — no fingerprint image crosses the UART or reaches the phone. |
| **Snatch detection** | LSM6DSO 6-DoF IMU at 104 Hz, polled every 10 ms. Theft is declared only when jerk > 0.75 g **and** rotation > 180 °/s in the same sample. Walking gives one, turning gives the other, a grab gives both. |
| **Phone control** | Using nRF Connect app set up four BLE opcodes: multi-touch enrolment, wipe all templates, list the database, manual latch override. No on-device enrolment path. |
| **Lockout handling** | Five failed scans put the FPC2534 into a 15 s internal lockout. The firmware mirrors it with a non-blocking deadline, suppresses re-arming, then aborts and resyncs. |
| **Proximity** | A BLE disconnect means the wallet and its owner have separated. |
| **Out-of-band alerts** | Using Discord webhook POST over WiFi on its own task, so a slow handshake never delays a lock. Fires on disconnect, tamper alarm, manual override and biometric lockout. |

---

## Documentation

| | |
|---|---|
| **[1. Firmware architecture](docs/01-system-architecture.md)** | States, tasks, event bits, BLE protocol, theft rule, known gaps. |
| **[2. Hardware design](docs/02-hardware-design.md)** | Components, bus choices, PCB layout, power tree, pin map as built. |
| **[3. Board bring-up](docs/03-board-bringup-and-debugging.md)** | The bring-up ladder and the three hardware faults this board shipped with. |
| **[4. FPC2534 UART debugging](docs/04-fpc2534-uart-debugging.md)** | Error 33 — the bug that blocked the whole product. |
| **[Test catalogue](test/README.md)** | The seven flashable test images and what each proves. |

---

## System architecture

```
        core 1 (application)                    core 0 (shared with the radios)
  +------------------------------+        +------------------------------------+
  |  SysManager   prio 2   4 KB  |        |  IMU_Track   prio 2   4 KB         |
  |  the only writer of state    |        |  polls the IMU every 10 ms         |
  |                              |        |                                    |
  |  BioAuth      prio 2   8 KB  |        |  NetAlert    prio 1   6 KB         |
  |  owns Serial1 exclusively    |        |  WiFi + TLS webhook POST           |
  +------------------------------+        +------------------------------------+
                    |                                        |
                    +------------------+---------------------+
                                       v
                    systemEvents       (event group, 4 bits)
                    bleCommandQueue    (BLE opcodes,   depth 5)
                    alertMessageQueue  (alert strings, depth 4)
```

```
  LOCKED --finger--> AUTH_VERIFYING --match--> UNLOCKED --5 s--> LOCKED
     ^  ^                  |                       |
     |  +--- BLE 0x04 -----+                       |
     |                                             |
     +--- TAMPER_ALARM <---- theft signature <-----+
```

Three rules hold it together:

- **One writer.** `currentState` is written by `TaskSystemManager` and nothing
  else. Every other task raises a bit and lets the manager decide. No mutexes.
- **Primitives, not shared memory.** Tasks talk through one event group and two
  queues. No task dereferences another task's data.
- **Latency-sensitive work on core 1.** The state machine and the UART driver
  are pinned away from the radio stacks on core 0.

Details: [docs/01](docs/01-system-architecture.md).

---

## BLE interface

```
service  4FAFC201-1FB5-459E-8FCC-C5C9C331914B   advertised as "SmartWallet"
  |-- STATUS   beb5483e-...-26a8   READ | NOTIFY   one status byte
  +-- COMMAND  beb5483e-...-26a9   WRITE           one opcode byte
```

We use the app of nRF Connect by Nordic Semiconductor to test the funcionality of the commands

| Command | | Status | |
|---|---|---|---|
| `0x01` | start multi-touch enrolment | `0x00` | clear |
| `0x02` | delete all templates | `0x01` | wrong finger |
| `0x03` | list the template database | `0x02` | motion / theft detected |
| `0x04` | manual latch override | `0x04` | enrolment in progress |

The write callback queues the opcode and returns; the BLE callback context never
touches the UART. Same UUIDs and encodings as `test/unit/ble_link/`, so the
phone side can be developed against a board with no sensors attached.

---

## Implementation process

```
problem research -> state machine -> schematic -> 2-layer PCB -> fabrication
   -> bring-up ladder (multimeter, then one unit test per peripheral)
   -> integration build under FreeRTOS, no radios
   -> full firmware with BLE + WiFi
```

The state machine came before the schematic, so every component on the board
exists because a transition needed it. Nothing was flashed until the board
passed a fixed sequence of electrical checks — two of the three board faults
were found with a multimeter before a line of firmware ran.

---

## Repository layout

```
├── src/
│   └── main.cpp            the whole firmware: config, BLE, callbacks,
│                           setup(), and the four FreeRTOS tasks
│
├── include/
│   └── secrets.example.h   template for the untracked include/secrets.h
│
├── lib/
│   └── SparkFun_FPC2534/   vendored, not a registry dependency — it is patched
│
├── test/                   seven flashable images; see test/README.md
│   ├── unit/               one peripheral each, no RTOS
│   └── integration/        all drivers together under FreeRTOS, no radios
│
├── docs/                   the reasoning, and the hardware images
└── platformio.ini          one environment per image
```

- `src/main.cpp` is one translation unit on purpose. Every hard bug on this
  project was an *interaction* between subsystems, and keeping the whole of it
  on one screen is what made those visible.
- `lib/SparkFun_FPC2534/` is a fork, not a dependency — the frame synchroniser
  from [docs/04](docs/04-fpc2534-uart-debugging.md) lives inside it.
- `test/integration/full_system_rtos/` is left as written, as-designed pin map
  and all. It proved the concurrency worked before connectivity was added.

---

## Building it

Requires [PlatformIO](https://platformio.org/). The vendored library is already
in `lib/`.

```bash
git clone <this repo>
cd Walet_project

cp include/secrets.example.h include/secrets.h      # untracked; fill in your own

pio run                                             # build the firmware
pio run -e wallet_firmware -t upload -t monitor     # flash it and watch
pio run -e test_imu_i2c -t upload -t monitor        # flash one unit test
```

`main.cpp` includes `secrets.h` unconditionally, so that copy step is required
before the firmware compiles. It needs three defines:

```c
#define WIFI_SSID        "your-network"
#define WIFI_PASS        "your-password"
#define DISCORD_WEBHOOK  "https://discord.com/api/webhooks/..."
```

---

### Personal Contributions

- **Problem and parts research** — what existing anti-theft wallets do not
  solve, and components that fit an IoT form factor.
- **Prototyping** — Testing sessors and funcionality with the constrains of the space
- **Hardware Design & Layout:** — Formulated component constraints for ultra-compact IoT integration, drew schematics, and routed the 2-layer PCB in KiCad.
- **PCB soldering and debugging** — Hand-soldered IC packages and passives, diagnosed routing defects with multimeters including the three rework fixes in [docs/03](docs/03-board-bringup-and-debugging.md).
- **Firmware testing** — the unit test per driver, the integration build, and
  the bring-up ladder they sit at the top of.
- **Firmware development** — the FreeRTOS architecture, the drivers, the state
  machine, the FPC2534 frame synchroniser, the BLE command protocol, the IMU
  snatch rule, and the alert path.
- **System integration debugging** — the seven cross-component bugs in
  [docs/05](docs/05-system-integration-debugging.md), which is where most of the
  firmware time after "each driver works" went.

**Team:** Alexis Navarrete · Aiden Krueger · Erictuan Nong
**Course:** ECE 196, UC San Diego, Spring 2026

---

## Status

Working prototype, demonstrated end to end: enrolment, matching and template
management from the phone, servo lock and auto-relock, manual BLE override, IMU
snatch detection, BLE status notifications, and the WiFi webhook alert path.

Known gaps: [docs/01 §1.8](docs/01-system-architecture.md#18-known-gaps). The
most serious is that the BLE COMMAND characteristic is plain `PROPERTY_WRITE`
with no bonding, so any device in range can erase every template or open the
latch — closing it needs a coordinated change with the companion app.

Three fixes in [docs/05](docs/05-system-integration-debugging.md) are diagnosed
but not yet in `src/main.cpp`; §5.0 says which. Hardware fixes for a rev B are
in [docs/02 §2.8](docs/02-hardware-design.md#28-what-a-rev-b-would-change).
