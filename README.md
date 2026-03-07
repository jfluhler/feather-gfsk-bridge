# Feather GFSK Bridge

Wireless serial bridge using the Adafruit Feather M0 LoRa 900MHz. Bypasses the
LoRa modem and drives the SX1276/RFM95W in bare-metal GFSK mode for ~12x higher
throughput than LoRa at comparable range.

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

The packet format table is defined in `gfsk_tx.ino` and can be customized for
any fixed-length, header-delimited protocol with XOR checksums:

```cpp
const PacketFormat formats[FORMAT_COUNT] = {
  { 0xA5, 12 },  // Example: 12-byte packet starting with 0xA5
  { 0xB5,  8 },  // Example: 8-byte packet starting with 0xB5
  ...
};
```

## Configuration

All settings are configured over USB serial (115200 baud) and can be saved to
flash memory on the ATSAMD21 so they persist across power cycles.

### TX Commands

| Command | Description |
|---------|-------------|
| `?` | Show current settings and help |
| `d` | Toggle debug hex dump |
| `b <rate>` | Set UART1 baud rate (9600 - 5000000) |
| `m` | Toggle mode (RAW / FORMAT) |
| `s` | Save settings to flash |
| `r` | Reset to defaults |

### RX Commands

| Command | Description |
|---------|-------------|
| `?` | Show status, RSSI, frame count |
| `d` | Toggle debug hex dump |

See [Configuration Guide](doc/configuration.md) for details.

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

A standalone Python/tkinter GUI for logging serial data from the RX Feather
(or any serial device). Features:

- Port and baud rate selection
- Binary (.bin) or hex text (.log) output
- Configurable file path and prefix
- Live hex/ASCII display with auto-scroll
- Byte counter and file size tracking

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
│   │   └── gfsk_tx.ino
│   └── gfsk_rx/               # Receiver firmware
│       └── gfsk_rx.ino
├── app/
│   ├── serial_logger.py       # Standalone serial logger GUI
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
