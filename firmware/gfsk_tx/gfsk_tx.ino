// =============================================================================
// Feather GFSK Bridge — Wireless Serial Transmitter
// =============================================================================
// Receives serial data on UART1 (pins 0/1) and relays it over 250 kbps GFSK
// radio to a remote receiver using the SX1276/RFM95W transceiver.
//
// Two operating modes:
//   RAW         Raw byte passthrough with time-based batching
//   FORMAT      Format-aware: validates packets by header/length/checksum
//               before transmitting (reduces wasted radio bandwidth)
//
// USB Serial Commands (115200 baud):
//   ?           Show current settings and help
//   d           Toggle debug output
//   b <rate>    Set UART1 baud rate (e.g. "b 1000000")
//   m           Toggle mode (RAW / FORMAT)
//   s           Save settings to flash
//   r           Reset to defaults
//
// Hardware: Adafruit Feather M0 LoRa 900MHz (ATSAMD21 + RFM95W)
// =============================================================================

#include <SPI.h>
#include <FlashStorage.h>
#include "SX1276FSK.h"

// --- Feather M0 LoRa pin assignments ---
#define RFM95_CS   8
#define RFM95_RST  4
#define LED        13

// --- Operating modes ---
#define MODE_RAW    0
#define MODE_FORMAT 1

// --- Configuration defaults ---
#define DEFAULT_UART_BAUD  1000000
#define DEFAULT_MODE       MODE_FORMAT

// --- Format-Aware packet table ---
// Define known packet formats: {header_byte, total_length_including_checksum}
// The last byte of each packet is assumed to be an XOR checksum.
// Add/remove entries for your protocol. Set FORMAT_COUNT to match.
#define FORMAT_COUNT 4
struct PacketFormat {
  uint8_t header;
  uint8_t length;
};

const PacketFormat formats[FORMAT_COUNT] = {
  { 0xA5, 12 },  // Normal event (4B timestamp + 6B state + checksum)
  { 0xB5,  8 },  // Low-data event (4B timestamp + channel + state + checksum)
  { 0xC5, 10 },  // Reduced summary (channel + 4B timestamp + 2B count + state + checksum)
  { 0xD5,  9 },  // Status/ACK (status + mode + 3B version + 2B ADC + checksum)
};

// --- Persistent settings ---
#define SETTINGS_MAGIC 0xBF02

struct Settings {
  uint16_t magic;
  uint32_t uartBaud;
  uint8_t  mode;  // MODE_RAW or MODE_FORMAT
};

FlashStorage(savedSettings, Settings);
Settings config;

// --- Radio ---
SX1276FSK radio(RFM95_CS, RFM95_RST);

// --- Circular buffer ---
// 8 KB ring buffer absorbs UART bursts while radio is transmitting.
#define RING_SIZE 8192
uint8_t ring[RING_SIZE];
volatile uint16_t ringHead = 0;
volatile uint16_t ringTail = 0;

uint16_t ringUsed() {
  return (ringHead - ringTail + RING_SIZE) % RING_SIZE;
}

uint16_t ringFree() {
  return RING_SIZE - 1 - ringUsed();
}

void ringWrite(const uint8_t *data, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    ring[ringHead] = data[i];
    ringHead = (ringHead + 1) % RING_SIZE;
  }
}

void ringWriteByte(uint8_t b) {
  ring[ringHead] = b;
  ringHead = (ringHead + 1) % RING_SIZE;
}

uint8_t ringPeek(uint16_t offset) {
  return ring[(ringTail + offset) % RING_SIZE];
}

void ringAdvance(uint16_t n) {
  ringTail = (ringTail + n) % RING_SIZE;
}

// --- Radio TX frame buffer ---
uint8_t txBuf[SX_MAX_PAYLOAD];

// --- Format-aware parser state ---
enum ParseState { WAIT_HEADER, READ_PAYLOAD };
ParseState parseState = WAIT_HEADER;
uint8_t pktBuf[16];  // largest expected packet
uint8_t pktIdx = 0;
uint8_t pktExpectedLen = 0;

// --- Stats ---
uint32_t framesSent = 0;
uint32_t pktsBuffered = 0;
uint32_t pktsDropped = 0;
uint32_t peakUsed = 0;
uint32_t lastStatusMs = 0;
bool debugMode = false;

// --- Raw mode timing ---
uint32_t lastByteMs = 0;
#define RAW_BATCH_TIMEOUT_MS 2  // flush after 2ms of silence

// --- USB command buffer ---
char cmdBuf[32];
uint8_t cmdLen = 0;

// --- Format-aware helpers ---

uint8_t formatPacketLen(uint8_t header) {
  for (uint8_t i = 0; i < FORMAT_COUNT; i++) {
    if (formats[i].header == header) return formats[i].length;
  }
  return 0;
}

bool verifyXorChecksum(const uint8_t *buf, uint8_t len) {
  uint8_t xorVal = 0;
  for (uint8_t i = 0; i < len; i++)
    xorVal ^= buf[i];
  return xorVal == 0;
}

