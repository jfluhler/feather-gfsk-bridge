// =============================================================================
// SX1276FSK — Minimal bare-metal GFSK driver for SX1276/RFM95W
// =============================================================================
// Targets the Adafruit Feather M0 LoRa (ATSAMD21 + RFM95W/SX1276).
// Uses the SX1276's FSK modem directly, bypassing LoRa for ~12x throughput.
//
// Default config: 250 kbps GFSK (BT=0.5), 125 kHz deviation, 915 MHz, +20 dBm
//
// References:
//   - Semtech SX1276/77/78/79 Datasheet (Rev 7, May 2020)
//   - tve/SX1276fsk (https://github.com/tve/SX1276fsk) — approach reference
//   - ARMmbed/mbed-semtech-lora-rf-drivers — FSK register definitions
// =============================================================================

#ifndef SX1276FSK_H
#define SX1276FSK_H

#include <Arduino.h>
#include <SPI.h>

// --- SX1276 FSK register map (subset) ---
#define SX_REG_FIFO             0x00
#define SX_REG_OPMODE           0x01
#define SX_REG_BITRATE_MSB      0x02
#define SX_REG_BITRATE_LSB      0x03
#define SX_REG_FDEV_MSB         0x04
#define SX_REG_FDEV_LSB         0x05
#define SX_REG_FRF_MSB          0x06
#define SX_REG_FRF_MID          0x07
#define SX_REG_FRF_LSB          0x08
#define SX_REG_PA_CONFIG        0x09
#define SX_REG_PA_RAMP          0x0A
#define SX_REG_OCP              0x0B
#define SX_REG_LNA              0x0C
#define SX_REG_RX_CONFIG        0x0D
#define SX_REG_RSSI_CONFIG      0x0E
#define SX_REG_RSSI_THRESH      0x10
#define SX_REG_RSSI_VALUE       0x11
#define SX_REG_RXBW             0x12
#define SX_REG_AFCBW            0x13
#define SX_REG_PREAMBLE_DETECT  0x1F
#define SX_REG_PREAMBLE_MSB     0x25
#define SX_REG_PREAMBLE_LSB     0x26
#define SX_REG_SYNC_CONFIG      0x27
#define SX_REG_SYNC_VALUE1      0x28
#define SX_REG_SYNC_VALUE2      0x29
#define SX_REG_PACKET_CONFIG1   0x30
#define SX_REG_PACKET_CONFIG2   0x31
#define SX_REG_PAYLOAD_LENGTH   0x32
#define SX_REG_FIFO_THRESH      0x35
#define SX_REG_IRQ_FLAGS1       0x3E
#define SX_REG_IRQ_FLAGS2       0x3F
#define SX_REG_DIO_MAPPING1     0x40
#define SX_REG_DIO_MAPPING2     0x41
#define SX_REG_VERSION          0x42
#define SX_REG_PA_DAC           0x4D

// --- OpMode register ---
#define SX_OPMODE_LORA_BIT      0x80
#define SX_MODE_SLEEP           0x00
#define SX_MODE_STDBY           0x01
#define SX_MODE_FSTX            0x02
#define SX_MODE_TX              0x03
#define SX_MODE_FSRX            0x04
#define SX_MODE_RX              0x05

// --- IRQ flags 1 (0x3E) ---
#define SX_IRQ1_MODE_READY      0x80
#define SX_IRQ1_RX_READY        0x40
#define SX_IRQ1_TX_READY        0x20
#define SX_IRQ1_PREAMBLE        0x02
#define SX_IRQ1_SYNC_ADDR       0x01

// --- IRQ flags 2 (0x3F) ---
#define SX_IRQ2_FIFO_FULL       0x80
#define SX_IRQ2_FIFO_EMPTY      0x40
#define SX_IRQ2_FIFO_LEVEL      0x20
#define SX_IRQ2_FIFO_OVERRUN    0x10
#define SX_IRQ2_PACKET_SENT     0x08
#define SX_IRQ2_PAYLOAD_READY   0x04
#define SX_IRQ2_CRC_OK          0x02

// --- Payload limits ---
// FIFO = 64 bytes. Variable-length mode uses 1 byte for length prefix.
#define SX_MAX_PAYLOAD          63

// --- Crystal frequency ---
#define SX_FXOSC               32000000UL
#define SX_FSTEP               (SX_FXOSC / 524288.0)  // 2^19

class SX1276FSK {
public:
  SX1276FSK(uint8_t cs, uint8_t rst);

  // Initialize radio in GFSK mode. Returns false on failure.
  bool init();

  // Configure RF parameters
  void setFrequency(float mhz);
  void setTxPower(int8_t dbm);   // 2..20 dBm (PA_BOOST)
  void setBitrate(uint32_t bps);  // up to 300,000
  void setDeviation(uint32_t hz); // frequency deviation

  // Transmit a packet. Blocks until sent. Max len = SX_MAX_PAYLOAD.
  bool send(const uint8_t *data, uint8_t len);

  // Non-blocking send: start TX, returns immediately.
  bool sendStart(const uint8_t *data, uint8_t len);
  // Check if TX is complete.
  bool sendDone();

  // Switch to RX mode (call once, stays in RX).
  void startRx();

  // Check if a complete packet has been received.
  bool available();

  // Read received packet. Returns payload length, 0 if nothing.
  uint8_t receive(uint8_t *buf, uint8_t maxLen);

  // Last received packet RSSI in dBm.
  int16_t lastRssi();

  // Mode control
  void sleep();
  void standby();

private:
  uint8_t _cs, _rst;
  int16_t _lastRssi;
  bool    _inRx;
  bool    _inTx;

  void    setMode(uint8_t mode);
  void    waitMode(uint8_t flag);
  uint8_t readReg(uint8_t addr);
  void    writeReg(uint8_t addr, uint8_t val);
  void    readBurst(uint8_t addr, uint8_t *buf, uint8_t len);
  void    writeBurst(uint8_t addr, const uint8_t *buf, uint8_t len);
};

#endif
