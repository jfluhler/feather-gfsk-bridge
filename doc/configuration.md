# Configuration Guide

## Overview

The Feather GFSK Bridge is configured via USB serial commands at 115200 baud.
Settings can be saved to the ATSAMD21's internal flash and persist across
power cycles and resets.

All protocol-specific definitions live in two config files — the firmware and
logger app do not need modification to support a different protocol.

| Config File | Purpose | Location |
|-------------|---------|----------|
| `packet_config.h` | Firmware packet table + hybrid framing | `firmware/gfsk_tx/` |
| `packet_formats.json` | Logger app field decoding + display | `app/` |

## Factory Defaults

| Setting | Default | Notes |
|---------|---------|-------|
| UART1 baud | 1,000,000 (1 Mbaud) | Configurable at runtime via `b` command |
| Operating mode | FORMAT | Configurable at runtime via `m` command |
| Debug output | OFF | Configurable at runtime via `d` command |
| Keepalive interval | 30 seconds | Configurable via `k` command (0 = disabled) |
| Radio bitrate | 250 kbps | Compiled — edit `SX1276FSK.cpp` to change |
| Radio frequency | 915 MHz | Compiled — edit `SX1276FSK.cpp` to change |
| TX power | +20 dBm (100 mW) | Compiled — edit `SX1276FSK.cpp` to change |
| Ring buffer | 8 KB | Compiled — edit `RING_SIZE` in `gfsk_tx.ino` |
| Batch timeout | 2 ms (RAW mode) | Compiled — edit `RAW_BATCH_TIMEOUT_MS` |
| USB serial | 115200 baud | Fixed (command/debug interface) |

Radio parameters are compiled into the firmware. To change them at startup,
call `radio.setBitrate()`, `radio.setFrequency()`, and `radio.setTxPower()`
in `setup()`.

## TX Firmware Commands

Connect to the TX Feather's USB serial port at 115200 baud (e.g., Arduino
Serial Monitor, `screen /dev/ttyACM0 115200`, or the Serial Logger app).

### Quick Reference

| Command | Description | Needs Enter? |
|---------|-------------|-------------|
| `?` | Show settings and help | No |
| `d` | Toggle debug hex dump | No |
| `m` | Cycle mode (RAW/FORMAT/HYBRID) | No |
| `h` | Set HYBRID mode directly | No |
| `t` | Link test (100 frames, packet loss) | No |
| `T` | Throughput test (10s, max speed) | No |
| `k <sec>` | Set keepalive interval (0=off) | Yes |
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
- `1000000` — 1 Mbaud (factory default)
- `5000000` — 5 Mbaud (requires short traces, good signal integrity)

### Operating Mode

```
m
```

Cycles through RAW, FORMAT, and HYBRID modes:

**RAW mode:**
- All bytes from UART1 are buffered and transmitted as-is
- Time-based batching: waits ~2ms for more bytes before sending
- No validation, no filtering
- Best for: ASCII text, arbitrary protocols, testing

**FORMAT mode (default):**
- Parses bytes looking for known packet headers (defined in `packet_config.h`)
- Validates packet length and XOR checksum before transmitting
- Batches multiple validated packets per radio frame
- Discards corrupt or unrecognized bytes
- Best for: structured binary protocols with fixed-length packets

**HYBRID mode:**
- Self-framing packets whose length is determined by `hybridPacketLen()` in `packet_config.h`
- No header table or checksum validation needed
- Batches complete packets per radio frame
- Best for: compact, self-describing packet protocols

Use `h` to switch directly to HYBRID mode without cycling through the others.

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
| `?` | Show status (frames, bytes, RSSI, link state) |
| `d` | Toggle debug hex dump of received frames |

The RX periodically prints stats prefixed with `#`:
```
# RX: frames=142 bytes=8520 RSSI=-45 link=UP dBm
```

Lines starting with `#` can be filtered out by the receiving application to
separate metadata from payload data.

## Radio Tests

Both tests are auto-detected on the RX — no manual setup is needed on the
receive side. Test and throughput frames are never forwarded to serial output.

### Link Test (`t`)

Sends 100 × 32-byte test frames (`0xFE` marker) with sequence numbers.
Measures packet loss and average RSSI. Completes in ~200 ms.

```
t
```

Example RX output:
```
# --- Link Test: receiving ---
# --- Link Test Results ---
#   Frames received: 100
#   Frames expected: 100
#   Packet loss:     0.0 %
#   Avg RSSI:        -14 dBm
#   Throughput:      17977 B/s
#   Total:           3200 bytes in 178 ms
# --------------------------
```

### Throughput Test (`T`)

