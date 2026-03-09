# Feather GFSK Bridge

Wireless serial bridge using the Adafruit Feather M0 LoRa 900MHz. Bypasses the
LoRa modem and drives the SX1276/RFM95W in bare-metal GFSK mode for ~12x higher
throughput than LoRa at comparable range.

The bridge is **protocol-agnostic** — all protocol-specific definitions live in
two config files (`packet_config.h` for firmware, `packet_formats.json` for the
logger app). Edit these to match your protocol without modifying any source code.

| Parameter | Value |
|-----------|-------|
| Modulation | GFSK (Gaussian BT=0.5) |
| Bitrate | 250 kbps (~31 KB/s) |
| Frequency | 915 MHz (US ISM) |
| Deviation | 125 kHz |
| TX Power | +20 dBm (100 mW) |
| Range | ~2 km line-of-sight |
| Max Frame | 63 bytes |
| Hardware | Adafruit Feather M0 LoRa 900MHz |

## Quick Start

### Prerequisites

- [Arduino CLI](https://arduino.github.io/arduino-cli/) or Arduino IDE
- Adafruit SAMD board support: `arduino-cli core install adafruit:samd`
- Two Adafruit Feather M0 LoRa 900MHz boards
- Python 3.8+ with pyserial (for the logger app)

### Install the Library

Copy `lib/SX1276FSK/` into your Arduino libraries folder:

```bash
cp -r lib/SX1276FSK ~/Arduino/libraries/
```

### Build & Upload

```bash
# Transmitter
arduino-cli compile --fqbn adafruit:samd:adafruit_feather_m0 firmware/gfsk_tx/
arduino-cli upload  --fqbn adafruit:samd:adafruit_feather_m0 --port /dev/ttyACM0 firmware/gfsk_tx/

# Receiver
arduino-cli compile --fqbn adafruit:samd:adafruit_feather_m0 firmware/gfsk_rx/
arduino-cli upload  --fqbn adafruit:samd:adafruit_feather_m0 --port /dev/ttyACM1 firmware/gfsk_rx/
```

### Run the Logger App

```bash
pip install -r app/requirements.txt
python app/serial_logger.py
```

## Architecture

```
                   UART (configurable baud)              GFSK 250 kbps
  Serial Device ─────────────────────────────► Feather TX )))  ~~~ ((( Feather RX ──► PC
                   (pins 0/1)                    RFM95W          915 MHz       USB Serial
```

The bridge is transparent — it relays serial bytes over the radio link without
modifying them. The TX and RX Feathers are a wireless replacement for a serial
cable.

## TX Operating Modes

### Raw Passthrough

Buffers incoming UART bytes and batches them into 63-byte radio frames using
time-based batching (~2ms silence triggers a flush). No knowledge of the data
format. Best for arbitrary serial protocols or ASCII text.

### Format-Aware

Parses incoming bytes by header/length, validates XOR checksums, and only
transmits verified packets. Batches multiple packets per radio frame for
efficiency. Corrupt or unrecognized bytes are discarded before wasting radio
bandwidth.

The packet format table is defined in `firmware/gfsk_tx/packet_config.h`:

```cpp
const PacketFormat formats[FORMAT_COUNT] = {
  { 0xA5, 12 },  // header=0xA5, total length=12 bytes (including checksum)
  { 0xB5,  8 },
  ...
};
```

### Hybrid

Self-framing packets whose length is determined by bit patterns in the first
byte. No header table or checksum validation is needed — the protocol itself
encodes packet boundaries. The framing logic is defined in `packet_config.h`:

```cpp
inline uint8_t hybridPacketLen(uint8_t firstByte) {
  if (firstByte == 0xFF) return 3;   // sync / overflow
  if (firstByte & 0x80)  return 3;   // group event
  return 2;                           // single event
}
```

## Customizing for Your Protocol

Two config files control all protocol-specific behavior:

| File | Purpose | Used By |
|------|---------|---------|
| `firmware/gfsk_tx/packet_config.h` | Packet table (FORMAT mode) and framing rules (HYBRID mode) | TX firmware |
| `app/packet_formats.json` | Packet field definitions, checksum config, and hybrid framing | Serial Logger app |

Edit these files to match your protocol. The firmware (`gfsk_tx.ino`), receiver
(`gfsk_rx.ino`), and logger app (`serial_logger.py`) are all protocol-agnostic
and do not need modification.

## Configuration

All runtime settings are configured over USB serial (115200 baud) and can be
saved to flash memory on the ATSAMD21 so they persist across power cycles.

### TX Commands

| Command | Description |
|---------|-------------|
| `?` | Show current settings and help |
| `d` | Toggle debug hex dump |
| `b <rate>` | Set UART1 baud rate (9600 - 5000000) |
| `m` | Cycle mode (RAW / FORMAT / HYBRID) |
| `h` | Set HYBRID mode directly |
| `t` | Run link test (100 frames, reports speed) |
| `s` | Save settings to flash |
| `r` | Reset to defaults |

### RX Commands

| Command | Description |
|---------|-------------|
| `?` | Show status, RSSI, link state |
| `d` | Toggle debug hex dump |
| `t` | Listen for link test (10 second window) |

### Link Test & Keepalive

The TX sends a 4-byte keepalive ping every 5 seconds when idle, so the RX
can detect whether the link is up. The RX warns if no keepalive arrives for
15 seconds.

To test radio performance, run `t` on both sides (RX first, then TX). The TX
sends 100 test frames; the RX reports packet loss, average RSSI, and
throughput.

See [Configuration Guide](doc/configuration.md) for full command reference,
default settings, and packet format customization.

See [Hardware Guide](doc/hardware.md) for wiring, antenna, power, and
troubleshooting.

## Radio Performance

### GFSK vs LoRa

| Metric | LoRa (BW500/SF7) | GFSK (250 kbps) | Factor |
|--------|-----------------|------------------|--------|
| Throughput | ~2.7 KB/s | ~27 KB/s | 10x |
| Max frame | 251 bytes | 63 bytes | 0.25x |
| Sensitivity | ~-118 dBm | ~-99 dBm | -19 dB |
| Range (LOS) | 5-10 km | 1-3 km | ~0.3x |
| Flash usage | 39 KB (15%) | 24 KB (9%) | 0.6x |

### Link Budget (2 km)

| Factor | Value |
|--------|-------|
| TX power | +20 dBm |
| Antenna gain (each side) | ~2 dBi |
| Free-space path loss (915 MHz, 2 km) | -99.7 dB |
| Fading margin | -10 dB |
| Signal at receiver | -85.7 dBm |
| Sensitivity @ 250 kbps | ~-99 dBm |
| **Link margin** | **13.3 dB** |

## Serial Logger App

A standalone Python/tkinter GUI for logging and decoding serial data from the
RX Feather (or any serial device). Features:

- Port and baud rate selection
- Binary (.bin) or hex text (.log) output
- Configurable file path and prefix
- Live display modes: Hex, ASCII, Both, and **Decoded**
- Byte counter and file size tracking
- **Packet decode view** — parses and displays structured packets in real-time
  with checksum validation, field decoding, and color-coded output
- Supports both FORMAT and HYBRID decode modes

Packet formats are defined in `app/packet_formats.json`. Edit this file to
match your protocol. The JSON config supports field types including integers,
hex, bitmasks, bit flags, enumerations, and binary representations.

![Serial Logger](doc/serial_logger_screenshot.png)

## Project Structure

```
feather-gfsk-bridge/
├── README.md
├── LICENSE
├── lib/
│   └── SX1276FSK/            # Bare-metal GFSK driver for SX1276/RFM95W
│       ├── SX1276FSK.h
│       └── SX1276FSK.cpp
├── firmware/
│   ├── gfsk_tx/               # Transmitter firmware
│   │   ├── gfsk_tx.ino
│   │   └── packet_config.h    # Packet format definitions (edit for your protocol)
│   └── gfsk_rx/               # Receiver firmware
│       └── gfsk_rx.ino
├── app/
│   ├── serial_logger.py       # Serial logger GUI with packet decoder
│   ├── packet_formats.json    # Packet decode definitions (edit for your protocol)
│   └── requirements.txt
└── doc/
    ├── configuration.md        # Detailed configuration guide
    └── hardware.md             # Wiring, antenna, power notes
```

## Hardware

### Adafruit Feather M0 LoRa 900MHz

| Component | Specification |
|-----------|---------------|
| MCU | ATSAMD21G18 (ARM Cortex-M0+, 48 MHz, 256 KB flash, 32 KB RAM) |
| Radio | SX1276 / RFM95W (137-1020 MHz) |
| Antenna | SMA or wire (quarter-wave: 82 mm for 915 MHz) |
| USB | Native USB (CDC serial) |
| UART | Serial1 on pins 0 (RX) and 1 (TX), up to 5 Mbaud |
| Power | USB, LiPo battery, or 3.3V supply |

### Wiring (TX Side)

Connect the serial data source to the Feather's UART1 pins:

| Source | Feather Pin |
|--------|-------------|
| TX (data out) | Pin 0 (RX) |
| RX (data in) | Pin 1 (TX) — optional, for bidirectional |
| 3.3V | 3V |
| GND | GND |

The RX Feather connects to the PC via USB only.

## Acknowledgments

- [tve/SX1276fsk](https://github.com/tve/SX1276fsk) — reference implementation
  for bare-metal FSK on the SX1276
- [Semtech SX1276/77/78/79 Datasheet](https://cdn-shop.adafruit.com/product-files/3179/sx1276_77_78_79.pdf) —
  register map and electrical specifications
- [ARMmbed/mbed-semtech-lora-rf-drivers](https://github.com/ARMmbed/mbed-semtech-lora-rf-drivers) —
  FSK register definitions reference
- [Adafruit](https://www.adafruit.com/) — Feather M0 LoRa hardware, bootloader,
  and SAMD board support package
- [FlashStorage](https://github.com/cmaglie/FlashStorage) — EEPROM emulation
  for SAMD21

## License

MIT License. See [LICENSE](LICENSE) for details.
