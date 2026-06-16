/*
  MHpower – diagnostika sběrnice TM1640 (ESP32).
  Nahraj místo ostrého firmware, otevři Serial Monitor na 115200.

  Vyhodnotí, co reálně dorazí na GPIO18 a GPIO23:
    - kolik hran a jaký "high %" na každém pinu  -> živý / mrtvý / kmitající
    - syrové bajty pro obě přiřazení hodin (18 i 23) a obě hrany/inverze
      -> hledáme posloupnost 40 C0 .. .. (8x) .. 8x = platný rámec
*/

#include <Arduino.h>
#include <soc/gpio_struct.h>

const int PIN_A = 18;   // VSPI_SCK – ve firmware "CLK"
const int PIN_B = 23;   // VSPI_MOSI – ve firmware "DIN"
const uint32_t N = 18000;
uint8_t samples[N];

static inline uint8_t readPins() {
  const uint32_t v = GPIO.in;
  const uint8_t a = (v >> PIN_A) & 1;
  const uint8_t b = (v >> PIN_B) & 1;
  return a | (b << 1);          // bit0 = GPIO18, bit1 = GPIO23
}

bool waitActivity(uint32_t timeoutMs) {
  const uint32_t t0 = millis();
  const uint8_t last = readPins();
  while (millis() - t0 < timeoutMs) {
    if (readPins() != last) return true;
  }
  return false;
}

void capture() {
  noInterrupts();
  for (uint32_t i = 0; i < N; i++) samples[i] = readPins();
  interrupts();
}

// zkus: clockBit = který bit jsou hodiny (0=GPIO18,1=GPIO23), dataBit = druhý
void dumpVariant(uint8_t clockBit, uint8_t dataBit, bool fallingEdge, bool invert) {
  uint8_t bits[260];
  int nb = 0;
  uint8_t lc = (samples[0] >> clockBit) & 1;
  for (uint32_t i = 1; i < N && nb < 256; i++) {
    const uint8_t c = (samples[i] >> clockBit) & 1;
    const bool hit = fallingEdge ? (lc && !c) : (!lc && c);
    if (hit) {
      uint8_t d = (samples[i] >> dataBit) & 1;
      if (invert) d ^= 1;
      bits[nb++] = d;
    }
    lc = c;
  }
  Serial.printf("  CLK=GPIO%d %s inv=%d  bits=%3d :", clockBit ? 23 : 18,
                fallingEdge ? "fall" : "rise", invert, nb);
  for (int k = 0; k + 8 <= nb && k < 8 * 22; k += 8) {
    uint8_t v = 0;
    for (int n = 0; n < 8; n++) v |= (bits[k + n] & 1) << n;   // LSB first
    Serial.printf(" %02X", v);
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(400);
  pinMode(PIN_A, INPUT);
  pinMode(PIN_B, INPUT);
  Serial.println("\n=== MHpower bus diagnostika (GPIO18 / GPIO23) ===");
}

void loop() {
  const bool act = waitActivity(250);
  capture();

  uint32_t aEdges = 0, bEdges = 0, aHigh = 0, bHigh = 0;
  uint8_t la = samples[0] & 1, lb = (samples[0] >> 1) & 1;
  for (uint32_t i = 0; i < N; i++) {
    const uint8_t a = samples[i] & 1, b = (samples[i] >> 1) & 1;
    if (a != la) { aEdges++; la = a; }
    if (b != lb) { bEdges++; lb = b; }
    if (a) aHigh++;
    if (b) bHigh++;
  }

  Serial.printf("\n[%s] GPIO18: edges=%5u high=%2u%%   GPIO23: edges=%5u high=%2u%%\n",
                act ? "akt" : "KLID", aEdges, aHigh * 100 / N, bEdges, bHigh * 100 / N);

  // bajty pro obě přiřazení hodin a obě hrany, bez inverze i s inverzí dat
  dumpVariant(0, 1, false, false);
  dumpVariant(0, 1, true,  true);
  dumpVariant(1, 0, false, false);
  dumpVariant(1, 0, true,  true);

  delay(1200);
}
