// =============================================================================
// SX1276FSK — Minimal bare-metal FSK driver for SX1276/RFM95W
// =============================================================================

#include "SX1276FSK.h"

// SPI settings: SX1276 supports up to 10 MHz SPI clock
static const SPISettings SX_SPI(8000000, MSBFIRST, SPI_MODE0);

SX1276FSK::SX1276FSK(uint8_t cs, uint8_t rst)
  : _cs(cs), _rst(rst), _lastRssi(0), _inRx(false), _inTx(false) {}

// --- SPI register access ---

uint8_t SX1276FSK::readReg(uint8_t addr) {
  SPI.beginTransaction(SX_SPI);
  digitalWrite(_cs, LOW);
  SPI.transfer(addr & 0x7F);  // bit 7 = 0 for read
  uint8_t val = SPI.transfer(0);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
  return val;
}

void SX1276FSK::writeReg(uint8_t addr, uint8_t val) {
  SPI.beginTransaction(SX_SPI);
  digitalWrite(_cs, LOW);
  SPI.transfer(addr | 0x80);  // bit 7 = 1 for write
  SPI.transfer(val);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

void SX1276FSK::readBurst(uint8_t addr, uint8_t *buf, uint8_t len) {
  SPI.beginTransaction(SX_SPI);
  digitalWrite(_cs, LOW);
  SPI.transfer(addr & 0x7F);
  for (uint8_t i = 0; i < len; i++)
    buf[i] = SPI.transfer(0);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

void SX1276FSK::writeBurst(uint8_t addr, const uint8_t *buf, uint8_t len) {
  SPI.beginTransaction(SX_SPI);
  digitalWrite(_cs, LOW);
  SPI.transfer(addr | 0x80);
  for (uint8_t i = 0; i < len; i++)
    SPI.transfer(buf[i]);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

// --- Mode control ---

void SX1276FSK::setMode(uint8_t mode) {
  // Preserve upper bits (FSK mode = bit 7 clear), set mode in bits [2:0]
  writeReg(SX_REG_OPMODE, (readReg(SX_REG_OPMODE) & 0xF8) | (mode & 0x07));
}

void SX1276FSK::waitMode(uint8_t flag) {
  // Wait for a specific IrqFlags1 bit to be set
  uint32_t t0 = millis();
  while (!(readReg(SX_REG_IRQ_FLAGS1) & flag)) {
    if (millis() - t0 > 100) break;  // 100ms timeout
  }
}

void SX1276FSK::sleep() {
  setMode(SX_MODE_SLEEP);
  _inRx = false;
  _inTx = false;
}

void SX1276FSK::standby() {
  setMode(SX_MODE_STDBY);
  waitMode(SX_IRQ1_MODE_READY);
  _inRx = false;
  _inTx = false;
}

// --- Initialization ---

bool SX1276FSK::init() {
  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
  pinMode(_rst, OUTPUT);

  // Hardware reset
  digitalWrite(_rst, LOW);
  delay(1);
  digitalWrite(_rst, HIGH);
  delay(10);

  SPI.begin();

  // Verify chip is responding
  uint8_t ver = readReg(SX_REG_VERSION);
  if (ver != 0x12) return false;  // SX1276 version = 0x12

  // Enter sleep mode first (required to switch LoRa<->FSK)
  writeReg(SX_REG_OPMODE, 0x00);  // Sleep, FSK mode (bit 7 = 0)
  delay(5);

  // Go to standby for configuration
  setMode(SX_MODE_STDBY);
  waitMode(SX_IRQ1_MODE_READY);

  // --- Bitrate: 250 kbps ---
  // RegBitrate = FXOSC / bitrate = 32,000,000 / 250,000 = 128 = 0x0080
  writeReg(SX_REG_BITRATE_MSB, 0x00);
  writeReg(SX_REG_BITRATE_LSB, 0x80);

  // --- Frequency deviation: 125 kHz ---
  // RegFdev = Fdev / Fstep = 125,000 / 61.035 = 2048 = 0x0800
  writeReg(SX_REG_FDEV_MSB, 0x08);
  writeReg(SX_REG_FDEV_LSB, 0x00);

  // --- Carrier frequency: 915.0 MHz ---
  // RegFrf = 915,000,000 / 61.035 = 14,991,360 = 0xE4C000
  writeReg(SX_REG_FRF_MSB, 0xE4);
  writeReg(SX_REG_FRF_MID, 0xC0);
  writeReg(SX_REG_FRF_LSB, 0x00);

  // --- PA: +20 dBm on PA_BOOST ---
  // PaConfig: PA_BOOST=1, MaxPower=7, OutputPower=15
  writeReg(SX_REG_PA_CONFIG, 0xFF);
  // PaDac: enable +20 dBm mode (0x87 = enable, 0x84 = default/disable)
  writeReg(SX_REG_PA_DAC, 0x87);
  // OCP: 240 mA for +20 dBm (OcpOn=1, OcpTrim=27 → 240 mA)
  writeReg(SX_REG_OCP, 0x3B);

  // --- Modulation: GFSK with BT=0.5 ---
  // PaRamp[6:5] = 10 (Gaussian BT=0.5), ramp = 40us
  writeReg(SX_REG_PA_RAMP, 0x49);

  // --- Receiver ---
  // RxConfig: AGC auto on, trigger on PreambleDetect
  writeReg(SX_REG_RX_CONFIG, 0x0E);

  // RxBw: 250 kHz single-sideband (widest clean setting for 250kbps + 125kHz dev)
  //   RxBwMant=00(16), RxBwExp=1 → FXOSC / (16 * 2^(1+2)) = 32M / 128 = 250 kHz
  //   Carson BW needed: 2*(125+125) = 500 kHz DSB → 250 kHz SSB
  writeReg(SX_REG_RXBW, 0x01);

  // AfcBw: same as RxBw
  writeReg(SX_REG_AFCBW, 0x01);

  // LNA: max gain, auto via AGC
  writeReg(SX_REG_LNA, 0x20);

  // --- Preamble: 4 bytes of 0xAA ---
  writeReg(SX_REG_PREAMBLE_MSB, 0x00);
  writeReg(SX_REG_PREAMBLE_LSB, 0x04);

  // Preamble detector: on, 2-byte detect, 10 bit errors tolerance
  writeReg(SX_REG_PREAMBLE_DETECT, 0xAA);

  // --- Sync word: 2 bytes (0x2D 0xD4) ---
  // SyncConfig: auto restart RX on, sync on, 2 bytes (SyncSize=1 → 1+1=2)
  writeReg(SX_REG_SYNC_CONFIG, 0x51);
  writeReg(SX_REG_SYNC_VALUE1, 0x2D);
  writeReg(SX_REG_SYNC_VALUE2, 0xD4);

  // --- Packet engine ---
  // PacketConfig1: variable length, whitening, CRC on, no address filter
  //   [7]=1 variable, [6:5]=10 whitening, [4]=1 CRC on, [3]=0 auto clear,
  //   [2:1]=00 no addr filter, [0]=0
  writeReg(SX_REG_PACKET_CONFIG1, 0xD0);

  // PacketConfig2: packet mode (bit 6=1)
  writeReg(SX_REG_PACKET_CONFIG2, 0x40);

  // Max payload length
  writeReg(SX_REG_PAYLOAD_LENGTH, SX_MAX_PAYLOAD);

  // FIFO threshold: TX start on FifoNotEmpty (bit 7=1), threshold=15
  writeReg(SX_REG_FIFO_THRESH, 0x8F);

  // RSSI smoothing: 8 samples
  writeReg(SX_REG_RSSI_CONFIG, 0x02);

  // Clear FIFO overrun flag
  writeReg(SX_REG_IRQ_FLAGS2, SX_IRQ2_FIFO_OVERRUN);

  return true;
}

// --- Configuration ---

void SX1276FSK::setFrequency(float mhz) {
  uint32_t frf = (uint32_t)(mhz * 1000000.0 / SX_FSTEP);
  standby();
  writeReg(SX_REG_FRF_MSB, (frf >> 16) & 0xFF);
  writeReg(SX_REG_FRF_MID, (frf >> 8) & 0xFF);
  writeReg(SX_REG_FRF_LSB, frf & 0xFF);
}

void SX1276FSK::setTxPower(int8_t dbm) {
  // PA_BOOST output: Pout = 17 - (15 - OutputPower) for normal mode
  // With PaDac enabled: Pout = 20 - (15 - OutputPower), range 5..20 dBm
  standby();
  if (dbm > 20) dbm = 20;
  if (dbm > 17) {
    // Enable +20 dBm mode
    writeReg(SX_REG_PA_DAC, 0x87);
    writeReg(SX_REG_OCP, 0x3B);  // 240 mA
    if (dbm < 5) dbm = 5;
    writeReg(SX_REG_PA_CONFIG, 0x80 | (dbm - 5));
  } else {
    // Normal PA_BOOST mode: 2..17 dBm
    writeReg(SX_REG_PA_DAC, 0x84);
    writeReg(SX_REG_OCP, 0x2B);  // 100 mA
    if (dbm < 2) dbm = 2;
    writeReg(SX_REG_PA_CONFIG, 0x80 | (dbm - 2));
  }
}

void SX1276FSK::setBitrate(uint32_t bps) {
  uint16_t br = SX_FXOSC / bps;
  standby();
  writeReg(SX_REG_BITRATE_MSB, (br >> 8) & 0xFF);
  writeReg(SX_REG_BITRATE_LSB, br & 0xFF);
}

void SX1276FSK::setDeviation(uint32_t hz) {
  uint16_t fdev = (uint16_t)(hz / SX_FSTEP);
  standby();
  writeReg(SX_REG_FDEV_MSB, (fdev >> 8) & 0x3F);
  writeReg(SX_REG_FDEV_LSB, fdev & 0xFF);
}

// --- Transmit ---

bool SX1276FSK::send(const uint8_t *data, uint8_t len) {
  if (!sendStart(data, len)) return false;
  // Block until packet sent
  uint32_t t0 = millis();
  while (!sendDone()) {
    if (millis() - t0 > 100) {  // 100ms timeout
      standby();
      return false;
    }
  }
  return true;
}

bool SX1276FSK::sendStart(const uint8_t *data, uint8_t len) {
  if (len == 0 || len > SX_MAX_PAYLOAD) return false;

  standby();

  // Clear FIFO
  writeReg(SX_REG_IRQ_FLAGS2, SX_IRQ2_FIFO_OVERRUN);

  // Write length byte + payload to FIFO in one burst
  SPI.beginTransaction(SX_SPI);
  digitalWrite(_cs, LOW);
  SPI.transfer(SX_REG_FIFO | 0x80);  // write burst to FIFO
  SPI.transfer(len);                   // length prefix (variable mode)
  for (uint8_t i = 0; i < len; i++)
    SPI.transfer(data[i]);
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();

  // Start TX
  setMode(SX_MODE_TX);
  _inTx = true;
  _inRx = false;
  return true;
}

bool SX1276FSK::sendDone() {
  if (!_inTx) return true;
  if (readReg(SX_REG_IRQ_FLAGS2) & SX_IRQ2_PACKET_SENT) {
    standby();
    _inTx = false;
    return true;
  }
  return false;
}

// --- Receive ---

void SX1276FSK::startRx() {
  if (_inRx) return;
  standby();
  // Clear FIFO
  writeReg(SX_REG_IRQ_FLAGS2, SX_IRQ2_FIFO_OVERRUN);
  setMode(SX_MODE_RX);
  _inRx = true;
  _inTx = false;
}

bool SX1276FSK::available() {
  if (!_inRx) return false;
  return (readReg(SX_REG_IRQ_FLAGS2) & SX_IRQ2_PAYLOAD_READY) != 0;
}

uint8_t SX1276FSK::receive(uint8_t *buf, uint8_t maxLen) {
  if (!available()) return 0;

  // Read RSSI (captured at preamble detect)
  _lastRssi = -(readReg(SX_REG_RSSI_VALUE) / 2);

  // In variable-length mode, first FIFO byte is the length
  uint8_t len = readReg(SX_REG_FIFO);
  if (len > SX_MAX_PAYLOAD) len = SX_MAX_PAYLOAD;
  if (len > maxLen) len = maxLen;

  // Read payload
  readBurst(SX_REG_FIFO, buf, len);

  // Radio auto-restarts RX (SyncConfig AutoRestartRxMode)
  return len;
}

int16_t SX1276FSK::lastRssi() {
  return _lastRssi;
}