void printHex(uint8_t b) {
  if (b < 0x10) Serial.print('0');
  Serial.print(b, HEX);
}

void debugPacket(const uint8_t *buf, uint8_t len) {
  if (!debugMode) return;
  Serial.print('[');
  for (uint8_t i = 0; i < len; i++) {
    if (i > 0) Serial.print(' ');
    printHex(buf[i]);
  }
  Serial.println(']');
}

// --- Enqueue to ring buffer ---

void enqueueBytes(const uint8_t *buf, uint8_t len) {
  if (len > ringFree()) {
    pktsDropped++;
    return;
  }
  ringWrite(buf, len);
  pktsBuffered++;
  uint16_t used = ringUsed();
  if (used > peakUsed) peakUsed = used;
}

// --- Drain ring to radio ---

void drainFormatAware() {
  // Pack complete packets from ring into radio frame
  if (!radio.sendDone()) return;

  uint16_t avail = ringUsed();
  if (avail == 0) return;

  uint8_t txLen = 0;
  uint16_t scanPos = 0;

  while (scanPos < avail) {
    uint8_t hdr = ringPeek(scanPos);
    uint8_t pLen = formatPacketLen(hdr);

    if (pLen == 0 || scanPos + pLen > avail) break;
    if (txLen + pLen > SX_MAX_PAYLOAD) break;

    for (uint8_t i = 0; i < pLen; i++)
      txBuf[txLen + i] = ringPeek(scanPos + i);

    txLen += pLen;
    scanPos += pLen;
  }

  if (txLen > 0) {
    ringAdvance(scanPos);
    radio.sendStart(txBuf, txLen);
    framesSent++;
    digitalWrite(LED, !digitalRead(LED));
  }
}

void drainRaw() {
  // Pack raw bytes from ring into radio frame
  if (!radio.sendDone()) return;

  uint16_t avail = ringUsed();
  if (avail == 0) return;

  // In raw mode, wait for a batch timeout before sending
  // (unless buffer is getting full)
  if (avail < SX_MAX_PAYLOAD && millis() - lastByteMs < RAW_BATCH_TIMEOUT_MS) return;

  uint8_t txLen = (avail > SX_MAX_PAYLOAD) ? SX_MAX_PAYLOAD : (uint8_t)avail;

  for (uint8_t i = 0; i < txLen; i++)
    txBuf[i] = ringPeek(i);

  ringAdvance(txLen);
  radio.sendStart(txBuf, txLen);
  framesSent++;
  digitalWrite(LED, !digitalRead(LED));
}

// --- Settings management ---

void loadDefaults() {
  config.magic = SETTINGS_MAGIC;
  config.uartBaud = DEFAULT_UART_BAUD;
  config.mode = DEFAULT_MODE;
}

void loadSettings() {
  config = savedSettings.read();
  if (config.magic != SETTINGS_MAGIC) {
    Serial.println("No saved settings, using defaults");
    loadDefaults();
  } else {
    Serial.println("Settings loaded from flash");
  }
}

void saveSettings() {
  config.magic = SETTINGS_MAGIC;
  savedSettings.write(config);
  Serial.println("Settings saved to flash");
}

const char* modeName() {
  return config.mode == MODE_FORMAT ? "FORMAT" : "RAW";
}

void printSettings() {
  Serial.println("--- Feather GFSK Bridge ---");
  Serial.print("  UART1 baud: "); Serial.println(config.uartBaud);
  Serial.print("  TX mode:    "); Serial.println(modeName());
  Serial.println("  Radio:      250 kbps GFSK, 915 MHz, +20 dBm");
  Serial.print("  Debug:      "); Serial.println(debugMode ? "ON" : "OFF");
  if (config.mode == MODE_FORMAT) {
    Serial.println("  Packet formats:");
    for (uint8_t i = 0; i < FORMAT_COUNT; i++) {
      Serial.print("    0x"); printHex(formats[i].header);
      Serial.print(" -> "); Serial.print(formats[i].length);
      Serial.println(" bytes");
    }
  }
  Serial.println("--- Commands ---");
  Serial.println("  ?           Show this help");
  Serial.println("  d           Toggle debug output");
  Serial.println("  b <rate>    Set UART1 baud (e.g. b 1000000)");
  Serial.println("  m           Toggle mode (RAW/FORMAT)");
  Serial.println("  s           Save settings to flash");
  Serial.println("  r           Reset to defaults");
}

void applyUartBaud() {
  Serial1.end();
  Serial1.begin(config.uartBaud);
  parseState = WAIT_HEADER;
  pktIdx = 0;
  ringHead = 0;
  ringTail = 0;
  Serial.print("UART1 set to "); Serial.print(config.uartBaud); Serial.println(" baud");
}

// --- USB command processing ---

