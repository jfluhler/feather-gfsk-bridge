// =============================================================================
// Feather GFSK Bridge — Wireless Serial Receiver
// =============================================================================
// Receives 250 kbps GFSK radio frames and forwards the payload bytes to the
// PC via USB Serial. Works with both RAW and FORMAT-aware TX modes — the
// receiver is transparent, it just forwards whatever bytes arrive.
//
// USB Serial Commands (115200 baud):
//   ?           Show status and help
//   d           Toggle debug hex dump
//
// Link test frames (0xFE marker) are auto-detected and reported.
// Keepalive frames (0xFD marker) are tracked for link status.
//
// Hardware: Adafruit Feather M0 LoRa 900MHz (ATSAMD21 + RFM95W)
// =============================================================================

#include <SPI.h>
#include "SX1276FSK.h"

// --- Feather M0 LoRa pin assignments ---
#define RFM95_CS   8
#define RFM95_RST  4
#define LED        13

// --- Radio ---
SX1276FSK radio(RFM95_CS, RFM95_RST);

// --- Receive buffer ---
uint8_t rxBuf[SX_MAX_PAYLOAD];

// --- Stats ---
uint32_t framesRx = 0;
uint32_t bytesRx = 0;
uint32_t lastStatusMs = 0;
bool debugMode = false;

// --- Link test & throughput test (auto-detect) ---
#define TEST_MARKER      0xFE
#define THROUGHPUT_MARKER 0xFC
#define KEEPALIVE_MARKER  0xFD
#define TEST_IDLE_MS     3000  // report after 3s of no test frames
bool testActive = false;
uint32_t testStartMs = 0;
uint32_t testLastMs = 0;
uint16_t testFrames = 0;
uint32_t testBytes = 0;
int32_t testRssiSum = 0;
uint16_t testSeqMax = 0;
bool testSeqInit = false;
bool testIsThroughput = false;

// --- Keepalive tracking ---
uint32_t lastKeepAliveMs = 0;
int16_t keepAliveRssi = 0;
bool keepAliveActive = false;

void printHex(uint8_t b) {
  if (b < 0x10) Serial.print('0');
  Serial.print(b, HEX);
}

void setup() {
  pinMode(LED, OUTPUT);

  Serial.begin(115200);
  while (!Serial) {
    delay(1);
  }

  if (!radio.init()) {
    Serial.println("SX1276 GFSK init failed!");
    while (1) {
      digitalWrite(LED, HIGH); delay(100);
      digitalWrite(LED, LOW);  delay(100);
    }
  }

  radio.startRx();

  Serial.println("Feather GFSK Bridge RX ready");
  Serial.println("Radio: 250 kbps GFSK, 915 MHz");
  Serial.println("Send '?' for help");
  digitalWrite(LED, HIGH);
  delay(500);
  digitalWrite(LED, LOW);
}

