# 4. FPC2534 UART Debugging — Error 33

The hardest bug in the project, and the one that blocked everything else. The
sensor was correctly wired, powered and configured, and returned
`FPC_RESULT_IO_BAD_DATA` forever. Four attempted fixes made it worse before the
cause turned up.

| | |
|---|---|
| Board | ESP32-S3-MINI-1, custom PCB rev A |
| Link | UART1, 921600 baud, `SERIAL_8N1`, pins TX 17 / RX 18 |
| `RST_N`, `CS_N`/`SYS_WU` | strapped 3V3 — **no hardware reset available**, part never sleeps |
| Library | SparkFun FPC2534, vendored in `lib/` |

Two things were established before any debugging: the pin configuration is what
the datasheet asks for in UART mode, and the baud rate is right — 115200
produces *no data at all*, 921600 produces data. A wrong baud rate produces
garbage, not silence.

---

## 4.1 What error 33 actually means

| Code | Name | Meaning |
|---|---|---|
| 33 | `FPC_RESULT_IO_BAD_DATA` | frame header validation failed |
| 35 | `FPC_RESULT_IO_NO_DATA` | not enough bytes available |

It comes from exactly one place — the 8-byte header check in
`sfDevFPC2534::processNextResponse()`, which requires `version == 0x0004`, the
`0x0040` app-sender flag, and a type of `0x12` or `0x13`.

So error 33 says one thing: *these eight bytes are not the start of a valid
application frame.* It does not say the sensor is broken, absent or
misconfigured. Reading the error's definition rather than guessing at it is what
turned this from a hardware hunt into a parser bug.

---

## 4.2 The symptom log

| Attempt | Change | Result |
|---|---|---|
| 0 | as written: 200 ms service interval | error 33 flooding, occasional "device ready" slipping through |
| 1 | service every 1 ms, RX ring 512 B → 2 KB | error changed to **35** — progress, different bug |
| 2 | `readBytes()` with a timeout instead of an instant `available()` check | partial headers now wait for the rest of themselves |
| 3 | flush the buffer on a bad header | **no output at all — worse** |
| 4 | hex-dump the bytes that fail validation | solved it |

**Attempt 0** was arithmetic. At 921600 baud the sensor delivers ~92 KB/s. A
200 ms gap between service calls means ~18,400 bytes arriving into a 512-byte
ring. The ring overflows, bytes are dropped mid-frame, and the parser reads from
an arbitrary offset — which is exactly what a header-validation failure looks
like.

**Attempt 3 is the instructive failure.** The flush was discarding the sensor's
boot status message, which the FPC2534 sends *exactly once*. Flush it and the
sensor sits idle waiting for a command the host will never send, because it
never saw the sensor come up. The correct ordering — drain *before* a reset,
never after — is the whole of `software_reset_sensor()`:

```c
void software_reset_sensor(void) {
    while (Serial1.available()) { Serial1.read(); }   // drain first
    mySensor.sendReset();                             // then reset
    vTaskDelay(pdMS_TO_TICKS(300));                   // then let it come back up
}
```

**Attempt 4** printed what was actually on the wire instead of reasoning about
what should be:

```
00 78 3A 3A 3A 3A 3A 3A
```

`0x78` is ASCII `x`, `0x3A` is `:`. That is not a malformed binary frame — it is
text.

---

## 4.3 Root cause

The FPC2534 boots in two stages that speak different languages on the same wire:

```
   power-on
      |
      v
   BOOTLOADER STAGE  ---->  ASCII diagnostic text   ("x::::::")   <-- not protocol
      |
      v
   APPLICATION STAGE ---->  binary frames           (version 0x0004, flags 0x0040)
```

The parser assumes the stream begins on a frame boundary. It reads the first 8
bytes it finds, gets bootloader ASCII, and fails with error 33. The error
handler then flushes the whole buffer — including the valid binary frame sitting
behind the text, and the one-and-only "ready" message. The host then waits
forever.

A stream that starts with text, a parser that assumes it starts with binary, and
an error path that destroys the evidence.

**A second discovery:** the library's `begin()` never talks to the sensor. It
stores a pointer and returns `true`, always. "FPC2534 initialized" in the log
means "a `HardwareSerial*` was saved", nothing more. Hours were spent trusting
that line. An API that returns `bool` is not evidence — the first byte the
peripheral sends back is.

