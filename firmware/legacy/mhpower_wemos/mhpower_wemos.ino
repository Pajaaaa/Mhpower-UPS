/*
  MHpower MPU-500-12 display monitor for Wemos D1 mini / ESP8266.

  Passive sniffer for the TM1640 display bus:
    CLK -> D5 / GPIO14
    DIN -> D6 / GPIO12
    GND -> shared GND

  Important: TM1640 is probably 5 V logic. Use a level shifter or resistor
  divider before connecting CLK/DIN to the ESP8266.
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <eagle_soc.h>
#include <gpio.h>

// ----------------------- User settings -----------------------

const char* WIFI_SSID = "WIFI_SSID";       // nastav před flashem
const char* WIFI_PASS = "WIFI_PASSWORD";   // nastav před flashem

// Start carefully: keep Wi-Fi disabled until the serial decode is verified.
// Set to true later to enable the web UI and /api/status endpoint.
const bool ENABLE_WIFI = false;
const bool USE_POLLING_CAPTURE = false;
const bool PIN_AUTODETECT_DEBUG = false;

const uint8_t CLK_GPIO = 12; // D6
const uint8_t DIN_GPIO = 14; // D5

// A TM1640 update burst repeats about every 500 ms. Inside one burst the gaps
// between clock edges are tiny; between bursts they are huge.
const uint32_t BURST_GAP_US = 100;
const uint32_t FRAME_STALE_MS = 2000;

// ----------------------- Raw bit capture -----------------------

struct BitEvent {
  uint32_t t;
  uint8_t bit;
};

const uint16_t RING_SIZE = 512;
volatile BitEvent ringBuf[RING_SIZE];
volatile uint16_t ringHead = 0;
volatile uint16_t ringTail = 0;
volatile uint32_t ringOverflow = 0;
volatile uint32_t clockEdges = 0;
volatile uint32_t lastClockUs = 0;
volatile uint32_t dinChanges = 0;
volatile uint32_t lastDinChangeUs = 0;
volatile uint32_t d5Changes = 0;
volatile uint32_t d6Changes = 0;
volatile uint32_t d5Rises = 0;
volatile uint32_t d6Rises = 0;
uint8_t lastPolledClk = 1;
uint8_t lastPolledDin = 1;
uint16_t pollIdleCount = 0;
const uint16_t POLL_BURST_GAP_COUNT = 2000;

void pushCapturedBit(uint8_t bit, uint32_t t) {
  const uint16_t next = (ringHead + 1) & (RING_SIZE - 1);
  if (next == ringTail) {
    ringOverflow++;
    return;
  }

  ringBuf[ringHead].t = t;
  ringBuf[ringHead].bit = bit;
  ringHead = next;
}

void ICACHE_RAM_ATTR onClkRise() {
  const uint32_t now = micros();
  clockEdges++;
  lastClockUs = now;
  pushCapturedBit((GPIO_REG_READ(GPIO_IN_ADDRESS) & (1 << DIN_GPIO)) ? 1 : 0, now);
}

void ICACHE_RAM_ATTR onDinChangeDebug() {
  dinChanges++;
  lastDinChangeUs = micros();
}

void ICACHE_RAM_ATTR onD5ChangeDebug() {
  static uint8_t last = 0;
  const uint8_t now = (GPIO_REG_READ(GPIO_IN_ADDRESS) & (1 << 14)) ? 1 : 0;
  d5Changes++;
  if (!last && now) d5Rises++;
  last = now;
}

void ICACHE_RAM_ATTR onD6ChangeDebug() {
  static uint8_t last = 0;
  const uint8_t now = (GPIO_REG_READ(GPIO_IN_ADDRESS) & (1 << 12)) ? 1 : 0;
  d6Changes++;
  if (!last && now) d6Rises++;
  last = now;
}

extern uint8_t burstBits[160];
extern uint16_t burstLen;
void parseBurst();

void pollPinsFast() {
  const uint32_t pins = GPIO_REG_READ(GPIO_IN_ADDRESS);
  const uint8_t clk = (pins & (1 << CLK_GPIO)) ? 1 : 0;
  const uint8_t din = (pins & (1 << DIN_GPIO)) ? 1 : 0;

  if (din != lastPolledDin) {
    dinChanges++;
    lastPolledDin = din;
  }

  if (!lastPolledClk && clk) {
    clockEdges++;
    pollIdleCount = 0;
    if (burstLen < sizeof(burstBits)) {
      burstBits[burstLen++] = din;
    } else {
      ringOverflow++;
      burstLen = 0;
    }
  } else if (USE_POLLING_CAPTURE && burstLen > 0 && pollIdleCount < POLL_BURST_GAP_COUNT) {
    pollIdleCount++;
    if (pollIdleCount == POLL_BURST_GAP_COUNT) {
      lastClockUs = micros();
      parseBurst();
    }
  }
  lastPolledClk = clk;
}

bool popBit(BitEvent& ev) {
  noInterrupts();
  if (ringTail == ringHead) {
    interrupts();
    return false;
  }
  ev.t = ringBuf[ringTail].t;
  ev.bit = ringBuf[ringTail].bit;
  ringTail = (ringTail + 1) & (RING_SIZE - 1);
  interrupts();
  return true;
}

// ----------------------- Decoded state -----------------------

struct DisplayState {
  uint8_t mem[10] = {0};
  uint8_t brightnessCommand = 0;
  uint32_t frameCount = 0;
  uint32_t lastFrameMs = 0;
  uint32_t decodeErrors = 0;
  uint32_t overflows = 0;

  int inputVoltage = -1;
  int outputVoltage = -1;
  bool displayBlank = false;
  bool mainsPresent = false;
  bool onBattery = false;
  bool alarm = false;
  bool lowBattery = false;
  bool overload = false;
  bool charging = false;
  bool batteryFull = false;
  uint8_t batteryBars = 255;
  uint8_t loadLevel = 255;
};

DisplayState state;

uint8_t batteryHistory[8] = {0};
uint8_t batteryHistoryPos = 0;
uint8_t batteryHistoryCount = 0;

int digitFromPattern(uint8_t p) {
  switch (p) {
    case 0x77: return 0;
    case 0x41: return 1;
    case 0x6E: return 2;
    case 0x6D: return 3;
    case 0x59: return 4;
    case 0x3D: return 5;
    case 0x3F: return 6;
    case 0x61: return 7;
    case 0xFF: return 8;
    case 0x00: return -2; // blank
    default: return -1;   // unknown
  }
}

int decode3Digits(const uint8_t* p) {
  const int a = digitFromPattern(p[0]);
  const int b = digitFromPattern(p[1]);
  const int c = digitFromPattern(p[2]);

  if (a == -2 && b == -2 && c == -2) return -2;
  if (a < 0 || b < 0 || c < 0) return -1;
  return a * 100 + b * 10 + c;
}

uint8_t batteryBarsFrom09(uint8_t v) {
  switch (v) {
    case 0x10: return 0;
    case 0x30: return 1;
    case 0x70: return 2;
    case 0x71: return 3;
    case 0x75: return 4;
    case 0x77: return 5;
    default: return 255;
  }
}

uint8_t loadLevelFrom07(uint8_t v) {
  switch (v) {
    case 0x20: return 0;
    case 0x60: return 1;
    case 0x61: return 2;
    case 0x7F: return 5;
    default: return 255;
  }
}

void rememberBatteryPattern(uint8_t v) {
  batteryHistory[batteryHistoryPos] = v;
  batteryHistoryPos = (batteryHistoryPos + 1) % sizeof(batteryHistory);
  if (batteryHistoryCount < sizeof(batteryHistory)) batteryHistoryCount++;
}

bool batteryPatternIsChanging() {
  if (batteryHistoryCount < 4) return false;
  const uint8_t first = batteryHistory[0];
  for (uint8_t i = 1; i < batteryHistoryCount; i++) {
    if (batteryHistory[i] != first) return true;
  }
  return false;
}

void updateDecodedState(const uint8_t* mem, uint8_t brightness) {
  memcpy(state.mem, mem, 10);
  state.brightnessCommand = brightness;
  state.frameCount++;
  state.lastFrameMs = millis();
  state.overflows = ringOverflow;

  state.inputVoltage = decode3Digits(&state.mem[0]);
  state.outputVoltage = decode3Digits(&state.mem[3]);
  state.displayBlank = state.inputVoltage == -2 && state.outputVoltage == -2;

  const uint8_t mode = state.mem[6];
  state.onBattery = (mode & 0x40) != 0;
  state.mainsPresent = !state.onBattery;
  state.alarm = (mode & 0x04) != 0;

  state.batteryBars = batteryBarsFrom09(state.mem[9]);
  rememberBatteryPattern(state.mem[9]);
  state.charging = state.mainsPresent && batteryPatternIsChanging();
  state.batteryFull = state.mainsPresent && !state.charging && state.mem[9] == 0x77;
  state.lowBattery = state.onBattery && state.alarm &&
                     (state.mem[9] == 0x10 || state.mem[9] == 0x30);

  state.loadLevel = loadLevelFrom07(state.mem[7]);
  state.overload = state.mem[7] == 0x7F;
}

// ----------------------- TM1640 burst parser -----------------------

uint8_t burstBits[160];
uint16_t burstLen = 0;
uint32_t lastBitUs = 0;

uint8_t bitsToByte(const uint8_t* bits, uint16_t offset) {
  uint8_t value = 0;
  for (uint8_t i = 0; i < 8; i++) {
    value |= (bits[offset + i] & 1) << i;
  }
  return value;
}

void parseBurst() {
  if (burstLen < 24) {
    burstLen = 0;
    return;
  }

  uint8_t bytes[20];
  uint8_t byteCount = burstLen / 8;
  if (byteCount > sizeof(bytes)) byteCount = sizeof(bytes);
  for (uint8_t i = 0; i < byteCount; i++) {
    bytes[i] = bitsToByte(burstBits, i * 8);
  }

  // Normal full frame observed on the MHpower display:
  // 0x40, 0xC0, ten display RAM bytes, 0x8B.
  for (uint8_t i = 0; i + 12 < byteCount; i++) {
    if (bytes[i] == 0x40 && bytes[i + 1] == 0xC0 &&
        (bytes[i + 12] & 0xF0) == 0x80) {
      updateDecodedState(&bytes[i + 2], bytes[i + 12]);
      burstLen = 0;
      return;
    }
  }

  state.decodeErrors++;
  burstLen = 0;
}

void processCapturedBits() {
  BitEvent ev;
  while (popBit(ev)) {
    if (burstLen > 0 && (uint32_t)(ev.t - lastBitUs) > BURST_GAP_US) {
      parseBurst();
    }
    lastBitUs = ev.t;

    if (burstLen < sizeof(burstBits)) {
      burstBits[burstLen++] = ev.bit;
    } else {
      state.decodeErrors++;
      burstLen = 0;
    }
  }

  if (burstLen > 0 && (uint32_t)(micros() - lastBitUs) > BURST_GAP_US) {
    parseBurst();
  }
}

// ----------------------- Web output -----------------------

ESP8266WebServer server(80);

String hexByte(uint8_t v) {
  char buf[5];
  snprintf(buf, sizeof(buf), "%02X", v);
  return String(buf);
}

void appendJsonNumber(String& s, const char* key, int value) {
  s += "\"";
  s += key;
  s += "\":";
  if (value < 0) s += "null";
  else s += String(value);
}

void handleJson() {
  processCapturedBits();

  const bool online = (millis() - state.lastFrameMs) <= FRAME_STALE_MS;
  String s;
  s.reserve(900);
  s += "{";
  s += "\"online\":";
  s += online ? "true" : "false";
  s += ",";
  appendJsonNumber(s, "inputVoltage", state.inputVoltage);
  s += ",";
  appendJsonNumber(s, "outputVoltage", state.outputVoltage);
  s += ",\"frequencyHz\":50";
  s += ",\"mainsPresent\":";
  s += state.mainsPresent ? "true" : "false";
  s += ",\"onBattery\":";
  s += state.onBattery ? "true" : "false";
  s += ",\"alarm\":";
  s += state.alarm ? "true" : "false";
  s += ",\"lowBattery\":";
  s += state.lowBattery ? "true" : "false";
  s += ",\"overload\":";
  s += state.overload ? "true" : "false";
  s += ",\"charging\":";
  s += state.charging ? "true" : "false";
  s += ",\"batteryFull\":";
  s += state.batteryFull ? "true" : "false";
  s += ",";
  appendJsonNumber(s, "batteryBars", state.batteryBars == 255 ? -1 : state.batteryBars);
  s += ",";
  appendJsonNumber(s, "loadLevel", state.loadLevel == 255 ? -1 : state.loadLevel);
  s += ",\"displayBlank\":";
  s += state.displayBlank ? "true" : "false";
  s += ",\"frameCount\":";
  s += String(state.frameCount);
  s += ",\"decodeErrors\":";
  s += String(state.decodeErrors);
  s += ",\"overflows\":";
  s += String(state.overflows);
  s += ",\"lastFrameAgeMs\":";
  s += String(millis() - state.lastFrameMs);
  s += ",\"mem\":[";
  for (uint8_t i = 0; i < 10; i++) {
    if (i) s += ",";
    s += "\"";
    s += hexByte(state.mem[i]);
    s += "\"";
  }
  s += "]}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", s);
}

void handleRoot() {
  server.send(200, "text/html",
    "<!doctype html><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>MHpower</title>"
    "<style>body{font-family:system-ui;margin:24px;background:#111;color:#eee}"
    "pre{font-size:16px;white-space:pre-wrap;background:#1d1d1d;padding:16px;border-radius:8px}"
    ".ok{color:#65e572}.bad{color:#ff6b6b}</style>"
    "<h1>MHpower monitor</h1><pre id='out'>Loading...</pre>"
    "<script>async function tick(){try{let r=await fetch('/api/status');"
    "let j=await r.json();out.textContent=JSON.stringify(j,null,2)}catch(e){out.textContent=e}"
    "}setInterval(tick,1000);tick();</script>");
}

// ----------------------- Setup / loop -----------------------

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("MHpower TM1640 monitor starting");
  Serial.println("Serial-only decode mode");

  pinMode(CLK_GPIO, INPUT);
  pinMode(DIN_GPIO, INPUT);
  const uint32_t pins = GPIO_REG_READ(GPIO_IN_ADDRESS);
  lastPolledClk = (pins & (1 << CLK_GPIO)) ? 1 : 0;
  lastPolledDin = (pins & (1 << DIN_GPIO)) ? 1 : 0;
  if (PIN_AUTODETECT_DEBUG) {
    attachInterrupt(digitalPinToInterrupt(14), onD5ChangeDebug, CHANGE);
    attachInterrupt(digitalPinToInterrupt(12), onD6ChangeDebug, CHANGE);
  } else if (!USE_POLLING_CAPTURE) {
    attachInterrupt(digitalPinToInterrupt(CLK_GPIO), onClkRise, RISING);
    attachInterrupt(digitalPinToInterrupt(DIN_GPIO), onDinChangeDebug, CHANGE);
  }

  if (!ENABLE_WIFI) {
    WiFi.mode(WIFI_OFF);
    WiFi.forceSleepBegin();
    delay(1);
    Serial.println("WiFi disabled. Open Serial Monitor at 115200 baud.");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
    processCapturedBits();
  }
  Serial.println();
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/api/status", handleJson);
  server.begin();
}

void loop() {
  if (USE_POLLING_CAPTURE) {
    for (uint16_t i = 0; i < 300; i++) {
      pollPinsFast();
    }
  }
  processCapturedBits();
  if (ENABLE_WIFI) {
    server.handleClient();
  }

  static uint32_t lastPrint = 0;
  if (millis() - lastPrint > 1000) {
    lastPrint = millis();
    Serial.print("{frames:");
    Serial.print(state.frameCount);
    Serial.print(", input:");
    Serial.print(state.inputVoltage);
    Serial.print(", output:");
    Serial.print(state.outputVoltage);
    Serial.print(", mains:");
    Serial.print(state.mainsPresent ? "true" : "false");
    Serial.print(", battery:");
    Serial.print(state.onBattery ? "true" : "false");
    Serial.print(", alarm:");
    Serial.print(state.alarm ? "true" : "false");
    Serial.print(", lowBattery:");
    Serial.print(state.lowBattery ? "true" : "false");
    Serial.print(", charging:");
    Serial.print(state.charging ? "true" : "false");
    Serial.print(", full:");
    Serial.print(state.batteryFull ? "true" : "false");
    Serial.print(", bars:");
    if (state.batteryBars == 255) Serial.print("?");
    else Serial.print(state.batteryBars);
    Serial.print(", load:");
    if (state.loadLevel == 255) Serial.print("?");
    else Serial.print(state.loadLevel);
    Serial.print(", overload:");
    Serial.print(state.overload ? "true" : "false");
    Serial.print(", mode:0x");
    Serial.print(state.mem[6], HEX);
    Serial.print(", loadRaw:0x");
    Serial.print(state.mem[7], HEX);
    Serial.print(", batRaw:0x");
    Serial.print(state.mem[9], HEX);
    Serial.print(", mem:[");
    for (uint8_t i = 0; i < 10; i++) {
      if (i) Serial.print(" ");
      if (state.mem[i] < 0x10) Serial.print("0");
      Serial.print(state.mem[i], HEX);
    }
    Serial.print("], errors:");
    Serial.print(state.decodeErrors);
    Serial.print(", overflows:");
    Serial.print(ringOverflow);
    Serial.print(", edges:");
    Serial.print(clockEdges);
    Serial.print(", dinChanges:");
    Serial.print(dinChanges);
    Serial.print(", d5Rises:");
    Serial.print(d5Rises);
    Serial.print(", d5Changes:");
    Serial.print(d5Changes);
    Serial.print(", d6Rises:");
    Serial.print(d6Rises);
    Serial.print(", d6Changes:");
    Serial.print(d6Changes);
    Serial.print(", burstBits:");
    Serial.print(burstLen);
    Serial.print(", lastEdgeAgeMs:");
    Serial.print((uint32_t)(micros() - lastClockUs) / 1000);
    Serial.print(", lastDinAgeMs:");
    Serial.print((uint32_t)(micros() - lastDinChangeUs) / 1000);
    Serial.println("}");
  }
}
