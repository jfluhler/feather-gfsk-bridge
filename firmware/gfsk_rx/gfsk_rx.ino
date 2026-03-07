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
      Serial.println("#   Commands: d=debug, ?=help");
    }
  }

  // --- Receive radio frames ---
  if (radio.available()) {
    uint8_t len = radio.receive(rxBuf, sizeof(rxBuf));

    if (len > 0) {
      framesRx++;
      bytesRx += len;
      digitalWrite(LED, HIGH);

      // Forward raw payload to USB serial
      Serial.write(rxBuf, len);

      // Debug: hex dump to USB serial (prefixed with # so it's distinguishable)
      if (debugMode) {
        Serial.print("\n# RX ["); Serial.print(len); Serial.print("B]: ");
        for (uint8_t i = 0; i < len; i++) {
          printHex(rxBuf[i]);
          Serial.print(' ');
        }
        Serial.println();
      }

      digitalWrite(LED, LOW);
    }
  }

  // --- Periodic stats (every 5 seconds) ---
  if (millis() - lastStatusMs > 5000) {
    lastStatusMs = millis();
    Serial.print("# RX: frames="); Serial.print(framesRx);
    Serial.print(" bytes="); Serial.print(bytesRx);
    Serial.print(" RSSI="); Serial.print(radio.lastRssi());
    Serial.println(" dBm");
  }
}