void loop() {
  // --- USB serial commands ---
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'd' || c == 'D') {
      debugMode = !debugMode;
      Serial.print("# Debug: "); Serial.println(debugMode ? "ON" : "OFF");
    } else if (c == '?') {
      Serial.println("# --- Feather GFSK Bridge RX ---");
      Serial.println("#   Radio: 250 kbps GFSK, 915 MHz");
      Serial.print("#   Debug: "); Serial.println(debugMode ? "ON" : "OFF");
      Serial.print("#   Frames: "); Serial.println(framesRx);
      Serial.print("#   Bytes:  "); Serial.println(bytesRx);
      Serial.print("#   RSSI:   "); Serial.print(radio.lastRssi()); Serial.println(" dBm");
      if (keepAliveActive) {
        uint32_t age = (millis() - lastKeepAliveMs) / 1000;
        Serial.print("#   Link:   UP (last ping "); Serial.print(age); Serial.print("s ago, RSSI=");
        Serial.print(keepAliveRssi); Serial.println(" dBm)");
      } else {
        Serial.println("#   Link:   no keepalive received");
      }
      Serial.println("#   Commands: d=debug, ?=help");
      Serial.println("#   Link test: auto-detected when TX sends 't'");
    }
  }

  // --- Receive radio frames ---
  if (radio.available()) {
    uint8_t len = radio.receive(rxBuf, sizeof(rxBuf));

    if (len > 0) {
      int16_t rssi = radio.lastRssi();
      framesRx++;
      bytesRx += len;
      digitalWrite(LED, HIGH);

      // Check for keepalive frame
      if (len >= 4 && rxBuf[0] == KEEPALIVE_MARKER) {
        lastKeepAliveMs = millis();
        keepAliveRssi = rssi;
        keepAliveActive = true;
        // Don't forward keepalives to serial output
      }
      // Check for test/throughput frame (auto-detect)
      else if (len >= 3 && (rxBuf[0] == TEST_MARKER || rxBuf[0] == THROUGHPUT_MARKER)) {
        bool isThroughput = (rxBuf[0] == THROUGHPUT_MARKER);
        if (!testActive) {
          // First test frame — start collecting
          testActive = true;
          testIsThroughput = isThroughput;
          testStartMs = millis();
          testFrames = 0;
          testBytes = 0;
          testRssiSum = 0;
          testSeqMax = 0;
          testSeqInit = false;
          Serial.print("# --- ");
          Serial.print(isThroughput ? "Throughput" : "Link");
          Serial.println(" Test: receiving ---");
        }
        uint16_t seq = ((uint16_t)rxBuf[1] << 8) | rxBuf[2];
        testFrames++;
        testBytes += len;
        testRssiSum += rssi;
        testLastMs = millis();
        if (!testSeqInit || seq > testSeqMax) {
          testSeqMax = seq;
          testSeqInit = true;
        }
        // Don't forward test frames to serial output
      }
      else {
        // Forward normal payload to USB serial
        Serial.write(rxBuf, len);
      }

      // Debug: hex dump (prefixed with # so it's distinguishable)
      if (debugMode) {
        Serial.print("\n# RX ["); Serial.print(len); Serial.print("B RSSI=");
        Serial.print(rssi); Serial.print("]: ");
        for (uint8_t i = 0; i < len; i++) {
          printHex(rxBuf[i]);
          Serial.print(' ');
        }
        Serial.println();
      }

      digitalWrite(LED, LOW);
    }
  }

  // --- Test auto-report (3s after last test frame) ---
  if (testActive && (millis() - testLastMs >= TEST_IDLE_MS)) {
    testActive = false;
    Serial.print("# --- ");
    Serial.print(testIsThroughput ? "Throughput" : "Link");
    Serial.println(" Test Results ---");
    Serial.print("#   Frames received: "); Serial.println(testFrames);
    if (testSeqInit) {
      uint16_t expected = testSeqMax + 1;
      uint16_t lost = (expected > testFrames) ? expected - testFrames : 0;
      float lossP = (expected > 0) ? (lost * 100.0f / expected) : 0;
      Serial.print("#   Frames expected: "); Serial.println(expected);
      Serial.print("#   Packet loss:     "); Serial.print(lossP, 1); Serial.println(" %");
    }
    if (testFrames > 0) {
      int16_t avgRssi = (int16_t)(testRssiSum / testFrames);
      Serial.print("#   Avg RSSI:        "); Serial.print(avgRssi); Serial.println(" dBm");
      uint32_t elapsed = testLastMs - testStartMs;
      uint32_t bps = (elapsed > 0) ? (testBytes * 1000UL / elapsed) : 0;
      Serial.print("#   Throughput:      "); Serial.print(bps); Serial.print(" B/s");
      if (testIsThroughput) {
        Serial.print(" ("); Serial.print(bps * 8UL / 1000UL); Serial.print(" kbps)");
      }
      Serial.println();
      Serial.print("#   Total:           "); Serial.print(testBytes); Serial.print(" bytes in ");
      Serial.print(elapsed); Serial.println(" ms");
    } else {
      Serial.println("#   No test frames received — check TX is in range");
    }
    Serial.println("# --------------------------");
  }

  // --- Keepalive timeout check (2 minutes = ~4 missed pings at 30s) ---
  if (keepAliveActive && (millis() - lastKeepAliveMs > 120000)) {
    keepAliveActive = false;
    Serial.println("# WARN: keepalive lost (TX out of range or powered off)");
  }

  // --- Periodic stats (every 5 seconds) ---
  if (millis() - lastStatusMs > 5000) {
    lastStatusMs = millis();
    Serial.print("# RX: frames="); Serial.print(framesRx);
    Serial.print(" bytes="); Serial.print(bytesRx);
    Serial.print(" RSSI="); Serial.print(radio.lastRssi());
    if (keepAliveActive) {
      Serial.print(" link=UP");
    }
    Serial.println(" dBm");
  }
}
