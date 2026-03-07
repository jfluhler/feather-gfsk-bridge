# Configuration Guide

## Overview

The Feather GFSK Bridge is configured via USB serial commands at 115200 baud.
Settings can be saved to the ATSAMD21's internal flash and persist across
power cycles and resets.

## TX Firmware Commands

Connect to the TX Feather's USB serial port at 115200 baud (e.g., Arduino
Serial Monitor, `screen /dev/ttyACM0 115200`, or the Serial Logger app).

### Quick Reference

| Command | Description | Needs Enter? |
|---------|-------------|-------------|
| `?` | Show settings and help | No |
| `d` | Toggle debug hex dump | No |
| `m` | Toggle RAW/FORMAT mode | No |
| `s` | Save settings to flash | No |
| `r` | Reset to defaults | No |
| `b <rate>` | Set UART1 baud rate | Yes |

Single-character commands execute immediately. The `b` command requires a
parameter and needs Enter to submit.

### UART Baud Rate

```
b 1000000
```

Sets the UART1 (pins 0/1) baud rate. Valid range: 9600 to 5,000,000. The
change takes effect immediately. Use `s` to save.

Common values:
- `9600` — standard low-speed
- `115200` — common default
- `1000000` — 1 Mbaud
- `5000000` — 5 Mbaud (requires short traces, good signal integrity)

### Operating Mode

```
m
```

Toggles between RAW and FORMAT modes:

**RAW mode:**
- All bytes from UART1 are buffered and transmitted as-is
- Time-based batching: waits ~2ms for more bytes before sending
- No validation, no filtering
- Best for: ASCII text, arbitrary protocols, testing

**FORMAT mode:**
- Parses bytes looking for known packet headers
- Validates packet length and XOR checksum before transmitting
- Batches multiple validated packets per radio frame
- Discards corrupt or unrecognized bytes
- Best for: structured binary protocols with fixed-length packets

### Save / Reset

```
s       Save current settings to flash
r       Reset to defaults (1 Mbaud, FORMAT mode)
```

After `r`, settings are active but not saved. Use `s` to make them permanent.

### Debug Mode

```
d
```

Toggles debug output on USB serial:
- **FORMAT mode:** prints hex dump of each validated packet
  ```
  [A5 00 01 23 45 00 00 FF FF 00 00 A5 96]
  ```
- **RAW mode:** no per-byte debug (would flood the output)

Debug mode adds latency — disable for production use.

## RX Firmware Commands

| Command | Description |
|---------|-------------|
| `?` | Show status (frames, bytes, RSSI) |
| `d` | Toggle debug hex dump of received frames |

The RX periodically prints stats prefixed with `#`:
```
# RX: frames=142 bytes=8520 RSSI=-45 dBm
```

Lines starting with `#` can be filtered out by the receiving application to
separate metadata from payload data.

## Customizing Packet Formats

To use FORMAT mode with your own protocol, edit the format table in
`gfsk_tx.ino`:

```cpp
#define FORMAT_COUNT 4
const PacketFormat formats[FORMAT_COUNT] = {
  { 0xA5, 12 },  // header=0xA5, total length=12 bytes (including checksum)
  { 0xB5,  8 },
  { 0xC5, 10 },
  { 0xD5,  9 },
};
```

Requirements:
- Each packet type has a unique single-byte header
- Packet length is fixed (header + payload + 1-byte XOR checksum)
- Checksum is XOR of all bytes (including header); result should be 0x00
- Maximum single packet size: 63 bytes (SX1276 FIFO limit minus length prefix)

## Factory Defaults

| Setting | Default |
|---------|---------|
| UART1 baud | 1,000,000 |
| Mode | FORMAT |
| Radio bitrate | 250 kbps (not configurable at runtime) |
| Radio frequency | 915 MHz (not configurable at runtime) |
| TX power | +20 dBm (not configurable at runtime) |

Radio parameters are compiled into the firmware. To change them, modify the
`radio.init()` defaults in `SX1276FSK.cpp` or call `radio.setBitrate()`,
`radio.setFrequency()`, and `radio.setTxPower()` in setup.

## Flash Storage Notes

- Settings are stored in a dedicated flash page on the ATSAMD21
- A magic number (0xBF02) validates the stored data
- Re-flashing firmware erases saved settings (defaults will be restored)
- Flash write endurance: ~10,000 cycles (don't save in a loop)