Sends maximum-size 63-byte frames (`0xFC` marker) back-to-back for 10
seconds. Measures sustained throughput at the radio's maximum capacity.

```
T
```

Example RX output:
```
# --- Throughput Test: receiving ---
# --- Throughput Test Results ---
#   Frames received: 3504
#   Frames expected: 3504
#   Packet loss:     0.0 %
#   Avg RSSI:        -13 dBm
#   Throughput:      22070 B/s (176 kbps)
#   Total:           220752 bytes in 10002 ms
# --------------------------
```

Typical result: **22 KB/s (176 kbps)** — 70% of the 250 kbps raw bitrate.
The remaining 30% is consumed by per-frame overhead (preamble, sync word,
SPI FIFO load time, and inter-frame gap).

## Keepalive

The TX sends a 4-byte keepalive ping (`0xFD` marker + 24-bit uptime in
seconds) at a configurable interval (default 30 seconds) when the radio is
idle (no data queued). Use `k <sec>` to change the interval (0 = disabled),
then `s` to save.

The RX tracks the last keepalive and reports link status:

- `?` output includes `Link: UP (last ping Ns ago, RSSI=X dBm)` or
  `Link: no keepalive received`
- Periodic stats include `link=UP` when active
- A warning is printed if no keepalive arrives for 2 minutes:
  `# WARN: keepalive lost (TX out of range or powered off)`

Keepalive frames are not forwarded to serial output.

## Customizing Packet Formats

### Firmware Config — `packet_config.h`

This file defines how the TX firmware identifies and frames packets. Edit it
to match your protocol. The full default file:

```cpp
#ifndef PACKET_CONFIG_H
#define PACKET_CONFIG_H

// ---------------------------------------------------------------------------
// FORMAT MODE — Packet Table
// ---------------------------------------------------------------------------
// Each entry: { header_byte, total_length }
//   header_byte   Unique first byte that identifies the packet type.
//   total_length  Fixed packet size in bytes (header + payload + checksum).
//
// Requirements:
//   - Every packet type must have a unique header byte.
//   - Length includes the header and the trailing checksum byte.
//   - Maximum single packet size: 63 bytes (SX1276 FIFO limit).
//   - Checksum is XOR of all preceding bytes; XOR of the full packet == 0x00.

struct PacketFormat {
  uint8_t header;
  uint8_t length;
};

#define FORMAT_COUNT 4

const PacketFormat formats[FORMAT_COUNT] = {
  { 0xA5, 12 },  // 12 bytes: header + payload + checksum
  { 0xB5,  8 },  //  8 bytes
  { 0xC5, 10 },  // 10 bytes
  { 0xD5,  9 },  //  9 bytes
};

// ---------------------------------------------------------------------------
// HYBRID MODE — Self-Framing Rules
// ---------------------------------------------------------------------------
// Hybrid packets encode their own length in the first byte. Edit the
// function below to match your framing scheme.
//
// Default rules (3 packet types):
//   0xFF          Sync/overflow marker — 3 bytes
//   bit 7 = 1    Group event          — 3 bytes
//   bit 7 = 0    Single event         — 2 bytes

inline uint8_t hybridPacketLen(uint8_t firstByte) {
  if (firstByte == 0xFF) return 3;   // sync / overflow
  if (firstByte & 0x80)  return 3;   // group event
  return 2;                           // single event
}

#endif // PACKET_CONFIG_H
```

### Logger App Config — `packet_formats.json`

This file defines how the Serial Logger app decodes and displays packets. It
must match the firmware's `packet_config.h`. The full default file:

```json
{
  "_comment": "Packet format definitions for the Serial Logger decoder.",

  "checksum": {
    "type": "xor",
    "position": "last",
    "note": "XOR of all bytes including header; valid packet XORs to 0x00"
  },

  "format_mode": {
    "_comment": "Fixed-length packets identified by header byte.",
    "packets": [
      {
        "header": "0xA5",
        "name": "Normal Event",
        "length": 12,
        "fields": [
          { "name": "header",    "offset": 0, "size": 1, "format": "hex" },
          { "name": "timestamp", "offset": 1, "size": 4, "format": "uint32", "unit": "ticks" },
          { "name": "state",     "offset": 5, "size": 6, "format": "hex" },
          { "name": "checksum",  "offset": 11, "size": 1, "format": "hex" }
        ]
      },
      {
        "header": "0xB5",
        "name": "Channel Event",
        "length": 8,
        "fields": [
          { "name": "header",    "offset": 0, "size": 1, "format": "hex" },
          { "name": "timestamp", "offset": 1, "size": 4, "format": "uint32", "unit": "ticks" },
          { "name": "channel",   "offset": 5, "size": 1, "format": "uint8" },
          { "name": "state",     "offset": 6, "size": 1, "format": "uint8" },
          { "name": "checksum",  "offset": 7, "size": 1, "format": "hex" }
        ]
      },
      {
        "header": "0xC5",
        "name": "Reduced Summary",
        "length": 10,
        "fields": [
          { "name": "header",      "offset": 0, "size": 1, "format": "hex" },
          { "name": "channel",     "offset": 1, "size": 1, "format": "uint8" },
          { "name": "first_time",  "offset": 2, "size": 4, "format": "uint32", "unit": "ticks" },
          { "name": "count",       "offset": 6, "size": 2, "format": "uint16" },
          { "name": "final_state", "offset": 8, "size": 1, "format": "uint8" },
          { "name": "checksum",    "offset": 9, "size": 1, "format": "hex" }
        ]
      },
      {
        "header": "0xD5",
        "name": "Status/ACK",
        "length": 9,
        "fields": [
          { "name": "header",     "offset": 0, "size": 1, "format": "hex" },
          { "name": "status",     "offset": 1, "size": 1, "format": "bits", "bits": {
              "0": "overflow", "1": "retransmitting", "2": "rate_limited", "3": "pmod_active"
          }},
          { "name": "mode",       "offset": 2, "size": 1, "format": "enum", "values": {
              "0": "Normal", "1": "Channel Event", "2": "SerDes", "3": "Reserved"
          }},
          { "name": "fw_major",   "offset": 3, "size": 1, "format": "uint8" },
          { "name": "fw_minor",   "offset": 4, "size": 1, "format": "uint8" },
          { "name": "fw_patch",   "offset": 5, "size": 1, "format": "uint8" },
          { "name": "batt_adc",   "offset": 6, "size": 2, "format": "uint16" },
          { "name": "checksum",   "offset": 8, "size": 1, "format": "hex" }
        ]
      }
    ]
  },

  "hybrid_mode": {
    "_comment": "Self-framing packets whose length is encoded in the first byte.",
    "framing_rules": [
      { "match": "value",   "value": "0xFF", "length": 3, "name": "Sync/Overflow" },
      { "match": "bitmask", "mask": "0x80",  "expect": "0x80", "length": 3, "name": "Group Event" },
      { "match": "bitmask", "mask": "0x80",  "expect": "0x00", "length": 2, "name": "Single Event" }
    ],
    "packets": [
      {
        "name": "Single Event",
        "length": 2,
        "fields": [
          { "name": "state",   "offset": 0, "size": 1, "format": "bit", "bit": 6 },
          { "name": "channel", "offset": 0, "size": 1, "format": "mask", "mask": "0x3F" },
          { "name": "delta_t", "offset": 1, "size": 1, "format": "uint8", "unit": "ticks" }
        ]
      },
      {
        "name": "Sync/Overflow",
        "length": 3,
        "fields": [
          { "name": "marker",  "offset": 0, "size": 1, "format": "hex" },
          { "name": "delta_t", "offset": 1, "size": 2, "format": "uint16", "unit": "ticks" }
        ]
      },
      {
        "name": "Group Event",
        "length": 3,
        "fields": [
          { "name": "state",    "offset": 0, "size": 1, "format": "bit", "bit": 6 },
          { "name": "base_ch",  "offset": 0, "size": 1, "format": "mask", "mask": "0x3F" },
          { "name": "bitmap",   "offset": 1, "size": 1, "format": "bin" },
          { "name": "delta_t",  "offset": 2, "size": 1, "format": "uint8", "unit": "ticks" }
        ]
      }
    ]
  }
}
```

### JSON Field Format Types

| Format | Description | Example Output |
|--------|-------------|----------------|
| `hex` | Raw hex bytes | `A5`, `00 0F` |
| `uint8` | Unsigned 8-bit integer | `42` |
| `uint16` | Unsigned 16-bit integer (big-endian) | `1024` |
| `uint32` | Unsigned 32-bit integer (big-endian) | `98760` |
| `bit` | Single bit extraction (specify `"bit": N`) | `1` |
| `mask` | Bitmask extraction (specify `"mask": "0x3F"`) | `5` |
| `bin` | Binary string | `00000011` |
| `bits` | Named bit flags (specify `"bits": {"0": "name", ...}`) | `overflow, rate_limited` |
| `enum` | Named values (specify `"values": {"0": "name", ...}`) | `Normal` |

Add `"unit": "ticks"` to any numeric field to append a unit label in the display.

## Flash Storage Notes

- Settings are stored in a dedicated flash page on the ATSAMD21
- A magic number (0xBF03) validates the stored data
- Re-flashing firmware erases saved settings (defaults will be restored)
- Flash write endurance: ~10,000 cycles (don't save in a loop)
