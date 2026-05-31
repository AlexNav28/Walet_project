# FPC2534 UART Debugging Notes

Date: 2026-05-27

## Hardware Setup
- ESP32-S3-MINI-1 custom PCB
- FPC2534 fingerprint sensor connected via ZIF connector
- UART mode at 921600 baud (SERIAL_8N1)
- RST_N tied to 3.3V (active-low, so sensor runs normally)
- CS_N (SYS_WU) tied to 3.3V (UART mode selected, no deep sleep)
- IRQ on GPIO 16, TX on GPIO 17, RX on GPIO 18

## Pin Configuration Confirmed Correct
- RST_N HIGH = sensor not in reset = correct
- CS_N HIGH = SPI deselected / sensor stays awake = correct for UART mode
- 921600 baud confirmed correct (115200 produces no data, 921600 produces data)

## Symptoms Observed

### Original issue (200ms loop delay)
- Error 33 (`FPC_RESULT_IO_BAD_DATA`) flooding continuously
- Occasionally "FPC2534 Device is ready" would slip through
- Caused by: 200ms delay at 921600 baud = ~18,400 bytes arriving per loop iteration,
  overflowing the 512-byte RX buffer, causing byte loss and frame desynchronization

### After reducing delay to 1ms
- Error 35 (`FPC_RESULT_IO_NO_DATA`) — partial frames being caught mid-transmission
- Fixed by using `readBytes()` with timeout instead of instant `available()` check

### After adding buffer flush logic
- No output at all — the flush was discarding the sensor's boot status message
- The sensor only sends its "ready" status ONCE after boot

### After fixing flush timing (flush before reset, not after)
- Debug output shows bad header bytes: `00 78 3A 3A 3A 3A 3A 3A`
- 0x78 = 'x', 0x3A = ':' in ASCII — this is bootloader/debug text, not binary protocol

## Root Cause Analysis

The FPC2534 boot sequence sends data in two stages:
1. **Bootloader stage**: Sends ASCII debug/diagnostic text (not binary protocol frames)
2. **Application firmware stage**: Sends proper binary protocol frames (version 0x0004, flags with 0x0040)

The library's `processNextResponse()` reads the first 8 bytes expecting a binary frame header.
If it hits bootloader ASCII text first, validation fails (error 33). The error handler then
flushes the entire buffer, which also discards the valid binary frame that came AFTER the
bootloader text. The sensor is then idle and won't send another "ready" message.

### Frame header validation check (sfDevFPC2534.cpp):
```cpp
if (frameHeader.version != FPC_FRAME_PROTOCOL_VERSION ||          // must be 0x0004
    ((frameHeader.flags & FPC_FRAME_FLAG_SENDER_FW_APP) == 0) ||  // must have 0x0040
    (frameHeader.type != FPC_FRAME_TYPE_CMD_RESPONSE &&            // must be 0x12 or 0x13
     frameHeader.type != FPC_FRAME_TYPE_CMD_EVENT))
{
    return FPC_RESULT_IO_BAD_DATA;  // Error 33
}
```

## What "FPC2534 initialized" Actually Means

The `begin()` → `initialize()` function NEVER communicates with the sensor:
```cpp
bool initialize(sfDevFPC2534IComm &comm)
{
    _comm = &comm;
    return true;  // Always returns true
}
```
It only stores the Serial pointer. "Initialized" is misleading — it does NOT confirm the sensor responded.

## Solution Needed

Implement **frame synchronization**: instead of flushing the entire buffer on a bad header,
scan byte-by-byte looking for the frame start pattern (`04 00` = protocol version 0x0004 in
little-endian). This skips bootloader garbage and latches onto the first valid binary frame.

## Key Constants (from fpc_api.h)
- `FPC_FRAME_PROTOCOL_VERSION` = 0x0004
- `FPC_FRAME_TYPE_CMD_RESPONSE` = 0x12
- `FPC_FRAME_TYPE_CMD_EVENT` = 0x13
- `FPC_FRAME_FLAG_SENDER_FW_APP` = 0x0040
- `FPC_FRAME_FLAG_SENDER_FW_BL` = 0x0020
- Frame header size = 8 bytes (fpc_frame_hdr_t)

## Error Codes Reference
- 0 = FPC_RESULT_OK
- 1 = FPC_PENDING_OPERATION
- 33 = FPC_RESULT_IO_BAD_DATA (header validation failed)
- 35 = FPC_RESULT_IO_NO_DATA (not enough bytes available)

## Files Modified During Debugging
- `src/main.cpp` — startup sequence, loop error handling, buffer sizes
- `lib/SparkFun_FPC2534/src/sfTk/sfDevFPC2534UART.cpp` — read() function, setTimeout
- `lib/SparkFun_FPC2534/src/sfTk/sfDevFPC2534.cpp` — debug hex dump in bad_data path
