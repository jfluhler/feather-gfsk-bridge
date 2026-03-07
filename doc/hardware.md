# Hardware Guide

## Required Hardware

- 2x [Adafruit Feather M0 LoRa 900MHz](https://www.adafruit.com/product/3178)
  (one TX, one RX)
- 2x USB micro-B **data** cables (charge-only cables will not work)
- Wire antenna or SMA antenna for each board

## Feather M0 LoRa Pinout

| Pin | Function | Used By |
|-----|----------|---------|
| 0 (RX) | UART1 receive | Serial data input from source device |
| 1 (TX) | UART1 transmit | Serial data output (optional, bidirectional) |
| 3 | RFM95 IRQ | Radio interrupt (active high) |
| 4 | RFM95 RST | Radio reset (active low) |
| 8 | RFM95 CS | Radio SPI chip select |
| 13 | LED | Activity indicator (toggles on each TX/RX) |
| USB | Native USB | Configuration and data (115200 baud) |

Pins 3, 4, 8, and 13 are used by the firmware. All other Feather pins are
available for other purposes.

## Antenna

The RFM95W has a u.FL connector and/or an SMA edge connector depending on the
Feather variant.

### Quarter-Wave Wire Antenna

A simple quarter-wave wire antenna works well for testing:

```
Length = c / (4 * f) = 3e8 / (4 * 915e6) = 82 mm
```

Solder 82 mm of solid wire to the antenna pad. Keep it straight and vertical
for best results.

### SMA Antenna

For outdoor or longer-range deployments, use a 915 MHz SMA antenna. A
2 dBi rubber duck antenna is typical. Directional antennas (e.g., Yagi)
can significantly extend range.

## Wiring — TX Side

Connect the serial data source to the TX Feather's UART1:

```
Serial Source                TX Feather
┌──────────┐                ┌──────────┐
│      TX ─────────────────── Pin 0 (RX)│
│      RX ─────────────────── Pin 1 (TX)│  (optional)
│     3.3V ────────────────── 3V        │
│     GND ─────────────────── GND       │
└──────────┘                └──────────┘
```

**Important:**
- Signal levels must be 3.3V. The ATSAMD21 is NOT 5V tolerant.
- Keep UART wires short (<15 cm) at high baud rates (>1 Mbaud).
- The TX pin (Feather pin 1) is optional — only needed if the source device
  accepts serial commands back from the Feather.

## Wiring — RX Side

The RX Feather connects to the PC via USB only. No additional wiring needed.

```
RX Feather ───USB───► PC
                      └── Serial Logger App or Serial Monitor
```

## Power

### USB Power

Both Feathers can be powered from USB. The TX Feather can also be powered from
the serial source's 3.3V supply via the 3V pin.

### Battery Power

Each Feather has a JST connector for a 3.7V LiPo battery. The battery charges
when USB is connected. Battery operation is useful for the TX Feather in
remote/field deployments.

**Current consumption:**
- TX during transmit (+20 dBm): ~120 mA
- TX idle/receive: ~15 mA
- RX continuous receive: ~12 mA
- Sleep (not implemented): ~5 uA

With a 500 mAh LiPo, expect ~4 hours of continuous TX at +20 dBm, or ~35
hours at idle.

### Reducing Power

If you don't need full +20 dBm TX power, reduce it via the `setTxPower()`
call in the firmware. Each 3 dB reduction roughly halves the transmit current:

| TX Power | Current | Range (approx) |
|----------|---------|----------------|
| +20 dBm | ~120 mA | 2+ km |
| +17 dBm | ~80 mA | 1.5 km |
| +14 dBm | ~55 mA | 1 km |
| +10 dBm | ~35 mA | 500 m |

## Range Tips

- **Line of sight** is critical at 915 MHz. Trees, buildings, and terrain
  absorb signal quickly.
- **Height** helps enormously. Elevating one antenna by 2 meters can double
  the effective range.
- **Antenna orientation** matters. Keep both antennas vertical and parallel.
- **Avoid metal** near the antenna. Metal objects within ~10 cm will detune it.
- **Fresnel zone** clearance: at 2 km and 915 MHz, keep the path clear of
  obstacles within ~8 m of the direct line between antennas.

## Troubleshooting

### Radio init failed

- Check that pin 8 (CS) and pin 4 (RST) are not shorted or used elsewhere
- Verify the Feather is a 900 MHz variant (not 433 MHz)
- The SX1276 version register should read 0x12

### No data received

- Confirm both Feathers are on the same frequency (915 MHz default)
- Check RSSI in RX stats (`?` command) — if very low (<-110 dBm), check antennas
- Verify TX UART baud matches the source device
- Try RAW mode (`m` command on TX) to rule out format parsing issues

### USB not detected

- Ensure the cable is a **data** cable (not charge-only)
- Try double-tap reset to enter bootloader (drive appears as `FEATHERBOOT`)
- Check for driver issues on Windows