void processCommand(const char *cmd) {
  while (*cmd == ' ') cmd++;

  if (cmd[0] == 'd' || cmd[0] == 'D') {
    debugMode = !debugMode;
    Serial.print("Debug: "); Serial.println(debugMode ? "ON" : "OFF");
  }
  else if (cmd[0] == '?') {
    printSettings();
  }
  else if (cmd[0] == 's' || cmd[0] == 'S') {
    saveSettings();
  }
  else if (cmd[0] == 'r' || cmd[0] == 'R') {
    loadDefaults();
    applyUartBaud();
    Serial.println("Reset to defaults (use 's' to save)");
  }
  else if (cmd[0] == 'm' || cmd[0] == 'M') {
    config.mode = (config.mode == MODE_FORMAT) ? MODE_RAW : MODE_FORMAT;
    // Reset parser state when switching modes
    parseState = WAIT_HEADER;
    pktIdx = 0;
    ringHead = 0;
    ringTail = 0;
    Serial.print("Mode: "); Serial.println(modeName());
    Serial.println("Use 's' to save");
  }
  else if (cmd[0] == 'b' || cmd[0] == 'B') {
    const char *p = cmd + 1;
    while (*p == ' ') p++;
    uint32_t baud = strtoul(p, NULL, 10);
    if (baud >= 9600 && baud <= 5000000) {
      config.uartBaud = baud;
      applyUartBaud();
      Serial.println("Use 's' to save");
    } else {
      Serial.println("Invalid baud (9600-5000000)");
    }
  }
  else if (cmd[0] != '\0') {
    Serial.print("Unknown: "); Serial.println(cmd);
    Serial.println("Send '?' for help");
  }
}

// --- Arduino setup/loop ---

void setup() {
  pinMode(LED, OUTPUT);
  Serial.begin(115200);

  loadSettings();
  Serial1.begin(config.uartBaud);

  if (!radio.init()) {
    Serial.println("SX1276 GFSK init failed!");
    while (1) {
      digitalWrite(LED, HIGH); delay(100);
      digitalWrite(LED, LOW);  delay(100);
    }
  }

  Serial.println("Feather GFSK Bridge TX ready");
  Serial.print("Mode: "); Serial.println(modeName());
  Serial.print("UART1: "); Serial.print(config.uartBaud); Serial.println(" baud");
  Serial.println("Radio: 250 kbps GFSK, 915 MHz, +20 dBm");
  Serial.println("Send '?' for help");
  digitalWrite(LED, HIGH);
  delay(500);
  digitalWrite(LED, LOW);
}

void loop() {
  // --- USB serial commands ---
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen > 0) {
        cmdBuf[cmdLen] = '\0';
        processCommand(cmdBuf);
        cmdLen = 0;
      }
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    }
    if (cmdLen == 1 && (cmdBuf[0] == 'd' || cmdBuf[0] == 'D' ||
                        cmdBuf[0] == '?' || cmdBuf[0] == 's' ||
                        cmdBuf[0] == 'S' || cmdBuf[0] == 'r' ||
                        cmdBuf[0] == 'R' || cmdBuf[0] == 'm' ||
                        cmdBuf[0] == 'M')) {
      cmdBuf[1] = '\0';
      processCommand(cmdBuf);
      cmdLen = 0;
    }
  }

  // --- Read UART1 ---
  if (config.mode == MODE_FORMAT) {
    // Format-aware: parse and validate packets
    while (Serial1.available()) {
      uint8_t b = Serial1.read();

      switch (parseState) {
        case WAIT_HEADER: {
          uint8_t len = formatPacketLen(b);
          if (len > 0) {
            pktBuf[0] = b;
            pktIdx = 1;
            pktExpectedLen = len;
            parseState = READ_PAYLOAD;
          }
          break;
        }
        case READ_PAYLOAD:
          pktBuf[pktIdx++] = b;
          if (pktIdx >= pktExpectedLen) {
            if (verifyXorChecksum(pktBuf, pktExpectedLen)) {
              enqueueBytes(pktBuf, pktExpectedLen);
              debugPacket(pktBuf, pktExpectedLen);
            }
            parseState = WAIT_HEADER;
          }
          break;
      }
    }
    drainFormatAware();
  } else {
    // Raw passthrough: buffer all bytes
    while (Serial1.available()) {
      uint8_t b = Serial1.read();
      if (ringFree() > 0) {
        ringWriteByte(b);
        lastByteMs = millis();
      } else {
        pktsDropped++;
      }
      uint16_t used = ringUsed();
      if (used > peakUsed) peakUsed = used;
    }
    drainRaw();
  }

  // --- Periodic stats (every 5 seconds) ---
  if (millis() - lastStatusMs > 5000) {
    lastStatusMs = millis();
    Serial.print("TX ["); Serial.print(modeName());
    Serial.print("]: sent="); Serial.print(framesSent);
    Serial.print(" buf="); Serial.print(pktsBuffered);
    Serial.print(" drop="); Serial.print(pktsDropped);
    Serial.print(" ring="); Serial.print(ringUsed());
    Serial.print("/"); Serial.print(RING_SIZE);
    Serial.print(" peak="); Serial.println(peakUsed);
  }
}