---

## 4.4 The fix — frame synchronisation

Instead of trusting the stream to start on a boundary, scrape it for one. A
valid application frame must begin with `0x04` (little-endian `0x0004` version
field), so discard bytes until the next byte is `0x04`, then attempt a header
read. In `lib/SparkFun_FPC2534/src/sfTk/sfDevFPC2534.cpp`:

```cpp
uint16_t discarded = 0;
const uint16_t kMaxSyncDiscard = 1024;

while (Serial1.available() > 0 && Serial1.peek() != 0x04 && discarded < kMaxSyncDiscard)
{
    Serial1.read();   // drop exactly one byte of verified garbage
    discarded++;
}
```

Four properties make this work where flushing did not:

1. **One byte at a time**, so it can never step over a valid frame sitting
   behind the garbage. Flushing threw away the answer with the noise.
2. **Peek before read.** Nothing is consumed unless it is known not to be a
   frame start.
3. **The discard is bounded**, so a wire stuck at a non-`0x04` level cannot spin
   the loop forever.
4. **A failed header validation advances exactly one byte**, not a flush. A
   `0x04` can legitimately appear inside a payload, so a resync can land on a
   false start; advancing one byte lets the parser walk forward to the real one.

Two hardening changes went in alongside: the header check now also requires
`payload_size <= MAX_HOST_PACKET_SIZE_DEFAULT`, so a corrupt length cannot make
the driver wait for a nonsense number of bytes; and after a valid header the
parser waits up to 1500 ms for the payload rather than assuming it is already
buffered, so a truncated frame fails cleanly instead of hanging the task.

---

## 4.5 Files touched

| File | Change |
|---|---|
| `lib/.../sfDevFPC2534.cpp` | frame synchroniser, payload-size validation, bounded payload wait, single-byte advance on bad headers |
| `lib/.../sfDevFPC2534UART.cpp` | `read()` uses `readBytes()` with a timeout; `setTimeout(10)` |
| `src/main.cpp` | `setRxBufferSize(2048)` **before** `Serial1.begin()`, a 2 ms service interval, drain-before-reset ordering, no sensor commands from callbacks |
| `test/unit/fingerprint_uart/` | the bench the synchroniser was developed against |

The RX ring must be enlarged before `begin()`: `setRxBufferSize()` only takes
effect on a UART that has not been started, and a call after `begin()` returns
without an error and without doing anything.

---

## 4.6 What this leaves behind

A synchronising parser degrades quietly. If the link desynchronises badly,
`processNextResponse()` keeps returning error 33 while walking the stream forward
one byte per call. The sensor is not reporting an error — it does not know
anything is wrong — so `on_error` never fires and the wallet simply stops
responding to fingers. From outside, that is indistinguishable from a dead
sensor.

The firmware's answer is to remove the *causes* of desynchronisation, because on
this board every one turned out to be self-inflicted: one owner for arming, a
drain before every command, a 400 ms settle on the failure path, and respecting
the sensor's own 15 s lockout. All four are in
[docs/05](05-system-integration-debugging.md).

**Still owed:** the driver does not count consecutive bad frames —
`processNextResponse()`'s return value is discarded in `TaskBiometricAuth`, so a
desynchronisation that survives all of the above still presents as a silent dead
sensor. A consecutive-bad-frame counter that triggers `sendReset()` past a
threshold is the obvious next piece of hardening.

Also owed: the synchroniser reaches for `Serial1` directly, bypassing the
library's `sfDevFPC2534IComm` abstraction, because `peek()` is not exposed
through it. That is why `lib/SparkFun_FPC2534/` is a vendored fork rather than a
registry dependency.

---

## 4.7 If you hit this on your own board

1. Error 33 means "these 8 bytes are not a valid frame header", nothing more.
   Do not read it as "sensor missing".
2. Check the arithmetic first: at 921600 baud a 512-byte ring overflows in about
   5 ms of inattention.
3. Never flush the buffer on a parse error. The frame you want is usually
   directly behind the garbage you are about to destroy.
4. Drain *before* a reset, never after — the ready message is sent once.
5. Print the bytes. Four fixes were reasoned from theory; the hex dump settled
   it in one line.
6. `begin()` returning `true` is not evidence anything is on the other end.
7. Once the framing is right, suspect *who else is talking* — see
   [docs/05](05-system-integration-debugging.md).
