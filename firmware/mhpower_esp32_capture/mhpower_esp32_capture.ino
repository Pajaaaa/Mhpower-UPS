/*
  MHpower MPU display monitor - ESP32 serial decoder.

  Wiring (přes 74LVC14A - Schmittův invertor, napájený z 3V3):
    TM1640 CLK -> 1k -> 74LVC14 pin1(1A); pin2(1Y) -> 100R -> ESP32 GPIO18
    TM1640 DIN -> 1k -> 74LVC14 pin3(2A); pin4(2Y) -> 100R -> ESP32 GPIO23
    74LVC14 pin14(VCC) -> ESP32 3V3; pin7(GND) -> GND; 100nF blok. C
    Společná zem: displej + ESP32 + pin7. Displej zůstává na 5 V.
    Signály vyjdou invertované - firmware si edge+invert najde sám.

  Serial monitor: 115200 baud
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <soc/gpio_struct.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <ESPmDNS.h>
#include <time.h>
#include <math.h>

#define USE_TASK_WDT 1
const uint32_t WDT_TIMEOUT_S = 15;       // hardwarová pojistka proti zaseknutí
const uint32_t WIFI_RETRY_MS = 15000;    // po této době bez WiFi zkusit reconnect
const uint32_t WIFI_FALLBACK_MS = 300000;// po 5 min bez WiFi nahodit záchranný hotspot (AP)
const uint32_t WIFI_HARD_REBOOT_MS = 3600000;  // 1 h bez WiFi i bez klientů na hotspotu -> tvrdý reboot (poslední pojistka)
const uint32_t WEB_REINIT_MS = 600000;   // 10 min bez obslouženého HTTP požadavku -> recyklace listen socketu (pojistka proti zaseknutému webu)
const char* const AP_FALLBACK_PASS = "mhpower-setup";  // WPA2 heslo hotspotu (min. 8 znaků)
const uint8_t  EVENT_LOG_SIZE = 16;      // kruhový log událostí

const uint8_t PIN_CLK = 18;
const uint8_t PIN_DIN = 23;
const uint32_t FRAME_STALE_MS = 30000;
const uint32_t CAPTURE_SAMPLES = 18000;   // při 160 MHz ~290µs okno = rezerva nad 192µs rámec (10000 ořezávalo -> err)
const uint32_t CAPTURE_TRIGGER_TIMEOUT_MS = 120;
const uint32_t CAPTURE_INTERVAL_MS = 200;  // throttle: nesnímat naplno pořád, mezi pokusy nechat systém dýchat
const uint8_t SOURCE_CONFIRM_FRAMES = 3;
const uint16_t SNMP_PORT = 161;

// --- baterie / energie ---
const float BATTERY_NOMINAL_V = 12.0f;
const float BATTERY_USABLE_FRACTION = 0.85f;   // podíl kapacity využitelný do vypnutí UPS
const float PEUKERT_K = 1.15f;                 // Peukertův exponent olověné baterie
const float PEUKERT_REF_HOURS = 20.0f;         // jmenovitá kapacita platí při C20
const uint32_t ENERGY_DT_MAX_MS = 5000UL;      // strop kroku integrace energie
const float INVERTER_EFFICIENCY = 0.85f;       // účinnost měniče (napevno)
const uint16_t INVERTER_IDLE_W = 20;           // vlastní spotřeba měniče [W] (napevno)

WebServer server(80);
WiFiUDP snmpUdp;
Preferences prefs;
char responseBody[5000];

struct AppSettings {
  char deviceName[33] = "MHpower";
  char wifiSsid[33] = "WIFI_SSID";       // !!! nastav před prvním flashem (nebo později ve webu)
  char wifiPass[65] = "WIFI_PASSWORD";   // !!! nastav před prvním flashem
  char webUser[17] = "admin";
  char webPass[33] = "changeme";         // !!! změň přihlašovací heslo do webu
  uint16_t sourceWatts = 500;
  float batteryAh = 40.0;
  uint8_t batterySystemVoltage = 12;   // 12 / 24 / 48 V — vyšší řady MPU mají 24V (i 48V) banky
  float batteryHealthFactor = 1.0;
  char batteryInstallDate[11] = "2026-06-04";
  uint8_t healthWarnPercent = 60;
  uint16_t minRuntimeMinutes = 10;
  char snmpCommunity[33] = "public";
  char ntpServer[48] = "pool.ntp.org";
};

AppSettings settings;

// modely MPU a jejich napětí baterie (z manuálu): ≤800W=12V, ≤2500W=24V, jinak 48V
// (potvrzeno tabulkou výdrže — 12V banka utáhne zátěž do ~700W, 24V do ~1800W, 48V výš)
const uint16_t MPU_WATTS[] = {300, 500, 700, 800, 1050, 1200, 1400, 1600, 1800, 2100, 3000, 3500, 4200, 5000};
const uint8_t MPU_WATTS_N = 14;
bool isValidWatts(uint16_t w) { for (uint8_t i = 0; i < MPU_WATTS_N; i++) if (MPU_WATTS[i] == w) return true; return false; }
uint8_t batteryVoltageForWatts(uint16_t w) { return w <= 800 ? 12 : (w <= 2500 ? 24 : 48); }

struct SnmpValue {
  uint8_t type;
  int32_t integer;
  char text[48];
};

struct DisplayState {
  uint8_t mem[10] = {0};
  uint8_t brightnessCommand = 0;
  uint32_t frameCount = 0;
  uint32_t lastFrameMs = 0;
  uint32_t byteCount = 0;
  uint32_t decodeErrors = 0;
  uint32_t filteredFrames = 0;
  uint32_t captureOk = 0;
  uint32_t captureTimeouts = 0;
  uint32_t lastCaptureMs = 0;

  int inputVoltage = -1;
  int outputVoltage = -1;
  bool displayBlank = false;
  bool mainsPresent = false;
  bool onBattery = false;
  bool alarm = false;
  bool lowBattery = false;
  bool criticalBattery = false;
  bool overheat = false;
  bool overload = false;
  bool charging = false;
  bool batteryFull = false;
  uint8_t batteryBars = 255;
  uint8_t loadLevel = 255;
  int loadPercent = -1;
  int loadWattsEstimate = -1;
  const char* sourceState = "unknown";
  const char* batteryState = "unknown";
  const char* batteryVoltageRange = "unknown";
  float batteryVoltageEstimate = -1.0;
  const char* loadState = "unknown";
  uint32_t onBatteryRuntimeSec = 0;
  uint32_t lastBatteryRunSec = 0;
  int32_t runtimeRemainingEstimateSec = -1;
  int32_t runtimeTotalEstimateSec = -1;
  uint8_t batteryHealthPercent = 100;
  bool healthWarning = false;
  bool runtimeWarning = false;
  bool overVoltage = false;    // vstup >110 % nominálu -> AVR snižuje (displej V↑)
  bool underVoltage = false;   // vstup <90 % nominálu -> AVR zvyšuje (displej V↓)
  const char* avrState = "unknown";  // "buck" / "boost" / "normal" / "n/a"
};

DisplayState state;

struct RawDebugState {
  uint8_t data[48] = {0};
  uint8_t dataLen = 0;
  uint8_t mem[10] = {0};
  uint8_t brightness = 0;
  int score = 0;
  uint8_t edge = 0;
  int8_t offset = 0;
  uint8_t invert = 0;
  uint8_t order = 0;
  uint8_t phase = 0;
  uint8_t bitLen = 0;
  uint32_t updatedMs = 0;
};

RawDebugState rawDebug;

// --- odchyt neznámých číslic na displeji (hledáme chybějící „9") ---
// Když je některá ze 6 číslic napětí (mem[0..5]) neznámý segmentový vzor a obě
// zbývající číslice ve své trojici jsou platné 0–9, jde skoro jistě o reálnou
// číslici, kterou tabulka digitFromPattern() ještě nezná. Zapíšeme její syrový
// bajt + kontext, ať jde dohledat i bez sériáku (čte se přes /api/digitscan).
struct UnknownDigitLog {
  uint8_t pattern[8] = {0};   // až 8 různých neznámých bajtů
  uint16_t count[8] = {0};    // kolikrát se každý objevil
  uint8_t distinct = 0;       // kolik slotů je obsazeno
  uint8_t lastPattern = 0;    // poslední neznámý bajt
  uint8_t lastPos = 0;        // pozice 0..5 (0–2 vstup, 3–5 výstup)
  uint8_t lastCtx[6] = {0};   // mem[0..5] v okamžiku záchytu (pro rekonstrukci čísla)
  uint32_t lastEpoch = 0;
  uint32_t lastUptime = 0;
  uint32_t total = 0;         // celkový počet záchytů
};
UnknownDigitLog unkLog;

// --- icon-scan: histogram bajtů ikon mem[6] (mode) a mem[8] (ikony) s kontextem
// vstupního napětí. Hledáme bity přepětí V↑ / podpětí V↓ / ⚠️, které svítí jen když
// síť vyjede z pásma ~207–253 V. Čte se přes /api/iconscan.
struct IconScanLog {
  uint8_t mode[12] = {0};   uint16_t modeCount[12] = {0};   uint8_t modeDistinct = 0;
  uint8_t icon[12] = {0};   uint16_t iconCount[12] = {0};   uint8_t iconDistinct = 0;
  uint8_t hiMode = 0, hiIcon = 0; int hiV = -1; uint32_t hiEpoch = 0;   // záchyt při vstupu >253 V
  uint8_t loMode = 0, loIcon = 0; int loV = -1; uint32_t loEpoch = 0;   // záchyt při vstupu <207 V
  uint32_t total = 0;
};
IconScanLog iconLog;

uint8_t batteryHistory[8] = {0};
uint8_t batteryHistoryPos = 0;
uint8_t batteryHistoryCount = 0;
uint8_t fullBatteryStableFrames = 0;
bool hasStableSource = false;
bool stableOnBattery = false;
bool pendingOnBattery = false;
uint8_t pendingSourceCount = 0;

// debounce stavových příznaků proti glitchům jednoho rámce: alarm bit (mem[6] & 0x04) a
// přetížení (mem[7]==0x7F) se braly syrově z jednoho rámce -> jeden zkažený rámec dělal
// falešný alarm/přetížení (a tím i "summary_error" v power monitoru). onBattery na stejném
// bytu glitch ustojí díky SOURCE_CONFIRM_FRAMES, tyhle bity ne -> dostávají stejný debounce.
bool stableAlarmBit = false;
bool pendingAlarmBit = false;
uint8_t pendingAlarmCount = 0;
bool stableOverload = false;
bool pendingOverload = false;
uint8_t pendingOverloadCount = 0;

uint8_t samples[CAPTURE_SAMPLES];
bool batterySessionActive = false;
uint32_t batterySessionStartMs = 0;
uint32_t batteryRuntimeLastMs = 0;
float batterySessionUsedWh = 0.0;
bool batterySessionReachedCritical = false;
bool batterySessionHealthSaved = false;
float batterySessionStartFraction = 1.0f;
float avgDrainW = 0.0f;
uint32_t lastCaptureAttemptMs = 0;
uint32_t lastSnmpBindMs = 0;

// Potvrzovací debounce binárního příznaku: 'stable' se přepne na 'raw' teprve když 'raw'
// vydrží shodně po 'confirmFrames' po sobě jdoucích rámcích. Jeden zkažený rámec (běžné u
// pasivního odposlechu sběrnice) tak stavový příznak nepřepne. Vrací aktuální stabilní hodnotu.
bool confirmFlag(bool raw, bool &stable, bool &pending, uint8_t &count, uint8_t confirmFrames) {
  if (raw == stable) {
    pending = raw;
    count = 0;
  } else if (pending != raw) {
    pending = raw;
    count = 1;
  } else if (count < 255 && ++count >= confirmFrames) {
    stable = raw;
    count = 0;
  }
  return stable;
}

// --- diagnostika běhu (kvůli ladění výpadků/restartů bez sériáku) ---
esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;
uint32_t minFreeHeapBoot = 0;

// --- WiFi dohled (aktivní reconnect + záchranný hotspot) ---
uint32_t lastWifiOkMs = 0;
uint32_t lastWifiRetryMs = 0;
bool apFallbackActive = false;   // běží záchranný AP hotspot (WiFi se nepřipojila)
uint32_t lastWebReqMs = 0;       // čas posledního obslouženého HTTP požadavku (liveness web serveru)

// --- kruhový log událostí (výpadky sítě, baterie, alarmy) ---
struct LogEvent { uint32_t epoch; uint32_t uptime; char msg[28]; };
LogEvent eventLog[EVENT_LOG_SIZE];
uint8_t eventCount = 0;
uint8_t eventHead = 0;

uint32_t nowEpoch() {
  time_t t = time(nullptr);
  return t > 1700000000UL ? (uint32_t)t : 0;   // > 2023 => NTP synchronizováno
}
void logEvent(const char* msg) {
  LogEvent &e = eventLog[eventHead];
  e.epoch = nowEpoch();
  e.uptime = millis() / 1000UL;
  strncpy(e.msg, msg, sizeof(e.msg) - 1);
  e.msg[sizeof(e.msg) - 1] = 0;
  eventHead = (eventHead + 1) % EVENT_LOG_SIZE;
  if (eventCount < EVENT_LOG_SIZE) eventCount++;
}

// zaznamenej přechody stavů do logu
void detectEvents() {
  static bool init = false, pMains = true, pOver = false, pLow = false, pCrit = false, pOvl = false,
              pHealth = false, pRun = false;
  if (!init) {
    init = true;
    pMains = state.mainsPresent; pOver = state.overheat; pLow = state.lowBattery;
    pCrit = state.criticalBattery; pOvl = state.overload;
    pHealth = state.healthWarning; pRun = state.runtimeWarning;
    return;
  }
  if (state.mainsPresent && !pMains) logEvent("Síť obnovena");
  if (!state.mainsPresent && pMains) logEvent("Výpadek sítě");
  if (state.overheat && !pOver) logEvent("Přehřátí");
  if (state.lowBattery && !pLow) logEvent("Nízká baterie");
  if (state.criticalBattery && !pCrit) logEvent("Kritická baterie");
  if (state.overload && !pOvl) logEvent("Přetížení");
  if (state.healthWarning && !pHealth) logEvent("Kondice baterie pod prahem");
  if (state.runtimeWarning && !pRun) logEvent("Výdrž pod limitem");
  pMains = state.mainsPresent; pOver = state.overheat; pLow = state.lowBattery;
  pCrit = state.criticalBattery; pOvl = state.overload;
  pHealth = state.healthWarning; pRun = state.runtimeWarning;
}

// --- ukotvení výdrže na dílky baterie: naučená energie [Wh] na každý dílek 0..5 ---
// index = počet dílků, který se právě vybíjí (energie spotřebovaná, než dílek zhasne)
float learnedWhPerBar[6] = {0};
uint16_t learnedBarSamples[6] = {0};
int8_t energyBarLevel = -1;      // dílek, který se právě sleduje (-1 = nesledováno)
float whSinceBarEntry = 0.0f;    // Wh spotřebované od vstupu do aktuálního dílku
bool barEntryObserved = false;   // viděli jsme čistý vstup do aktuálního dílku?

const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "ext-pin";
    case ESP_RST_SW:        return "sw-restart";
    case ESP_RST_PANIC:     return "panika/exception";
    case ESP_RST_INT_WDT:   return "int-watchdog";
    case ESP_RST_TASK_WDT:  return "task-watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "neznamy";
  }
}

void loadSettings() {
  prefs.begin("mhpower", true);
  String devName = prefs.getString("devName", settings.deviceName);
  String ssid = prefs.getString("ssid", settings.wifiSsid);
  String pass = prefs.getString("pass", settings.wifiPass);
  String webUser = prefs.getString("webUser", settings.webUser);
  String webPass = prefs.getString("webPass", settings.webPass);
  String install = prefs.getString("batDate", settings.batteryInstallDate);
  String community = prefs.getString("snmpComm", settings.snmpCommunity);
  strncpy(settings.deviceName, devName.c_str(), sizeof(settings.deviceName) - 1);
  settings.deviceName[sizeof(settings.deviceName) - 1] = 0;
  if (settings.deviceName[0] == 0) strncpy(settings.deviceName, "MHpower", sizeof(settings.deviceName) - 1);
  strncpy(settings.wifiSsid, ssid.c_str(), sizeof(settings.wifiSsid) - 1);
  settings.wifiSsid[sizeof(settings.wifiSsid) - 1] = 0;
  strncpy(settings.wifiPass, pass.c_str(), sizeof(settings.wifiPass) - 1);
  settings.wifiPass[sizeof(settings.wifiPass) - 1] = 0;
  strncpy(settings.webUser, webUser.c_str(), sizeof(settings.webUser) - 1);
  settings.webUser[sizeof(settings.webUser) - 1] = 0;
  if (settings.webUser[0] == 0) strncpy(settings.webUser, "admin", sizeof(settings.webUser) - 1);
  strncpy(settings.webPass, webPass.c_str(), sizeof(settings.webPass) - 1);
  settings.webPass[sizeof(settings.webPass) - 1] = 0;
  if (settings.webPass[0] == 0) strncpy(settings.webPass, "changeme", sizeof(settings.webPass) - 1);
  strncpy(settings.snmpCommunity, community.c_str(), sizeof(settings.snmpCommunity) - 1);
  settings.snmpCommunity[sizeof(settings.snmpCommunity) - 1] = 0;
  if (settings.snmpCommunity[0] == 0) strncpy(settings.snmpCommunity, "public", sizeof(settings.snmpCommunity) - 1);
  String ntp = prefs.getString("ntp", settings.ntpServer);
  strncpy(settings.ntpServer, ntp.c_str(), sizeof(settings.ntpServer) - 1);
  settings.ntpServer[sizeof(settings.ntpServer) - 1] = 0;
  if (settings.ntpServer[0] == 0) strncpy(settings.ntpServer, "pool.ntp.org", sizeof(settings.ntpServer) - 1);
  settings.sourceWatts = prefs.getUShort("watts", settings.sourceWatts);
  if (!isValidWatts(settings.sourceWatts)) settings.sourceWatts = 500;
  settings.batterySystemVoltage = batteryVoltageForWatts(settings.sourceWatts);  // napětí baterie automaticky z modelu
  settings.batteryAh = prefs.getFloat("batAh", settings.batteryAh);
  settings.batteryHealthFactor = prefs.getFloat("batHealth", settings.batteryHealthFactor);
  if (settings.batteryHealthFactor < 0.20 || settings.batteryHealthFactor > 1.20) settings.batteryHealthFactor = 1.0;
  strncpy(settings.batteryInstallDate, install.c_str(), sizeof(settings.batteryInstallDate) - 1);
  settings.batteryInstallDate[sizeof(settings.batteryInstallDate) - 1] = 0;
  settings.healthWarnPercent = prefs.getUChar("health", settings.healthWarnPercent);
  settings.minRuntimeMinutes = prefs.getUShort("minRun", settings.minRuntimeMinutes);
  if (prefs.getBytesLength("whBar") == sizeof(learnedWhPerBar)) {
    prefs.getBytes("whBar", learnedWhPerBar, sizeof(learnedWhPerBar));
  }
  if (prefs.getBytesLength("whBarN") == sizeof(learnedBarSamples)) {
    prefs.getBytes("whBarN", learnedBarSamples, sizeof(learnedBarSamples));
  }
  for (uint8_t b = 0; b < 6; b++) {
    if (!(learnedWhPerBar[b] >= 0.0f && learnedWhPerBar[b] < 100000.0f)) { learnedWhPerBar[b] = 0.0f; learnedBarSamples[b] = 0; }
  }
  prefs.end();
}

// naučenou tabulku Wh/dílek ukládáme zvlášť (mění se jen při přechodu dílku)
void saveLearnedBars() {
  prefs.begin("mhpower", false);
  prefs.putBytes("whBar", learnedWhPerBar, sizeof(learnedWhPerBar));
  prefs.putBytes("whBarN", learnedBarSamples, sizeof(learnedBarSamples));
  prefs.end();
}

void resetLearnedBars() {
  for (uint8_t b = 0; b < 6; b++) { learnedWhPerBar[b] = 0.0f; learnedBarSamples[b] = 0; }
  energyBarLevel = -1;
  whSinceBarEntry = 0.0f;
  barEntryObserved = false;
  saveLearnedBars();
}

void saveSettings() {
  prefs.begin("mhpower", false);
  prefs.putString("devName", settings.deviceName);
  prefs.putString("ssid", settings.wifiSsid);
  prefs.putString("pass", settings.wifiPass);
  prefs.putString("webUser", settings.webUser);
  prefs.putString("webPass", settings.webPass);
  prefs.putString("snmpComm", settings.snmpCommunity);
  prefs.putString("ntp", settings.ntpServer);
  prefs.putUShort("watts", settings.sourceWatts);
  prefs.putFloat("batAh", settings.batteryAh);
  prefs.putFloat("batHealth", settings.batteryHealthFactor);
  prefs.putString("batDate", settings.batteryInstallDate);
  prefs.putUChar("health", settings.healthWarnPercent);
  prefs.putUShort("minRun", settings.minRuntimeMinutes);
  prefs.end();
}

static inline uint8_t readPins() {
  const uint32_t v = GPIO.in;
  const uint8_t clk = (v >> PIN_CLK) & 1;
  const uint8_t din = (v >> PIN_DIN) & 1;
  return clk | (din << 1);
}

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
    case 0x7D: return 9;  // potvrzeno digit-scanem (0x7D x674 pres noc)
    case 0x00: return -2;
    default: return -1;
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
    case 0x65: return 3;
    case 0x63: return 3;
    case 0x67: return 4;
    case 0x77: return 5;
    case 0x7F: return 5;
    default: return 255;
  }
}

float nominalUsableBatteryWh() {
  return settings.batteryAh * (BATTERY_NOMINAL_V * settings.batterySystemVoltage / 12.0f) * BATTERY_USABLE_FRACTION * settings.batteryHealthFactor;
}

// kolik reálně teče z baterie: výstupní zátěž / účinnost + vlastní spotřeba měniče
float batteryDrainWatts() {
  if (!state.onBattery || state.outputVoltage <= 0) return 0.0f;
  const float out = state.loadWattsEstimate > 0 ? (float)state.loadWattsEstimate : 0.0f;
  return out / INVERTER_EFFICIENCY + (float)INVERTER_IDLE_W;
}

// využitelná energie po korekci na vybíjecí proud (Peukert)
float usableBatteryWhAtDrain(float drainW) {
  const float wh = nominalUsableBatteryWh();
  if (drainW <= 1.0f) return wh;
  const float vbatt = state.batteryVoltageEstimate > 5.0f ? state.batteryVoltageEstimate : 12.2f;
  const float I = drainW / vbatt;
  const float Iref = settings.batteryAh / PEUKERT_REF_HOURS;
  if (Iref <= 0.0f || I <= Iref) return wh;
  return wh * powf(Iref / I, PEUKERT_K - 1.0f);
}

// ---- ukotvení výdrže na dílky baterie (lepší model) ----
// Měnič sám zná reálné napětí/proud/teplotu -> jeho výsledek jsou dílky.
// Místo rekonstrukce z Ah měříme reálnou energii spotřebovanou na každý dílek
// a z naučené tabulky Wh/dílek počítáme zbývající čas. Peukert/účinnost jsou
// v tom měření už zahrnuté. Dokud není naučeno dost dílků, použije se starý
// model (Ah+Peukert) jako prior — flashnout jde tedy bez rizika regrese.

uint8_t barsLearnedCount() {
  uint8_t n = 0;
  for (uint8_t b = 1; b <= 5; b++) if (learnedBarSamples[b] > 0) n++;
  return n;
}

// orientační podíl SoC pro hranici dílku (0..5), shodné s batterySoCFractionFromBars
float socFracForBar(uint8_t b) {
  switch (b) {
    case 5: return 1.00f;
    case 4: return 0.80f;
    case 3: return 0.62f;
    case 2: return 0.45f;
    case 1: return 0.25f;
    default: return 0.08f;   // b == 0
  }
}

// prior energie [Wh] spotřebovaná, než zhasne dílek b (než klesne na b-1)
float priorWhForBar(uint8_t b) {
  const float below = (b == 0) ? 0.0f : socFracForBar(b - 1);
  float delta = socFracForBar(b) - below;
  if (delta < 0.0f) delta = 0.0f;
  return delta * nominalUsableBatteryWh();
}

float whForBar(uint8_t b) {
  if (b > 5) return 0.0f;
  return learnedBarSamples[b] > 0 ? learnedWhPerBar[b] : priorWhForBar(b);
}

// vrátí true a naplní remainingWh/totalWh, pokud je naučeno dost dílků
bool estimateRuntimeFromBars(float &remainingWh, float &totalWh) {
  if (state.batteryBars > 5) return false;
  if (barsLearnedCount() < 3) return false;          // jinak nech starý model
  const uint8_t cb = state.batteryBars;
  float total = 0.0f;
  for (uint8_t b = 0; b <= 5; b++) total += whForBar(b);
  float lower = 0.0f;
  for (uint8_t b = 0; b < cb; b++) lower += whForBar(b);
  const float curWh = whForBar(cb);
  float remCur;
  if (barEntryObserved && energyBarLevel == (int8_t)cb) {
    remCur = curWh - whSinceBarEntry;        // víme, kolik dílku už ubylo
    if (remCur < 0.0f) remCur = 0.0f;
  } else {
    remCur = curWh * 0.5f;                    // vstoupili jsme doprostřed dílku
  }
  remainingWh = remCur + lower;
  totalWh = total;
  return true;
}

// hrubý odhad zbývající využitelné energie podle dílků baterie (0..5)
float batterySoCFractionFromBars(uint8_t bars) {
  switch (bars) {
    case 5: return 1.00f;
    case 4: return 0.80f;
    case 3: return 0.62f;
    case 2: return 0.45f;
    case 1: return 0.25f;
    case 0: return 0.08f;
    default: return 1.00f;
  }
}

void saveBatteryHealthFactor() {
  prefs.begin("mhpower", false);
  prefs.putFloat("batHealth", settings.batteryHealthFactor);
  prefs.end();
}

bool learnBatteryHealthFromCurrentSession() {
  if (batterySessionStartFraction < 0.90f) return false;   // učit jen z výboje, co začal plný
  const float nominalWh = settings.batteryAh * (BATTERY_NOMINAL_V * settings.batterySystemVoltage / 12.0f) * BATTERY_USABLE_FRACTION;
  if (nominalWh <= 1.0f || batterySessionUsedWh <= 1.0f) return false;

  float learned = batterySessionUsedWh / nominalWh;
  if (learned < 0.20f) learned = 0.20f;
  if (learned > 1.00f) learned = 1.00f;
  settings.batteryHealthFactor = settings.batteryHealthFactor * 0.70f + learned * 0.30f;
  saveBatteryHealthFactor();
  return true;
}

bool voltageLooksValid(int inputVoltage, int outputVoltage) {
  const bool inputOk = inputVoltage == -2 || (inputVoltage >= 0 && inputVoltage <= 260);
  const bool outputOk = outputVoltage == -2 || (outputVoltage >= 0 && outputVoltage <= 260);
  return inputOk && outputOk;
}

void rememberBatteryPattern(uint8_t v) {
  if (batteryBarsFrom09(v) == 255) return;   // do historie jen platné ikony – glitch dekódu nepouštět dál
  batteryHistory[batteryHistoryPos] = v;
  batteryHistoryPos = (batteryHistoryPos + 1) % sizeof(batteryHistory);
  if (batteryHistoryCount < sizeof(batteryHistory)) batteryHistoryCount++;
}

bool batteryPatternIsChanging() {
  if (batteryHistoryCount < 4) return false;
  // animace nabíjení = vzorek se reálně mění; jeden odskok v historii je glitch dekódu, ne animace.
  // Za "měnící se" ber jen stav, kdy se od nejčastější hodnoty liší aspoň 2 vzorky.
  uint8_t bestCount = 0;
  for (uint8_t i = 0; i < batteryHistoryCount; i++) {
    uint8_t c = 0;
    for (uint8_t j = 0; j < batteryHistoryCount; j++) if (batteryHistory[j] == batteryHistory[i]) c++;
    if (c > bestCount) bestCount = c;
  }
  return (uint8_t)(batteryHistoryCount - bestCount) >= 2;
}

bool firstBatteryBarIsBlinking() {
  if (batteryHistoryCount < 4) return false;
  bool seenOff = false;
  bool seenOne = false;
  for (uint8_t i = 0; i < batteryHistoryCount; i++) {
    const uint8_t v = batteryHistory[i];
    if (v == 0x10) seenOff = true;
    else if (v == 0x30) seenOne = true;
    else return false;
  }
  return seenOff && seenOne;
}

// zapiš jeden neznámý segmentový vzor do histogramu + ulož kontext
void registerUnknownPattern(uint8_t val, uint8_t pos, const uint8_t* mem) {
  unkLog.total++;
  unkLog.lastPattern = val;
  unkLog.lastPos = pos;
  memcpy(unkLog.lastCtx, mem, 6);
  unkLog.lastEpoch = nowEpoch();
  unkLog.lastUptime = millis() / 1000UL;
  for (uint8_t i = 0; i < unkLog.distinct; i++) {
    if (unkLog.pattern[i] == val) { if (unkLog.count[i] < 0xFFFF) unkLog.count[i]++; return; }
  }
  if (unkLog.distinct < 8) {
    unkLog.pattern[unkLog.distinct] = val;
    unkLog.count[unkLog.distinct] = 1;
    unkLog.distinct++;
  }
}

// projeď obě trojice číslic napětí; neznámou číslici zapiš jen když obě sousední
// v téže trojici jsou platné 0–9 (silný signál, že jde o reálný neznámý glyf, ne šum)
void recordUnknownDigits(const uint8_t* mem) {
  for (uint8_t g = 0; g < 2; g++) {
    const uint8_t base = g * 3;
    uint8_t good = 0, unk = 0, unkIdx = 0, unkVal = 0;
    for (uint8_t k = 0; k < 3; k++) {
      const int d = digitFromPattern(mem[base + k]);
      if (d >= 0) good++;
      else if (d == -1) { unk++; unkIdx = k; unkVal = mem[base + k]; }
    }
    if (unk == 1 && good == 2) registerUnknownPattern(unkVal, base + unkIdx, mem);
  }
}

static void iconAddDistinct(uint8_t* vals, uint16_t* counts, uint8_t* n, uint8_t v, uint8_t cap) {
  for (uint8_t i = 0; i < *n; i++) { if (vals[i] == v) { if (counts[i] < 0xFFFF) counts[i]++; return; } }
  if (*n < cap) { vals[*n] = v; counts[*n] = 1; (*n)++; }
}

// zaznamenej ikonové bajty s kontextem vstupního napětí; inputV = -1 mimo síť (lo se nezapíše)
void recordIconScan(const uint8_t* mem, int inputV) {
  iconLog.total++;
  iconAddDistinct(iconLog.mode, iconLog.modeCount, &iconLog.modeDistinct, mem[6], 12);
  iconAddDistinct(iconLog.icon, iconLog.iconCount, &iconLog.iconDistinct, mem[8], 12);
  if (inputV > 253) { iconLog.hiMode = mem[6]; iconLog.hiIcon = mem[8]; iconLog.hiV = inputV; iconLog.hiEpoch = nowEpoch(); }
  else if (inputV >= 0 && inputV < 207) { iconLog.loMode = mem[6]; iconLog.loIcon = mem[8]; iconLog.loV = inputV; iconLog.loEpoch = nowEpoch(); }
}

void updateDecodedState(const uint8_t* mem, uint8_t brightness) {
  const int candidateInput = decode3Digits(&mem[0]);
  const int candidateOutput = decode3Digits(&mem[3]);
  const uint8_t candidateBars = batteryBarsFrom09(mem[9]);
  const uint8_t candidateLoad = loadLevelFrom07(mem[7]);
  const bool candidateDisplayBlank = candidateInput == -2 && candidateOutput == -2;
  const bool candidateOverheat =
      candidateDisplayBlank &&
      (mem[6] == 0x43 || mem[6] == 0x83 || mem[6] == 0xC3 || mem[8] == 0x67);

  const bool brightnessOk = (brightness & 0xF0) == 0x80;
  // záchyt neznámých číslic ZÁMĚRNĚ před filtrem napětí: právě neznámá číslice (např. „9")
  // shodí candidateInput na -1 a frame by se jinak zahodil dřív, než bychom bajt viděli
  if (brightnessOk) recordUnknownDigits(mem);

  if (!voltageLooksValid(candidateInput, candidateOutput) || !brightnessOk) {
    state.filteredFrames++;
    return;
  }

  const bool rawOnBattery = candidateOverheat ? false : ((mem[6] & 0x40) != 0);
  if (!hasStableSource) {
    stableOnBattery = rawOnBattery;
    pendingOnBattery = rawOnBattery;
    pendingSourceCount = 0;
    hasStableSource = true;
  } else if (rawOnBattery != stableOnBattery) {
    if (pendingOnBattery != rawOnBattery) {
      pendingOnBattery = rawOnBattery;
      pendingSourceCount = 1;
    } else if (pendingSourceCount < 255) {
      pendingSourceCount++;
    }

    if (pendingSourceCount >= SOURCE_CONFIRM_FRAMES) {
      stableOnBattery = rawOnBattery;
      pendingSourceCount = 0;
    }
  } else {
    pendingOnBattery = rawOnBattery;
    pendingSourceCount = 0;
  }

  memcpy(state.mem, mem, 10);
  state.brightnessCommand = brightness;
  state.frameCount++;
  state.lastFrameMs = millis();

  state.inputVoltage = candidateInput;
  state.outputVoltage = candidateOutput;
  state.displayBlank = candidateDisplayBlank;
  state.overheat = candidateOverheat;

  const uint8_t mode = state.mem[6];
  state.onBattery = state.overheat ? false : stableOnBattery;
  state.mainsPresent = !state.onBattery;
  const bool rawAlarmBit = (mode & 0x04) != 0;
  const bool stableAlarm = confirmFlag(rawAlarmBit, stableAlarmBit, pendingAlarmBit,
                                       pendingAlarmCount, SOURCE_CONFIRM_FRAMES);
  state.alarm = state.overheat || stableAlarm;

  state.batteryBars = candidateBars;
  rememberBatteryPattern(state.mem[9]);
  const bool chargingAnimation = batteryPatternIsChanging();
  const bool criticalBlink = firstBatteryBarIsBlinking();
  const bool fullPattern = state.mainsPresent && candidateBars == 5 && state.mem[9] == 0x77;
  if (!state.overheat && fullPattern && !chargingAnimation) {
    if (fullBatteryStableFrames < 10) fullBatteryStableFrames++;
  } else {
    fullBatteryStableFrames = 0;
  }
  state.batteryFull = !state.overheat && fullPattern && fullBatteryStableFrames >= 3;
  const bool mainsBatteryKnown = state.mainsPresent && candidateBars != 255;
  state.charging = !state.overheat && state.mainsPresent &&
                   (chargingAnimation || (mainsBatteryKnown && !state.batteryFull));
  state.criticalBattery = !state.overheat && state.onBattery && criticalBlink &&
                          (state.mem[9] == 0x10 || state.mem[9] == 0x30);
  // nízká baterie = (skoro) vybitá, ale ještě ne kritická; kritická ji vždy zahrnuje
  state.lowBattery = !state.overheat && state.onBattery &&
                     (state.criticalBattery || (state.batteryBars != 255 && state.batteryBars <= 1));

  state.loadLevel = candidateLoad;
  const bool rawOverload = state.mem[7] == 0x7F;
  state.overload = confirmFlag(rawOverload, stableOverload, pendingOverload,
                               pendingOverloadCount, SOURCE_CONFIRM_FRAMES);
  if (state.loadLevel == 255) {
    state.loadPercent = -1;
    state.loadWattsEstimate = -1;
  } else {
    state.loadPercent = state.loadLevel * 20;
    state.loadWattsEstimate = (state.loadLevel * (int)settings.sourceWatts) / 5;
  }

  // přepětí/podpětí z dekódovaného vstupního napětí (prahy 110 %/90 % nominálu 230 V → 253/207 V)
  if (state.mainsPresent && state.inputVoltage >= 0) {
    state.overVoltage = state.inputVoltage > 253;
    state.underVoltage = state.inputVoltage < 207;
  } else {
    state.overVoltage = false;
    state.underVoltage = false;
  }
  state.avrState = state.overVoltage ? "buck" : (state.underVoltage ? "boost" : (state.mainsPresent ? "normal" : "n/a"));
  recordIconScan(state.mem, state.mainsPresent ? state.inputVoltage : -1);

  state.sourceState = state.overheat ? "overheat" : (state.onBattery ? "battery" : "mains");

  if (state.overheat) state.batteryState = "overheat";
  else if (state.criticalBattery) state.batteryState = "critical";
  else if (state.lowBattery) state.batteryState = "low";
  else if (state.charging) state.batteryState = "charging";
  else if (state.batteryFull) state.batteryState = "full";
  else if (state.batteryBars != 255) state.batteryState = "discharging";
  else state.batteryState = "unknown";

  state.batteryVoltageEstimate = -1.0;
  state.batteryVoltageRange = "unknown";
  if (state.onBattery) {
    if (state.lowBattery && (state.mem[9] == 0x10 || state.mem[9] == 0x30)) {
      state.batteryVoltageEstimate = 11.5;
      state.batteryVoltageRange = "critical_11.5V";
    } else {
      switch (state.batteryBars) {
        case 5: state.batteryVoltageEstimate = 13.1; state.batteryVoltageRange = "above_13.0V"; break;
        case 4: state.batteryVoltageEstimate = 13.0; state.batteryVoltageRange = "around_13.0V"; break;
        case 3: state.batteryVoltageEstimate = 12.7; state.batteryVoltageRange = "around_12.7V"; break;
        case 2: state.batteryVoltageEstimate = 12.3; state.batteryVoltageRange = "around_12.3V"; break;
        case 1: state.batteryVoltageEstimate = 11.9; state.batteryVoltageRange = "around_11.9V"; break;
        case 0: state.batteryVoltageEstimate = 11.5; state.batteryVoltageRange = "critical_11.5V"; break;
      }
    }
  }
  // vyšší řady MPU mají 24V (příp. 48V) banky — odhad je kalibrovaný na 12V blok,
  // celkové napětí banky proto škálujeme dle nastaveného systémového napětí (range = napětí na blok)
  if (state.batteryVoltageEstimate >= 0 && settings.batterySystemVoltage != 12)
    state.batteryVoltageEstimate *= (settings.batterySystemVoltage / 12.0f);

  if (state.overload) state.loadState = "overload";
  else {
    switch (state.loadLevel) {
      case 0: state.loadState = "none"; break;
      case 1: state.loadState = "low"; break;
      case 2: state.loadState = "medium"; break;
      case 3: state.loadState = "high"; break;
      case 4: state.loadState = "very_high"; break;
      case 5: state.loadState = "max"; break;
      default: state.loadState = "unknown"; break;
    }
  }

  if (state.onBattery) {
    const uint32_t now = millis();
    if (!batterySessionActive) {
      batterySessionActive = true;
      batterySessionStartMs = now;
      batteryRuntimeLastMs = now;
      batterySessionUsedWh = 0.0f;
      batterySessionReachedCritical = false;
      batterySessionHealthSaved = false;
      batterySessionStartFraction = batterySoCFractionFromBars(state.batteryBars);
      avgDrainW = 0.0f;
      energyBarLevel = state.batteryBars <= 5 ? (int8_t)state.batteryBars : -1;
      whSinceBarEntry = 0.0f;
      barEntryObserved = false;   // start uprostřed dílku -> jeho náplň neznáme
    }
    uint32_t dtMs = now - batteryRuntimeLastMs;
    batteryRuntimeLastMs = now;
    if (dtMs > ENERGY_DT_MAX_MS) dtMs = ENERGY_DT_MAX_MS;   // krok ořízni, nezahazuj
    const float drainW = batteryDrainWatts();
    if (drainW > 0.0f) {
      const float stepWh = drainW * ((float)dtMs / 3600000.0f);
      batterySessionUsedWh += stepWh;
      whSinceBarEntry += stepWh;
      avgDrainW = avgDrainW <= 0.0f ? drainW : (avgDrainW * 0.9f + drainW * 0.1f);
    }
    // přechod dílku: energie utracená na opouštěném dílku se naučí (EMA)
    if (state.batteryBars <= 5) {
      const int8_t cb = (int8_t)state.batteryBars;
      if (energyBarLevel < 0) {
        energyBarLevel = cb; whSinceBarEntry = 0.0f; barEntryObserved = false;
      } else if (cb != energyBarLevel) {
        if (cb == energyBarLevel - 1 && barEntryObserved && whSinceBarEntry > 0.5f) {
          const uint8_t lb = (uint8_t)energyBarLevel;
          learnedWhPerBar[lb] = learnedBarSamples[lb] == 0
              ? whSinceBarEntry
              : learnedWhPerBar[lb] * 0.7f + whSinceBarEntry * 0.3f;
          if (learnedBarSamples[lb] < 60000) learnedBarSamples[lb]++;
          saveLearnedBars();
        }
        const bool stepDown = cb < energyBarLevel;
        energyBarLevel = cb;
        whSinceBarEntry = 0.0f;
        barEntryObserved = stepDown;   // čistý vstup do dílku víme jen při poklesu
      }
    }
    if (state.criticalBattery && !batterySessionReachedCritical) {
      batterySessionReachedCritical = true;
      batterySessionHealthSaved = learnBatteryHealthFromCurrentSession();
    }

    state.onBatteryRuntimeSec = (now - batterySessionStartMs) / 1000UL;
    const float predictW = avgDrainW > 0.0f ? avgDrainW : drainW;   // klouzavý průměr = stabilní odhad
    float barRemWh = 0.0f, barTotWh = 0.0f;
    if (predictW > 0.0f && estimateRuntimeFromBars(barRemWh, barTotWh)) {
      // lepší model: ukotveno na naučenou energii dílků (Peukert/účinnost už v tom je)
      state.runtimeRemainingEstimateSec = barRemWh > 0.0f ? (int32_t)((barRemWh / predictW) * 3600.0f) : 0;
      state.runtimeTotalEstimateSec = barTotWh > 0.0f ? (int32_t)((barTotWh / predictW) * 3600.0f) : -1;
    } else {
      // prior/fallback: Ah + Peukert, dokud není naučeno dost dílků
      const float usableWh = usableBatteryWhAtDrain(predictW);
      const float availableWh = usableWh * batterySessionStartFraction;
      if (predictW > 0.0f && usableWh > 0.0f) {
        const float remainingWh = availableWh - batterySessionUsedWh;
        state.runtimeRemainingEstimateSec = remainingWh > 0.0f ? (int32_t)((remainingWh / predictW) * 3600.0f) : 0;
        state.runtimeTotalEstimateSec = (int32_t)((usableWh / predictW) * 3600.0f);
      } else {
        state.runtimeRemainingEstimateSec = -1;
        state.runtimeTotalEstimateSec = -1;
      }
    }
  } else {
    if (batterySessionActive) {
      state.lastBatteryRunSec = (millis() - batterySessionStartMs) / 1000UL;
      if (batterySessionReachedCritical && !batterySessionHealthSaved) {
        batterySessionHealthSaved = learnBatteryHealthFromCurrentSession();
      }
      batterySessionActive = false;
      avgDrainW = 0.0f;
      energyBarLevel = -1;
      whSinceBarEntry = 0.0f;
      barEntryObserved = false;
    }
    state.onBatteryRuntimeSec = 0;
    state.runtimeRemainingEstimateSec = -1;
    state.runtimeTotalEstimateSec = -1;
  }
  state.batteryHealthPercent = (uint8_t)(settings.batteryHealthFactor * 100.0f + 0.5f);
  state.healthWarning = state.batteryHealthPercent < settings.healthWarnPercent;
  state.runtimeWarning = state.onBattery && state.runtimeRemainingEstimateSec >= 0 &&
                         state.runtimeRemainingEstimateSec < (int32_t)settings.minRuntimeMinutes * 60;
  detectEvents();
}

bool waitForClockActivity() {
  const uint32_t started = millis();
  uint8_t last = readPins() & 1;
  uint32_t spins = 0;
  while (millis() - started < CAPTURE_TRIGGER_TIMEOUT_MS) {
    const uint8_t now = readPins() & 1;
    if (now != last) return true;
    last = now;
    if ((++spins & 0x3FF) == 0) yield();   // ať busy-wait nemonopolizuje jádro
  }
  return false;
}

bool captureFast() {
  if (!waitForClockActivity()) return false;
  noInterrupts();
  for (uint32_t i = 0; i < CAPTURE_SAMPLES; i++) {
    samples[i] = readPins();
  }
  interrupts();
  return true;
}

uint8_t extractBits(bool rising, int8_t sampleOffset, bool invert, uint8_t* bits, uint16_t maxBits) {
  uint16_t count = 0;
  uint8_t last = samples[0] & 1;
  for (uint32_t i = 1; i < CAPTURE_SAMPLES; i++) {
    const uint8_t clk = samples[i] & 1;
    const bool hit = rising ? (!last && clk) : (last && !clk);
    if (hit) {
      const int32_t j = (int32_t)i + sampleOffset;
      if (j >= 0 && j < (int32_t)CAPTURE_SAMPLES && count < maxBits) {
        uint8_t bit = (samples[j] >> 1) & 1;
        if (invert) bit ^= 1;
        bits[count++] = bit;
      }
    }
    last = clk;
  }
  return count > 255 ? 255 : (uint8_t)count;
}

uint8_t packBits(const uint8_t* bits, uint8_t bitCountIn, bool lsbFirst,
                 uint8_t phase, uint8_t* out, uint8_t maxOut) {
  uint8_t count = 0;
  for (uint8_t i = phase; i + 8 <= bitCountIn && count < maxOut; i += 8) {
    uint8_t value = 0;
    if (lsbFirst) {
      for (uint8_t n = 0; n < 8; n++) value |= (bits[i + n] & 1) << n;
    } else {
      for (uint8_t n = 0; n < 8; n++) value = (value << 1) | (bits[i + n] & 1);
    }
    out[count++] = value;
  }
  return count;
}

int scoreData(const uint8_t* data, uint8_t dataLen) {
  int score = 0;
  for (uint8_t i = 0; i < dataLen; i++) {
    const uint8_t v = data[i];
    if (v == 0x40 || v == 0xC0) score += 100;
    if ((v & 0xF0) == 0x80) score += 20;
    if (digitFromPattern(v) >= 0) score += 15;
    if (batteryBarsFrom09(v) != 255) score += 10;
    if (loadLevelFrom07(v) != 255) score += 10;
  }
  return score;
}

bool findFrame(const uint8_t* data, uint8_t dataLen, uint8_t* mem, uint8_t* brightness) {
  for (uint8_t i = 0; i + 12 < dataLen; i++) {
    if (data[i] == 0x40 && data[i + 1] == 0xC0 && (data[i + 12] & 0xF0) == 0x80) {
      memcpy(mem, data + i + 2, 10);
      *brightness = data[i + 12];
      return true;
    }
  }
  for (uint8_t i = 0; i + 11 < dataLen; i++) {
    if (data[i] == 0xC0 && (data[i + 11] & 0xF0) == 0x80) {
      memcpy(mem, data + i + 1, 10);
      *brightness = data[i + 11];
      return true;
    }
  }
  return false;
}

bool tryDecodeVariant(uint8_t edge, int8_t offset, uint8_t inv, uint8_t order, uint8_t phase,
                      uint8_t* mem, uint8_t* brightness, uint8_t* dataOut, uint8_t* dataLenOut,
                      uint8_t* bitLenOut, int* scoreOut) {
  uint8_t bits[256];
  uint8_t data[48];
  const uint8_t bitLen = extractBits(edge == 0, offset, inv != 0, bits, sizeof(bits));
  const uint8_t dataLen = packBits(bits, bitLen, order == 0, phase, data, sizeof(data));
  if (!findFrame(data, dataLen, mem, brightness)) return false;

  int score = scoreData(data, dataLen) - phase;
  const int vin = decode3Digits(mem);
  const int vout = decode3Digits(mem + 3);
  if (vin >= 0) score += 500;
  if (vout >= 0) score += 500;
  if (batteryBarsFrom09(mem[9]) != 255) score += 300;
  if (loadLevelFrom07(mem[7]) != 255) score += 200;

  *dataLenOut = dataLen;
  *bitLenOut = bitLen;
  *scoreOut = score;
  memcpy(dataOut, data, dataLen);
  return true;
}

void acceptDecodedFrame(const uint8_t* mem, uint8_t brightness, const uint8_t* data, uint8_t dataLen,
                        int score, uint8_t edge, int8_t offset, uint8_t inv, uint8_t order,
                        uint8_t phase, uint8_t bitLen) {
  rawDebug.score = score;
  rawDebug.dataLen = dataLen;
  memcpy(rawDebug.data, data, dataLen);
  memcpy(rawDebug.mem, mem, 10);
  rawDebug.brightness = brightness;
  rawDebug.edge = edge;
  rawDebug.offset = offset;
  rawDebug.invert = inv;
  rawDebug.order = order;
  rawDebug.phase = phase;
  rawDebug.bitLen = bitLen;
  rawDebug.updatedMs = millis();
  updateDecodedState(mem, brightness);
  state.byteCount++;
}

bool decodeCapture() {
  uint8_t bestMem[10] = {0};
  uint8_t bestBrightness = 0;
  uint8_t bestData[48] = {0};
  uint8_t bestDataLen = 0;
  uint8_t bestEdge = 0;
  int8_t bestOffset = 0;
  uint8_t bestInvert = 0;
  uint8_t bestOrder = 0;
  uint8_t bestPhase = 0;
  uint8_t bestBitLen = 0;
  int bestScore = -1;
  bool found = false;

  if (rawDebug.updatedMs != 0) {
    int score = 0;
    uint8_t dataLen = 0;
    uint8_t bitLen = 0;
    if (tryDecodeVariant(rawDebug.edge, rawDebug.offset, rawDebug.invert, rawDebug.order, rawDebug.phase,
                         bestMem, &bestBrightness, bestData, &dataLen, &bitLen, &score)) {
      acceptDecodedFrame(bestMem, bestBrightness, bestData, dataLen, score,
                         rawDebug.edge, rawDebug.offset, rawDebug.invert, rawDebug.order,
                         rawDebug.phase, bitLen);
      return true;
    }
  }

  uint8_t data[48];

  for (uint8_t edge = 0; edge < 2; edge++) {
    for (int8_t offset = -3; offset <= 7; offset++) {
      for (uint8_t inv = 0; inv < 2; inv++) {
        for (uint8_t order = 0; order < 2; order++) {
          for (uint8_t phase = 0; phase < 8; phase++) {
            uint8_t mem[10];
            uint8_t brightness = 0;
            uint8_t dataLen = 0;
            uint8_t bitLen = 0;
            int score = 0;
            if (tryDecodeVariant(edge, offset, inv, order, phase, mem, &brightness, data, &dataLen, &bitLen, &score)) {
              if (score > bestScore) {
                bestScore = score;
                memcpy(bestMem, mem, 10);
                bestBrightness = brightness;
                bestDataLen = dataLen;
                memcpy(bestData, data, dataLen);
                bestEdge = edge;
                bestOffset = offset;
                bestInvert = inv;
                bestOrder = order;
                bestPhase = phase;
                bestBitLen = bitLen;
                found = true;
              }
            }
          }
        }
      }
    }
  }

  if (found) {
    acceptDecodedFrame(bestMem, bestBrightness, bestData, bestDataLen, bestScore,
                       bestEdge, bestOffset, bestInvert, bestOrder, bestPhase, bestBitLen);
    return true;
  }

  state.decodeErrors++;
  return false;
}

void printHex2(Stream& out, uint8_t v) {
  if (v < 0x10) out.print('0');
  out.print(v, HEX);
}

bool requireAuth() {
  lastWebReqMs = millis();   // dorazil HTTP požadavek -> web server prokazatelně žije
  if (server.authenticate(settings.webUser, settings.webPass)) return true;
  server.requestAuthentication();
  return false;
}

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="cs">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>MHpower monitor</title>
  <style>
    :root{color-scheme:dark;--bg:#101214;--panel:#181c20;--line:#2a3036;--text:#eef3f7;--muted:#9ba6b0;--good:#43d17a;--warn:#ffd166;--bad:#ff4a4a;--blue:#5dade2}
    *{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,Segoe UI,sans-serif;background:var(--bg);color:var(--text)}
    header{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:14px 18px;border-bottom:1px solid var(--line)}
    h1{font-size:19px;margin:0;font-weight:750}main{padding:18px;max-width:1080px;margin:0 auto}
    a{color:#9dccff}.banner{display:none;margin:0 0 14px 0;padding:14px 16px;border:1px solid #ff5555;background:#7a1717;color:#fff;border-radius:8px;font-size:25px;font-weight:900}
    .banner.on{display:block}.grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px}
    .tile{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:14px;min-height:104px;overflow:hidden}
    .tile.goodsource{background:#102719;border-color:#2c8e55;box-shadow:0 0 0 1px #1f623d inset}
    .tile.danger{background:#3b1010;border-color:#b82b2b;box-shadow:0 0 0 1px #6e1717 inset}
    .label{color:var(--muted);font-size:13px;margin-bottom:8px}.value{font-size:30px;font-weight:780;line-height:1.1}.unit{color:var(--muted);font-size:15px;margin-left:3px}
    .status{display:flex;flex-wrap:wrap;gap:8px;margin:14px 0}.pill{padding:7px 10px;border:1px solid var(--line);border-radius:999px;background:#14181c;font-size:14px;color:var(--muted)}
    .pill.good{color:var(--good);border-color:#245c3b}.pill.warn{color:var(--warn);border-color:#6d5a25}.pill.bad{color:#fff;background:#8f1f1f;border-color:#e04444;font-weight:800}
    .bars{display:flex;gap:5px;height:28px;align-items:end;margin:8px 0 2px}.bar{width:22px;background:#26313a;border:1px solid #39444d;border-radius:3px;height:22px}
    .bar.on{background:var(--good);border-color:#6fed9d}.bar.charge.on{background:var(--warn);border-color:#ffe49a;animation:blink 1s steps(2,start) infinite}.bar.critical.on{background:var(--bad);border-color:#ff9b9b;animation:blink .7s steps(2,start) infinite}.bar.load.on{background:var(--blue);border-color:#8fd0ff}.bar.load.hot{background:var(--bad);border-color:#ff8b8b}
    .small{color:var(--muted);font-size:13px;margin-top:6px}.footer{margin-top:18px;border-top:1px solid var(--line);padding:10px 0;color:#8f9aa4;font-size:13px;text-align:center}
    @keyframes blink{50%{opacity:.35}}
    @media(max-width:860px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}.value{font-size:26px}header{align-items:flex-start;flex-direction:column}}
  </style>
</head>
<body>
  <header><h1><span id="pageTitle">Monitoring zdroje MHpower 500W</span> <span class="small" id="deviceName"></span></h1><div class="small"><span id="age">...</span> | <a href="/settings">systém</a> | ID <span id="hwId">-</span></div></header>
  <main>
    <span id="modelWatts" style="display:none">500</span>
    <div class="banner" id="batteryBanner">BĚH NA BATERII</div>
    <section class="grid">
      <div class="tile"><div class="label">Vstup</div><div class="value"><span id="input">-</span><span class="unit">V</span></div></div>
      <div class="tile"><div class="label">Výstup</div><div class="value"><span id="output">-</span><span class="unit">V</span></div></div>
      <div class="tile"><div class="label">Frekvence</div><div class="value"><span id="freq">50</span><span class="unit">Hz</span></div></div>
      <div class="tile" id="avrTile"><div class="label">Síť (AVR)</div><div class="value" id="avr">-</div></div>
      <div class="tile" id="sourceTile"><div class="label">Zdroj</div><div class="value" id="source">-</div></div>
      <div class="tile"><div class="label">Baterie</div><div class="value" id="batteryValue">-</div><div class="bars" id="batteryBars"></div><div class="small" id="batteryText"></div></div>
      <div class="tile"><div class="label">Běh na baterii</div><div class="value" id="runtimeNow">-</div><div class="small" id="runtimeText"></div></div>
      <div class="tile"><div class="label">Zátěž</div><div class="value"><span id="loadPercent">-</span><span class="unit">%</span></div><div class="bars" id="loadBars"></div><div class="small" id="loadText"></div></div>
      <div class="tile"><div class="label">Výkon</div><div class="value"><span id="loadWatts">-</span><span class="unit">W</span></div></div>
      <div class="tile"><div class="label">Alarm</div><div class="value" id="alarm">-</div></div>
      <div class="tile"><div class="label">Data</div><div class="value" id="online">-</div></div>
      <div class="tile"><div class="label">Wi-Fi</div><div class="value"><span id="rssi">-</span><span class="unit">dBm</span></div><div class="small" id="ip"></div></div>
      <div class="tile"><div class="label">Diagnostika</div><div class="small" id="diag"></div></div>
    </section>
    <div class="status" id="pills"></div>
    <h2 class="label" style="margin:18px 0 8px;font-size:14px">Poslední události</h2>
    <div id="events" class="small"></div>
    <div class="footer">Pavel Vlcek v1.11 hkfree.org</div>
  </main>
  <script>
    function val(v){return v===null||v===undefined||v<0?'-':v}
    function signed(v){return v===null||v===undefined?'-':v}
    function fmtTime(sec){if(sec===null||sec===undefined||sec<0)return '-';sec=Math.floor(sec);const h=Math.floor(sec/3600),m=Math.floor((sec%3600)/60),s=sec%60;if(h>0)return h+':'+String(m).padStart(2,'0')+' h';if(m>0)return m+':'+String(s).padStart(2,'0')+' min';return s+' s'}
    function pill(text,cls=''){return `<span class="pill ${cls}">${text}</span>`}
    function bars(id,count,max,cls){const el=document.getElementById(id);let html='',c=Number.isFinite(count)?count:0;for(let i=1;i<=max;i++){let on=i<=c,hot=cls==='load'&&count>=max;html+=`<span class="bar ${cls||''} ${on?'on':''} ${hot&&on?'hot':''}"></span>`}el.innerHTML=html}
    async function renderEvents(){try{const r=await fetch('/api/events',{cache:'no-store'});const j=await r.json();let h='';for(const e of j.events){const t=e.epoch>0?new Date(e.epoch*1000).toLocaleString('cs-CZ'):('běh '+fmtTime(e.uptime));h+=`<div>${t} — ${e.msg}</div>`}document.getElementById('events').innerHTML=h||'<div>žádné události</div>'}catch(e){}}
    async function tick(){let j;try{const r=await fetch('/api/status',{cache:'no-store'});j=await r.json()}catch(e){j={online:false,error:String(e)}}
      input.textContent=val(j.inputVoltage);output.textContent=val(j.outputVoltage);freq.textContent=val(j.frequencyHz);
      avr.textContent=j.overVoltage?'V↑ přepětí':(j.underVoltage?'V↓ podpětí':(j.mainsPresent?'v normě':'-'));avrTile.classList.toggle('danger',!!(j.overVoltage||j.underVoltage));
      if(j.settings&&j.settings.sourceWatts){modelWatts.textContent=j.settings.sourceWatts;pageTitle.textContent='Monitoring zdroje MHpower '+j.settings.sourceWatts+'W'}
      deviceName.textContent=(j.settings&&j.settings.deviceName)?' - '+j.settings.deviceName:'';
      if(j.hwId)hwId.textContent=j.hwId;
      source.textContent=j.onBattery?'BATERIE':(j.mainsPresent?'SÍŤ':'-');sourceTile.classList.toggle('danger',!!j.onBattery);sourceTile.classList.toggle('goodsource',!!j.mainsPresent&&!j.onBattery);batteryBanner.classList.toggle('on',!!j.onBattery);
      alarm.textContent=j.overheat?'TEPLOTA':(j.alarm?'ANO':'OK');online.textContent=j.online?'OK':'OFF';age.textContent='poslední data '+val(j.lastAgeMs)+' ms';
      bars('batteryBars',j.overheat?1:(j.criticalBattery?1:(j.charging?5:j.batteryBars)),5,j.overheat?'critical':(j.criticalBattery?'critical':(j.charging?'charge':'')));bars('loadBars',j.loadLevel,5,'load');
      batteryValue.textContent=j.overheat?'přehřátí':(j.criticalBattery?'0 %':(j.charging?'nabíjení':(Number.isFinite(j.batteryBars)&&j.batteryBars>=0?(j.batteryBars*20)+' %':'-')));loadPercent.textContent=val(j.loadPercent);loadWatts.textContent=val(j.loadWattsEstimate);
      runtimeNow.textContent=fmtTime(j.onBatteryRuntimeSec);runtimeText.textContent='zbývá '+fmtTime(j.runtimeRemainingEstimateSec)+' / celkem '+fmtTime(j.runtimeTotalEstimateSec)+' / posledně '+fmtTime(j.lastBatteryRunSec);
      const batAh=(j.settings&&j.settings.batteryAh!==undefined)?Number(j.settings.batteryAh).toFixed(1):'-';
      const batHealth=j.batteryHealthPercent!==undefined?j.batteryHealthPercent:'-';
      batteryText.textContent=(j.overheat?'teplotní alarm':(j.criticalBattery?'kritický stav před vypnutím':(j.charging?'nabíjení':((j.batteryState||'')+(j.batteryVoltageEstimate!==null&&j.batteryVoltageEstimate!==undefined?' / '+Number(j.batteryVoltageEstimate).toFixed(1)+' V':'')))))+' / '+batAh+' Ah / zdraví '+batHealth+' %';loadText.textContent=j.loadState||'';
      rssi.textContent=signed(j.rssi);ip.innerHTML='IP '+(j.ip||'-')+'<br>maska '+(j.mask||'-')+'<br>brána '+(j.gw||'-')+'<br>DNS '+(j.dns||'-');diag.textContent='rámců '+val(j.frames)+' / heap '+val(j.freeHeap)+' (min '+val(j.minFreeHeap)+', blok '+val(j.maxAllocHeap)+') / err '+val(j.errors)+' / filtr '+val(j.filtered)+' / cap '+val(j.captureOk)+' / tout '+val(j.captureTimeouts)+' | běh '+fmtTime(j.uptimeSec)+' / reset: '+(j.resetReason||'?')+' / TX '+(j.txPowerDbm!==undefined?Number(j.txPowerDbm).toFixed(1)+' dBm':'?')+' / CPU '+val(j.cpuMhz)+' MHz / dílky '+val(j.barsLearned)+'/5'+(j.learnedUsableWh?' ('+val(j.learnedUsableWh)+' Wh)':'');
      let ps='';ps+=pill(j.mainsPresent?'Síť přítomna':'Bez sítě',j.mainsPresent?'good':'bad');if(j.onBattery)ps+=pill('BĚH NA BATERII','bad');if(j.overheat)ps+=pill('PŘEHŘÁTÍ','bad');if(j.criticalBattery)ps+=pill('BATERIE 0 %','bad');if(j.charging)ps+=pill('Nabíjení','good');if(j.batteryFull)ps+=pill('Baterie plná','good');if(j.lowBattery)ps+=pill('Nízká baterie','bad');if(j.overload)ps+=pill('Přetížení','bad');if(j.overVoltage)ps+=pill('PŘEPĚTÍ V↑','warn');if(j.underVoltage)ps+=pill('PODPĚTÍ V↓','warn');if(j.alarm&&!j.overheat)ps+=pill('Alarm','bad');if(j.healthWarning)ps+=pill('Kondice baterie nízká','warn');if(j.runtimeWarning)ps+=pill('Výdrž pod limitem','warn');pills.innerHTML=ps;renderEvents()}
    setInterval(tick,3000);tick();
  </script>
</body>
</html>
)HTML";

void sendStatusJson() {
  const uint32_t age = state.lastFrameMs == 0 ? 0xFFFFFFFFUL : millis() - state.lastFrameMs;
  const bool online = state.lastFrameMs != 0 && age <= FRAME_STALE_MS;
  int learnedUsableWh = 0;
  for (uint8_t b = 0; b <= 5; b++) if (learnedBarSamples[b] > 0) learnedUsableWh += (int)(learnedWhPerBar[b] + 0.5f);
  snprintf(responseBody, sizeof(responseBody),
    "{\"online\":%s,\"frames\":%lu,\"inputVoltage\":%d,\"outputVoltage\":%d,"
    "\"frequencyHz\":50,\"mainsPresent\":%s,\"onBattery\":%s,\"sourceState\":\"%s\","
    "\"alarm\":%s,\"overheat\":%s,\"lowBattery\":%s,\"criticalBattery\":%s,\"charging\":%s,\"batteryFull\":%s,"
    "\"batteryState\":\"%s\",\"batteryVoltageEstimate\":%.1f,\"batteryVoltageRange\":\"%s\","
    "\"batteryPercent\":null,\"batteryBars\":%d,\"batteryHealthPercent\":%u,"
    "\"healthWarning\":%s,\"runtimeWarning\":%s,"
    "\"onBatteryRuntimeSec\":%lu,\"runtimeRemainingEstimateSec\":%ld,"
    "\"runtimeTotalEstimateSec\":%ld,\"lastBatteryRunSec\":%lu,\"loadLevel\":%d,\"loadPercent\":%d,"
    "\"loadWattsEstimate\":%d,\"loadState\":\"%s\",\"overload\":%s,\"displayBlank\":%s,"
    "\"overVoltage\":%s,\"underVoltage\":%s,\"avrState\":\"%s\","
    "\"modeRaw\":\"0x%02X\",\"loadRaw\":\"0x%02X\",\"iconRaw\":\"0x%02X\",\"batteryRaw\":\"0x%02X\","
    "\"brightnessRaw\":\"0x%02X\",\"lastAgeMs\":%lu,\"bytes\":%lu,\"errors\":%lu,"
    "\"filtered\":%lu,\"captureOk\":%lu,\"captureTimeouts\":%lu,\"lastCaptureMs\":%lu,"
    "\"freeHeap\":%u,\"rssi\":%d,\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\","
    "\"resetReason\":\"%s\",\"uptimeSec\":%lu,\"minFreeHeap\":%u,\"maxAllocHeap\":%u,"
    "\"txPowerDbm\":%.2f,\"cpuMhz\":%u,\"barsLearned\":%u,\"learnedUsableWh\":%d,"
    "\"hwId\":\"%s\","
    "\"settings\":{\"deviceName\":\"%s\",\"sourceWatts\":%u,\"batteryAh\":%.1f,\"batterySystemVoltage\":%u,\"batteryInstallDate\":\"%s\","
    "\"healthWarnPercent\":%u,\"minRuntimeMinutes\":%u}}",
    online ? "true" : "false",
    (unsigned long)state.frameCount,
    state.inputVoltage, state.outputVoltage,
    state.mainsPresent ? "true" : "false",
    state.onBattery ? "true" : "false",
    state.sourceState,
    state.alarm ? "true" : "false",
    state.overheat ? "true" : "false",
    state.lowBattery ? "true" : "false",
    state.criticalBattery ? "true" : "false",
    state.charging ? "true" : "false",
    state.batteryFull ? "true" : "false",
    state.batteryState,
    state.batteryVoltageEstimate,
    state.batteryVoltageRange,
    state.batteryBars == 255 ? -1 : state.batteryBars,
    state.batteryHealthPercent,
    state.healthWarning ? "true" : "false",
    state.runtimeWarning ? "true" : "false",
    (unsigned long)state.onBatteryRuntimeSec,
    (long)state.runtimeRemainingEstimateSec,
    (long)state.runtimeTotalEstimateSec,
    (unsigned long)state.lastBatteryRunSec,
    state.loadLevel == 255 ? -1 : state.loadLevel,
    state.loadPercent,
    state.loadWattsEstimate,
    state.loadState,
    state.overload ? "true" : "false",
    state.displayBlank ? "true" : "false",
    state.overVoltage ? "true" : "false",
    state.underVoltage ? "true" : "false",
    state.avrState,
    state.mem[6], state.mem[7], state.mem[8], state.mem[9], state.brightnessCommand,
    age == 0xFFFFFFFFUL ? 0 : (unsigned long)age,
    (unsigned long)state.byteCount,
    (unsigned long)state.decodeErrors,
    (unsigned long)state.filteredFrames,
    (unsigned long)state.captureOk,
    (unsigned long)state.captureTimeouts,
    (unsigned long)state.lastCaptureMs,
    ESP.getFreeHeap(),
    WiFi.RSSI(),
    WiFi.localIP().toString().c_str(),
    WiFi.subnetMask().toString().c_str(),
    WiFi.gatewayIP().toString().c_str(),
    WiFi.dnsIP().toString().c_str(),
    resetReasonStr(bootResetReason),
    (unsigned long)(millis() / 1000UL),
    ESP.getMinFreeHeap(),
    ESP.getMaxAllocHeap(),
    (double)WiFi.getTxPower() / 4.0,
    getCpuFrequencyMhz(),
    barsLearnedCount(),
    learnedUsableWh,
    deviceHwId().c_str(),
    settings.deviceName,
    settings.sourceWatts,
    settings.batteryAh,
    settings.batterySystemVoltage,
    settings.batteryInstallDate,
    settings.healthWarnPercent,
    settings.minRuntimeMinutes);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", responseBody);
}

void handleRoot() {
  if (!requireAuth()) return;
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  if (!requireAuth()) return;
  sendStatusJson();
}

String escHtml(const char* s) {
  String out;
  while (*s) {
    const char c = *s++;
    if (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else if (c == '\'') out += F("&#39;");
    else out += c;
  }
  return out;
}

void handleSettings() {
  if (!requireAuth()) return;

  if (server.method() == HTTP_POST) {
    const float oldBatteryAh = settings.batteryAh;
    char oldBatteryDate[sizeof(settings.batteryInstallDate)];
    strncpy(oldBatteryDate, settings.batteryInstallDate, sizeof(oldBatteryDate));
    oldBatteryDate[sizeof(oldBatteryDate) - 1] = 0;

    if (server.hasArg("devName")) {
      String v = server.arg("devName");
      v.trim();
      v.replace("\"", "");
      v.replace("\\", "");
      if (v.length() > 0 && v.length() < sizeof(settings.deviceName)) {
        strncpy(settings.deviceName, v.c_str(), sizeof(settings.deviceName) - 1);
        settings.deviceName[sizeof(settings.deviceName) - 1] = 0;
      }
    }
    if (server.hasArg("webUser")) {
      String v = server.arg("webUser");
      v.trim();
      if (v.length() > 0 && v.length() < sizeof(settings.webUser)) {
        strncpy(settings.webUser, v.c_str(), sizeof(settings.webUser) - 1);
        settings.webUser[sizeof(settings.webUser) - 1] = 0;
      }
    }
    if (server.hasArg("webPass")) {
      String v = server.arg("webPass");
      if (v.length() > 0 && v.length() < sizeof(settings.webPass)) {
        strncpy(settings.webPass, v.c_str(), sizeof(settings.webPass) - 1);
        settings.webPass[sizeof(settings.webPass) - 1] = 0;
      }
    }
    if (server.hasArg("ssid")) {
      String v = server.arg("ssid");
      v.trim();
      if (v.length() > 0 && v.length() < sizeof(settings.wifiSsid)) {
        strncpy(settings.wifiSsid, v.c_str(), sizeof(settings.wifiSsid) - 1);
        settings.wifiSsid[sizeof(settings.wifiSsid) - 1] = 0;
      }
    }
    if (server.hasArg("pass")) {
      String v = server.arg("pass");
      if (v.length() > 0 && v.length() < sizeof(settings.wifiPass)) {   // prázdné = beze změny
        strncpy(settings.wifiPass, v.c_str(), sizeof(settings.wifiPass) - 1);
        settings.wifiPass[sizeof(settings.wifiPass) - 1] = 0;
      }
    }
    if (server.hasArg("snmp")) {
      String v = server.arg("snmp");
      v.trim();
      if (v.length() > 0 && v.length() < sizeof(settings.snmpCommunity)) {
        strncpy(settings.snmpCommunity, v.c_str(), sizeof(settings.snmpCommunity) - 1);
        settings.snmpCommunity[sizeof(settings.snmpCommunity) - 1] = 0;
      }
    }
    if (server.hasArg("ntp")) {
      String v = server.arg("ntp");
      v.trim();
      if (v.length() > 0 && v.length() < sizeof(settings.ntpServer)) {
        strncpy(settings.ntpServer, v.c_str(), sizeof(settings.ntpServer) - 1);
        settings.ntpServer[sizeof(settings.ntpServer) - 1] = 0;
        configTime(0, 0, settings.ntpServer, "time.google.com");   // hned přepnout NTP
      }
    }
    if (server.hasArg("watts")) {
      const uint16_t w = (uint16_t)server.arg("watts").toInt();
      if (isValidWatts(w)) { settings.sourceWatts = w; settings.batterySystemVoltage = batteryVoltageForWatts(w); }
    }
    if (server.hasArg("batAh")) {
      const float ah = server.arg("batAh").toFloat();
      if (ah >= 1.0 && ah <= 500.0) settings.batteryAh = ah;
    }
    if (server.hasArg("batDate")) {
      String v = server.arg("batDate");
      v.trim();
      if (v.length() > 0 && v.length() < sizeof(settings.batteryInstallDate)) {
        strncpy(settings.batteryInstallDate, v.c_str(), sizeof(settings.batteryInstallDate) - 1);
        settings.batteryInstallDate[sizeof(settings.batteryInstallDate) - 1] = 0;
      }
    }
    if (server.hasArg("health")) {
      const int v = server.arg("health").toInt();
      if (v >= 1 && v <= 100) settings.healthWarnPercent = (uint8_t)v;
    }
    if (server.hasArg("minRun")) {
      const int v = server.arg("minRun").toInt();
      if (v >= 1 && v <= 1440) settings.minRuntimeMinutes = (uint16_t)v;
    }
    if (fabs(settings.batteryAh - oldBatteryAh) > 0.05f || strcmp(settings.batteryInstallDate, oldBatteryDate) != 0) {
      settings.batteryHealthFactor = 1.0f;
      state.batteryHealthPercent = 100;
      resetLearnedBars();   // nová baterie -> zahodit naučenou tabulku Wh/dílek
    }
    saveSettings();
    server.sendHeader("Location", "/settings?saved=1");
    server.send(303);
    return;
  }

  String html;
  html.reserve(9200);
  html += F("<!doctype html><html lang='cs'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>MHpower systém</title><style>:root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;background:#101214;color:#eef3f7;font-family:system-ui,-apple-system,Segoe UI,sans-serif}header{padding:14px 18px;border-bottom:1px solid #2a3036}main{padding:18px;max-width:1080px;margin:0 auto}h1{font-size:19px;margin:0}a{color:#9dccff}.grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px}.field,.panel{background:#181c20;border:1px solid #2a3036;border-radius:8px;padding:12px;min-width:0}label{display:block;color:#b7c9d8;font-size:13px;margin-bottom:7px}input,select{width:100%;background:#101418;color:#eef3f7;border:1px solid #34404a;border-radius:6px;padding:10px;font-size:15px}button{border:0;border-radius:7px;padding:10px 14px;font-size:15px;font-weight:700;background:#2d8cff;color:white;margin:14px 8px 0 0}.restart{background:#8f1f1f}.note{color:#9ba6b0;margin-top:12px}.sectionTitle{margin:22px 0 10px;font-size:16px}.footer{margin-top:18px;border-top:1px solid #2a3036;padding:10px 0;color:#8f9aa4;font-size:13px;text-align:center}@media(max-width:860px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}}@media(max-width:520px){.grid{grid-template-columns:1fr}}</style></head><body>");
  html += F("<header><h1>Monitoring zdroje MHpower ");
  html += settings.sourceWatts;
  html += F("W");
  if (settings.deviceName[0]) {
    html += F(" - ");
    html += escHtml(settings.deviceName);
  }
  html += F("</h1></header><main><p><a href='/'>zpět na monitor</a></p>");
  if (server.hasArg("saved")) html += F("<p class='note'>Uloženo.</p>");
  html += F("<form method='post' action='/settings'>");

  html += F("<h2 class='sectionTitle'>Zařízení a síť</h2><section class='grid'>");
  html += F("<div class='field'><label>Pojmenování zdroje</label><input name='devName' maxlength='32' value='");
  html += escHtml(settings.deviceName);
  html += F("'></div><div class='field'><label>Wi-Fi SSID</label><input name='ssid' maxlength='32' value='");
  html += escHtml(settings.wifiSsid);
  html += F("'></div><div class='field'><label>Wi-Fi heslo</label><input name='pass' maxlength='64' type='password' placeholder='(beze změny)' value='");
  html += F("'></div><div class='field'><label>Web uživatel</label><input name='webUser' maxlength='16' value='");
  html += escHtml(settings.webUser);
  html += F("'></div><div class='field'><label>Web heslo</label><input name='webPass' maxlength='32' type='password' placeholder='(beze změny)' value='");
  html += F("'></div><div class='field'><label>SNMP community</label><input name='snmp' maxlength='32' value='");
  html += escHtml(settings.snmpCommunity);
  html += F("'></div><div class='field'><label>NTP server</label><input name='ntp' maxlength='47' value='");
  html += escHtml(settings.ntpServer);
  html += F("'></div></section>");
  html += F("<p class='note'>Když se Wi-Fi nepřipojí (do 15 s po startu nebo po 5 min výpadku), naskočí záchranný hotspot <b>");
  html += escHtml(apSsid().c_str());
  html += F("</b> (heslo <b>");
  html += AP_FALLBACK_PASS;
  html += F("</b>), web pak běží na <b>http://192.168.4.1</b>. Po obnovení Wi-Fi se sám vypne.</p>");
  if (apFallbackActive) html += F("<p class='note' style='color:#ffd479'>Záchranný hotspot je právě aktivní.</p>");
  html += F("<p class='note'>Trvalý HW identifikátor desky (MAC, nemění se reflashem ani smazáním nastavení): <b>");
  html += escHtml(deviceHwId().c_str());
  html += F("</b> &mdash; SNMP idx 45.</p>");

  html += F("<h2 class='sectionTitle'>Zdroj a baterie</h2><section class='grid'>");
  html += F("<div class='field'><label>Typ zdroje (napětí baterie se nastaví automaticky)</label><select name='watts'>");
  for (uint8_t i = 0; i < MPU_WATTS_N; i++) {
    html += F("<option value='");
    html += MPU_WATTS[i];
    html += "'";
    if (settings.sourceWatts == MPU_WATTS[i]) html += F(" selected");
    html += F(">MHpower ");
    html += MPU_WATTS[i];
    html += F("W (");
    html += batteryVoltageForWatts(MPU_WATTS[i]);
    html += F("V baterie)</option>");
  }
  html += F("</select></div><div class='field'><label>Kapacita baterie Ah</label><input name='batAh' type='number' min='1' max='500' step='0.1' value='");
  html += String(settings.batteryAh, 1);
  html += F("'></div><div class='field'><label>Datum instalace baterie</label><input name='batDate' type='date' value='");
  html += escHtml(settings.batteryInstallDate);
  html += F("'></div></section>");

  html += F("<h2 class='sectionTitle'>Výdrž a varování</h2><section class='grid'>");
  html += F("<div class='field'><label>Minimální výdrž minut</label><input name='minRun' type='number' min='1' max='1440' value='");
  html += settings.minRuntimeMinutes;
  html += F("'></div><div class='field'><label>Varování zdraví baterie pod %</label><input name='health' type='number' min='1' max='100' value='");
  html += settings.healthWarnPercent;
  html += F("'></div></section>");

  html += F("<p class='note'>Změna kapacity nebo data baterie resetuje naučenou kondici na 100 %.</p>");
  html += F("<button type='submit'>Uložit nastavení</button></form>");
  html += F("<p class='note'>SNMP v1 UDP/161, OID 1.3.6.1.4.1.53864.1.1</p>");

  html += F("<h2 class='sectionTitle'>Firmware a údržba</h2><section class='grid'>");
  html += F("<div class='field'><form id='fwform' method='POST' action='/update' enctype='multipart/form-data'>");
  html += F("<label>Nahrát nový firmware .bin</label><input type='file' id='fwfile' name='update' accept='.bin'>");
  html += F("<button type='submit'>Nahrát firmware</button></form>");
  html += F("<progress id='fwprog' value='0' max='100' style='display:none;width:100%;height:14px;margin-top:12px'></progress>");
  html += F("<p class='note' id='fwstat'>Po nahrání se ESP32 sám restartuje.</p></div>");
  html += F("<div class='field'><label>Restart zařízení</label>");
  html += F("<form method='post' action='/restart'><button class='restart' type='submit'>Restartovat ESP32</button></form></div>");
  html += F("</section>");

  html += F("<div class='footer'>Pavel Vlcek v1.11 hkfree.org</div></main>");
  html += F("<script>(function(){var f=document.getElementById('fwform');if(!f)return;"
            "f.addEventListener('submit',function(e){e.preventDefault();"
            "var fi=document.getElementById('fwfile');if(!fi.files.length){return}"
            "var p=document.getElementById('fwprog'),s=document.getElementById('fwstat'),x=new XMLHttpRequest();"
            "p.style.display='block';p.value=0;s.textContent='Nahrávám 0 %';"
            "x.upload.onprogress=function(ev){if(ev.lengthComputable){p.value=Math.round(ev.loaded/ev.total*100);s.textContent='Nahrávám '+p.value+' %'}};"
            "x.onload=function(){if(x.status==200){p.value=100;s.textContent='Hotovo: '+x.responseText+' \\u2014 ESP32 se restartuje\\u2026';setTimeout(function(){location.href='/'},9000)}else{s.textContent='Chyba '+x.status+': '+x.responseText}};"
            "x.onerror=function(){s.textContent='Chyba spojen\\u00ed p\\u0159i nahr\\u00e1v\\u00e1n\\u00ed'};"
            "x.open('POST','/update');x.send(new FormData(f))})})();</script>");
  html += F("</body></html>");
  server.send(200, "text/html", html);
}

void handleRestart() {
  if (!requireAuth()) return;
  server.send(200, "text/plain", "Restartuji ESP32");
  delay(500);
  ESP.restart();
}

void handleUpdatePage() {
  if (!requireAuth()) return;
  server.sendHeader("Location", "/settings");
  server.send(303);
}

bool updateAuthorized = false;   // OTA upload smí do flash zapisovat jen po ověření hesla

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();
#if USE_TASK_WDT
  // Krmit WDT na KAŽDÝ chunk – i na odmítnutém/vadném uploadu. Když se zápis zamítne
  // (špatné heslo / Update.begin selže / Update.write selže – třeba nahraný merged 4MB
  // obraz místo app .ino.bin), prohlížeč stejně dotáhne zbytek souboru a loop task
  // visí v server.handleClient(); bez krmení by po WDT_TIMEOUT_S přišel task-watchdog
  // reboot. Takhle upload v klidu doteče a handleUpdateDone vrátí čistou chybu.
  esp_task_wdt_reset();
#endif
  if (upload.status == UPLOAD_FILE_START) {
    // POZOR: tento callback běží během uploadu, JEŠTĚ než se zavolá handleUpdateDone,
    // takže heslo musíme ověřit tady – jinak by šel firmware nahrát úplně bez hesla.
    updateAuthorized = server.authenticate(settings.webUser, settings.webPass);
    if (!updateAuthorized) return;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) updateAuthorized = false;
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!updateAuthorized) return;
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.abort();
      updateAuthorized = false;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (updateAuthorized) Update.end(true);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    updateAuthorized = false;
  }
}

void handleUpdateDone() {
  if (!requireAuth()) return;
  const bool ok = updateAuthorized && !Update.hasError();
  updateAuthorized = false;
  server.send(200, "text/plain", ok ? "OK, restartuji" : "Chyba uploadu (neověřeno nebo vadný soubor)");
  delay(500);
  if (ok) ESP.restart();
}

const uint32_t SNMP_BASE_OID[] = {1, 3, 6, 1, 4, 1, 53864, 1, 1};
const uint8_t SNMP_BASE_LEN = sizeof(SNMP_BASE_OID) / sizeof(SNMP_BASE_OID[0]);
const uint8_t SNMP_MAX_INDEX = 48;

bool readBerLen(const uint8_t* data, int total, int& pos, int& len) {
  if (pos >= total) return false;
  uint8_t b = data[pos++];
  if ((b & 0x80) == 0) {
    len = b;
    return pos + len <= total;
  }
  const uint8_t count = b & 0x7F;
  if (count == 0 || count > 2 || pos + count > total) return false;
  len = 0;
  for (uint8_t i = 0; i < count; i++) len = (len << 8) | data[pos++];
  return pos + len <= total;
}

bool expectTlv(const uint8_t* data, int total, int& pos, uint8_t tag, int& len) {
  if (pos >= total || data[pos++] != tag) return false;
  return readBerLen(data, total, pos, len);
}

int writeBerLen(uint8_t* out, int pos, int len) {
  if (len < 128) {
    out[pos++] = (uint8_t)len;
  } else {
    out[pos++] = 0x81;
    out[pos++] = (uint8_t)len;
  }
  return pos;
}

int writeTlv(uint8_t* out, int pos, uint8_t tag, const uint8_t* value, int len) {
  out[pos++] = tag;
  pos = writeBerLen(out, pos, len);
  memcpy(out + pos, value, len);
  return pos + len;
}

int writeIntegerTlv(uint8_t* out, int pos, int32_t value) {
  uint8_t v[4] = {
    (uint8_t)((value >> 24) & 0xFF),
    (uint8_t)((value >> 16) & 0xFF),
    (uint8_t)((value >> 8) & 0xFF),
    (uint8_t)(value & 0xFF)
  };
  return writeTlv(out, pos, 0x02, v, 4);
}

int writeStringTlv(uint8_t* out, int pos, const char* text) {
  return writeTlv(out, pos, 0x04, (const uint8_t*)text, strlen(text));
}

int encodeOidContent(const uint32_t* oid, uint8_t oidLen, uint8_t* out) {
  if (oidLen < 2) return 0;
  int pos = 0;
  out[pos++] = (uint8_t)(oid[0] * 40 + oid[1]);
  for (uint8_t i = 2; i < oidLen; i++) {
    uint32_t v = oid[i];
    uint8_t tmp[5];
    uint8_t n = 0;
    tmp[n++] = v & 0x7F;
    v >>= 7;
    while (v > 0 && n < sizeof(tmp)) {
      tmp[n++] = 0x80 | (v & 0x7F);
      v >>= 7;
    }
    while (n > 0) out[pos++] = tmp[--n];
  }
  return pos;
}

int writeOidTlv(uint8_t* out, int pos, const uint32_t* oid, uint8_t oidLen) {
  uint8_t encoded[32];
  const int len = encodeOidContent(oid, oidLen, encoded);
  return writeTlv(out, pos, 0x06, encoded, len);
}

bool decodeOid(const uint8_t* data, int len, uint32_t* oid, uint8_t& oidLen) {
  oidLen = 0;
  if (len <= 0) return false;
  oid[oidLen++] = data[0] / 40;
  oid[oidLen++] = data[0] % 40;
  int pos = 1;
  while (pos < len && oidLen < 16) {
    uint32_t v = 0;
    uint8_t guard = 0;
    do {
      if (pos >= len || guard++ > 5) return false;
      v = (v << 7) | (data[pos] & 0x7F);
    } while (data[pos++] & 0x80);
    oid[oidLen++] = v;
  }
  return true;
}

int oidIndex(const uint32_t* oid, uint8_t oidLen) {
  if (oidLen != SNMP_BASE_LEN + 1 && oidLen != SNMP_BASE_LEN + 2) return -1;
  for (uint8_t i = 0; i < SNMP_BASE_LEN; i++) {
    if (oid[i] != SNMP_BASE_OID[i]) return -1;
  }
  const uint32_t idx = oid[SNMP_BASE_LEN];
  if (oidLen == SNMP_BASE_LEN + 2 && oid[SNMP_BASE_LEN + 1] != 0) return -1;
  return idx >= 1 && idx <= SNMP_MAX_INDEX ? (int)idx : -1;
}

bool oidLess(const uint32_t* a, uint8_t alen, const uint32_t* b, uint8_t blen) {
  const uint8_t n = alen < blen ? alen : blen;
  for (uint8_t i = 0; i < n; i++) {
    if (a[i] != b[i]) return a[i] < b[i];
  }
  return alen < blen;
}

int nextOidIndex(const uint32_t* oid, uint8_t oidLen) {
  for (uint8_t idx = 1; idx <= SNMP_MAX_INDEX; idx++) {
    uint32_t candidate[10];
    memcpy(candidate, SNMP_BASE_OID, sizeof(SNMP_BASE_OID));
    candidate[SNMP_BASE_LEN] = idx;
    if (oidLess(oid, oidLen, candidate, SNMP_BASE_LEN + 1)) return idx;
  }
  return -1;
}

void rawMemText(char* out, size_t len) {
  snprintf(out, len, "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
           state.mem[0], state.mem[1], state.mem[2], state.mem[3], state.mem[4],
           state.mem[5], state.mem[6], state.mem[7], state.mem[8], state.mem[9]);
}

bool getSnmpValue(uint8_t idx, SnmpValue& value) {
  value.type = 0x02;
  value.integer = -1;
  value.text[0] = 0;
  const uint32_t age = state.lastFrameMs == 0 ? 0xFFFFFFFFUL : millis() - state.lastFrameMs;
  const bool online = state.lastFrameMs != 0 && age <= FRAME_STALE_MS;
  switch (idx) {
    case 1: value.type = 0x04; strncpy(value.text, settings.deviceName, sizeof(value.text) - 1); break;
    case 2: value.integer = online ? 1 : 0; break;
    case 3: value.integer = state.inputVoltage; break;
    case 4: value.integer = state.outputVoltage; break;
    case 5: value.integer = 50; break;
    case 6: value.integer = state.mainsPresent ? 1 : 0; break;
    case 7: value.integer = state.onBattery ? 1 : 0; break;
    case 8: value.type = 0x04; strncpy(value.text, state.sourceState, sizeof(value.text) - 1); break;
    case 9: value.integer = state.alarm ? 1 : 0; break;
    case 10: value.integer = state.overheat ? 1 : 0; break;
    case 11: value.integer = state.loadPercent; break;
    case 12: value.integer = state.loadWattsEstimate; break;
    case 13: value.integer = state.loadLevel == 255 ? -1 : state.loadLevel; break;
    case 14: value.integer = state.overload ? 1 : 0; break;
    case 15: value.integer = state.batteryBars == 255 ? -1 : state.batteryBars; break;
    case 16: value.type = 0x04; strncpy(value.text, state.batteryState, sizeof(value.text) - 1); break;
    case 17: value.integer = WiFi.RSSI(); break;
    case 18: value.integer = ESP.getFreeHeap(); break;
    case 19: value.integer = (int32_t)state.frameCount; break;
    case 20: value.integer = (int32_t)state.decodeErrors; break;
    case 21: value.integer = (int32_t)state.filteredFrames; break;
    case 22: value.integer = (int32_t)state.captureTimeouts; break;
    case 23: value.integer = settings.sourceWatts; break;
    case 24: value.integer = (int32_t)(settings.batteryAh * 10.0f + 0.5f); break;
    case 25: value.integer = state.batteryHealthPercent; break;
    case 26: value.integer = (int32_t)state.onBatteryRuntimeSec; break;
    case 27: value.integer = state.runtimeRemainingEstimateSec; break;
    case 28: value.integer = (int32_t)state.lastBatteryRunSec; break;
    case 29: value.type = 0x04; strncpy(value.text, WiFi.localIP().toString().c_str(), sizeof(value.text) - 1); break;
    case 30: value.type = 0x04; rawMemText(value.text, sizeof(value.text)); break;
    case 31: value.integer = state.charging ? 1 : 0; break;
    case 32: value.integer = state.criticalBattery ? 1 : 0; break;
    case 33: value.integer = state.lowBattery ? 1 : 0; break;
    case 34: value.integer = state.batteryFull ? 1 : 0; break;
    case 35: value.integer = state.batteryVoltageEstimate < 0 ? -1 : (int32_t)(state.batteryVoltageEstimate * 10.0f + 0.5f); break;
    case 36: value.integer = age == 0xFFFFFFFFUL ? -1 : (int32_t)age; break;
    case 37: value.integer = state.healthWarning ? 1 : 0; break;
    case 38: value.integer = state.runtimeWarning ? 1 : 0; break;
    case 39: value.integer = (int32_t)(millis() / 1000UL); break;          // uptime [s]
    case 40: value.integer = (int32_t)ESP.getMinFreeHeap(); break;          // min volný heap [B]
    case 41: value.integer = (int32_t)ESP.getMaxAllocHeap(); break;         // největší volný blok [B]
    case 42: value.type = 0x04; strncpy(value.text, resetReasonStr(bootResetReason), sizeof(value.text) - 1); break;  // důvod restartu
    case 43: value.integer = (int32_t)(WiFi.getTxPower() * 10 / 4); break;  // TX výkon [dBm×10]
    case 44: value.integer = (int32_t)getCpuFrequencyMhz(); break;          // takt CPU [MHz]
    case 45: value.type = 0x04; strncpy(value.text, deviceHwId().c_str(), sizeof(value.text) - 1); break;  // HW ID = MAC (trvalý otisk desky)
    case 46: value.integer = state.overVoltage ? 1 : 0; break;   // přepětí (V↑, AVR snižuje)
    case 47: value.integer = state.underVoltage ? 1 : 0; break;  // podpětí (V↓, AVR zvyšuje)
    case 48: value.type = 0x04; strncpy(value.text, state.avrState, sizeof(value.text) - 1); break;  // AVR stav: buck/boost/normal/n-a
    default: return false;
  }
  value.text[sizeof(value.text) - 1] = 0;
  return true;
}

void sendSnmpResponse(const uint8_t* requestIdTlv, int requestIdTlvLen, const char* community,
                      const uint32_t* responseOid, uint8_t responseOidLen, const SnmpValue& value) {
  uint8_t valueTlv[80];
  int valueLen = value.type == 0x04 ? writeStringTlv(valueTlv, 0, value.text) : writeIntegerTlv(valueTlv, 0, value.integer);

  uint8_t oidTlv[40];
  int oidLen = writeOidTlv(oidTlv, 0, responseOid, responseOidLen);

  uint8_t varbindContent[140];
  int varbindContentLen = 0;
  memcpy(varbindContent + varbindContentLen, oidTlv, oidLen);
  varbindContentLen += oidLen;
  memcpy(varbindContent + varbindContentLen, valueTlv, valueLen);
  varbindContentLen += valueLen;

  uint8_t varbind[150];
  int varbindLen = writeTlv(varbind, 0, 0x30, varbindContent, varbindContentLen);
  uint8_t varlist[160];
  int varlistLen = writeTlv(varlist, 0, 0x30, varbind, varbindLen);

  uint8_t pduContent[190];
  int pduContentLen = 0;
  memcpy(pduContent + pduContentLen, requestIdTlv, requestIdTlvLen);
  pduContentLen += requestIdTlvLen;
  pduContentLen = writeIntegerTlv(pduContent, pduContentLen, 0);
  pduContentLen = writeIntegerTlv(pduContent, pduContentLen, 0);
  memcpy(pduContent + pduContentLen, varlist, varlistLen);
  pduContentLen += varlistLen;

  uint8_t pdu[210];
  int pduLen = writeTlv(pdu, 0, 0xA2, pduContent, pduContentLen);

  uint8_t version[] = {0x02, 0x01, 0x00};
  uint8_t communityTlv[50];
  int communityLen = writeStringTlv(communityTlv, 0, community);

  uint8_t messageContent[280];
  int messageContentLen = 0;
  memcpy(messageContent + messageContentLen, version, sizeof(version));
  messageContentLen += sizeof(version);
  memcpy(messageContent + messageContentLen, communityTlv, communityLen);
  messageContentLen += communityLen;
  memcpy(messageContent + messageContentLen, pdu, pduLen);
  messageContentLen += pduLen;

  uint8_t response[300];
  int responseLen = writeTlv(response, 0, 0x30, messageContent, messageContentLen);
  snmpUdp.beginPacket(snmpUdp.remoteIP(), snmpUdp.remotePort());
  snmpUdp.write(response, responseLen);
  snmpUdp.endPacket();
}

void handleSnmp() {
  const int packetSize = snmpUdp.parsePacket();
  if (packetSize <= 0 || packetSize > 484) return;

  uint8_t packet[484];
  const int len = snmpUdp.read(packet, sizeof(packet));
  int pos = 0;
  int seqLen = 0;
  if (!expectTlv(packet, len, pos, 0x30, seqLen)) return;

  int fieldLen = 0;
  if (!expectTlv(packet, len, pos, 0x02, fieldLen)) return;
  pos += fieldLen;

  if (!expectTlv(packet, len, pos, 0x04, fieldLen)) return;
  if (fieldLen <= 0 || fieldLen >= 33) return;
  char community[33];
  memcpy(community, packet + pos, fieldLen);
  community[fieldLen] = 0;
  pos += fieldLen;
  if (strcmp(community, settings.snmpCommunity) != 0) return;

  if (pos >= len) return;
  const uint8_t pduType = packet[pos++];
  if (pduType != 0xA0 && pduType != 0xA1) return;
  int pduLen = 0;
  if (!readBerLen(packet, len, pos, pduLen)) return;

  const int reqIdStart = pos;
  if (!expectTlv(packet, len, pos, 0x02, fieldLen)) return;
  pos += fieldLen;
  const int reqIdTlvLen = pos - reqIdStart;
  if (reqIdTlvLen <= 0 || reqIdTlvLen > 8) return;

  if (!expectTlv(packet, len, pos, 0x02, fieldLen)) return;
  pos += fieldLen;
  if (!expectTlv(packet, len, pos, 0x02, fieldLen)) return;
  pos += fieldLen;
  if (!expectTlv(packet, len, pos, 0x30, fieldLen)) return;
  if (!expectTlv(packet, len, pos, 0x30, fieldLen)) return;
  if (!expectTlv(packet, len, pos, 0x06, fieldLen)) return;

  uint32_t reqOid[16];
  uint8_t reqOidLen = 0;
  if (!decodeOid(packet + pos, fieldLen, reqOid, reqOidLen)) return;

  int idx = pduType == 0xA1 ? nextOidIndex(reqOid, reqOidLen) : oidIndex(reqOid, reqOidLen);
  if (idx < 1 || idx > SNMP_MAX_INDEX) return;

  SnmpValue value;
  if (!getSnmpValue((uint8_t)idx, value)) return;

  uint32_t responseOid[SNMP_BASE_LEN + 2];
  uint8_t responseOidLen = 0;
  if (pduType == 0xA1) {
    memcpy(responseOid, SNMP_BASE_OID, sizeof(SNMP_BASE_OID));
    responseOid[SNMP_BASE_LEN] = (uint32_t)idx;
    responseOid[SNMP_BASE_LEN + 1] = 0;
    responseOidLen = SNMP_BASE_LEN + 2;
  } else {
    memcpy(responseOid, reqOid, reqOidLen * sizeof(uint32_t));
    responseOidLen = reqOidLen;
  }
  sendSnmpResponse(packet + reqIdStart, reqIdTlvLen, community, responseOid, responseOidLen, value);
}

void handleSnmpBurst(uint8_t maxPackets = 8) {
  for (uint8_t i = 0; i < maxPackets; i++) {
    handleSnmp();
    delay(0);
  }
}

void bindSnmpUdp() {
  if (WiFi.status() != WL_CONNECTED) return;
  snmpUdp.stop();
  snmpUdp.begin(SNMP_PORT);
  lastSnmpBindMs = millis();
}

void keepSnmpUdpAlive() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (lastSnmpBindMs == 0 || millis() - lastSnmpBindMs > 30000UL) {
    bindSnmpUdp();
  }
}

// AP je hned u ESP -> snížený vysílací výkon = menší proudové špičky (brownout) i odběr.
// Možnosti: WIFI_POWER_19_5dBm (max) … 15 / 13 / 11 / 8_5 / 7 / 5 / 2 / MINUS_1dBm (min).
const wifi_power_t WIFI_TX_POWER = WIFI_POWER_5dBm;

void onWifiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      lastWifiOkMs = millis();
      bindSnmpUdp();
      startMdns();   // přehlásit mDNS po (re)connectu
      Serial.printf("[WiFi] IP %s\n", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("[WiFi] odpojeno");
      break;
    default: break;
  }
}

// trvalý HW identifikátor desky = MAC z eFuse (přežije reflash i smazání NVS)
String deviceHwId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// SSID záchranného hotspotu: MHpower-<2 bajty MAC> (unikátní, rozpoznatelné)
String apSsid() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[20];
  snprintf(buf, sizeof(buf), "MHpower-%02X%02X", mac[4], mac[5]);
  return String(buf);
}

// nahodí AP hotspot, ale STA nechá běžet (AP_STA) -> reconnect pokračuje na pozadí
void startApFallback() {
  if (apFallbackActive) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid().c_str(), AP_FALLBACK_PASS);
  WiFi.setTxPower(WIFI_TX_POWER);
  apFallbackActive = true;
  Serial.printf("[WiFi] zachranny hotspot %s / %s -> http://%s\n",
                apSsid().c_str(), AP_FALLBACK_PASS, WiFi.softAPIP().toString().c_str());
  logEvent("Záchranný hotspot");
}

// STA se vrátila -> zruš AP a zpět do úsporného STA režimu (kvůli odběru/brownoutu)
void stopApFallback() {
  if (!apFallbackActive) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_TX_POWER);
  apFallbackActive = false;
  Serial.println("[WiFi] hotspot vypnut (STA obnovena)");
  logEvent("Hotspot vypnut");
}

// aktivní dohled: po WIFI_RETRY_MS reconnect, po WIFI_FALLBACK_MS záchranný hotspot
void maintainWifi() {
  const uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    lastWifiOkMs = now;
    if (apFallbackActive) stopApFallback();
    return;
  }
  if (lastWifiOkMs == 0) lastWifiOkMs = now;
  // místo nekonečného restartu (ten by špatně zadanou WiFi nikdy neopravil) nahodíme hotspot
  if (!apFallbackActive && now - lastWifiOkMs > WIFI_FALLBACK_MS) startApFallback();
  // poslední pojistka: hodina bez STA a nikdo není připojený na hotspot -> tvrdý reboot
  if (now - lastWifiOkMs > WIFI_HARD_REBOOT_MS && WiFi.softAPgetStationNum() == 0) {
    Serial.println("[WiFi] >1 h bez WiFi a bez klientu -> restart");
    delay(50);
    ESP.restart();
  }
  if (now - lastWifiRetryMs > WIFI_RETRY_MS) {
    lastWifiRetryMs = now;
    Serial.println("[WiFi] reconnect...");
    WiFi.disconnect();
    WiFi.begin(settings.wifiSsid, settings.wifiPass);
  }
}

// pojistka proti zaseknutému web serveru: když dlouho nikdo neprošel, recykluj listen socket.
// Otevřený dashboard pollne á 3 s -> požadavky chodí -> server je prokazatelně živý a nerecykluje se.
void maintainWeb() {
  const uint32_t now = millis();
  if (updateAuthorized) { lastWebReqMs = now; return; }   // probíhá OTA -> serverem nehýbat
  if (lastWebReqMs == 0) { lastWebReqMs = now; return; }
  if (now - lastWebReqMs > WEB_REINIT_MS) {
    server.stop();
    server.begin();
    lastWebReqMs = now;
    Serial.println("[web] recyklace listen socketu (pojistka proti zaseknuti)");
  }
}

void sendEventsJson() {
  int n = snprintf(responseBody, sizeof(responseBody), "{\"ntp\":%s,\"events\":[", nowEpoch() ? "true" : "false");
  for (uint8_t i = 0; i < eventCount; i++) {
    const uint8_t idx = (uint8_t)((eventHead + EVENT_LOG_SIZE - 1 - i) % EVENT_LOG_SIZE);  // nejnovější první
    const LogEvent &e = eventLog[idx];
    n += snprintf(responseBody + n, sizeof(responseBody) - n, "%s{\"epoch\":%lu,\"uptime\":%lu,\"msg\":\"%s\"}",
                  i ? "," : "", (unsigned long)e.epoch, (unsigned long)e.uptime, e.msg);
    if (n > (int)sizeof(responseBody) - 120) break;
  }
  snprintf(responseBody + n, sizeof(responseBody) - n, "]}");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", responseBody);
}
void handleEvents() {
  if (!requireAuth()) return;
  sendEventsJson();
}

// odhad číslice ze segmentového bajtu vč. dosud nemapované „9" (tečka/DP = bit7 se ignoruje)
int8_t guessDigitFromSegments(uint8_t v) {
  static const uint8_t tbl[10] = {0x77,0x41,0x6E,0x6D,0x59,0x3D,0x3F,0x61,0x7F,0x7D};
  const uint8_t m = v & 0x7F;
  for (uint8_t d = 0; d < 10; d++) if (tbl[d] == m) return d;
  return -1;
}

void sendDigitScan() {
  const char* posName = unkLog.lastPos < 3 ? "vstup" : "vystup";
  const uint8_t col = unkLog.lastPos % 3;   // 0=stovky 1=desitky 2=jednotky
  const char* colName = col == 0 ? "stovky" : (col == 1 ? "desitky" : "jednotky");
  int n = snprintf(responseBody, sizeof(responseBody),
                   "nezname cislice displeje - fw v1.11\n"
                   "celkem zachyceno: %lu\n", (unsigned long)unkLog.total);
  if (unkLog.total) {
    const int8_t g = guessDigitFromSegments(unkLog.lastPattern);
    n += snprintf(responseBody + n, sizeof(responseBody) - n,
                  "naposledy: 0x%02X na pozici %u (%s/%s) | kontext %02X %02X %02X  %02X %02X %02X"
                  " | pred %lu s (uptime %lu s, ntp %s)\n",
                  unkLog.lastPattern, unkLog.lastPos, posName, colName,
                  unkLog.lastCtx[0], unkLog.lastCtx[1], unkLog.lastCtx[2],
                  unkLog.lastCtx[3], unkLog.lastCtx[4], unkLog.lastCtx[5],
                  (unsigned long)(unkLog.lastEpoch ? (nowEpoch() - unkLog.lastEpoch) : 0),
                  (unsigned long)unkLog.lastUptime, unkLog.lastEpoch ? "ano" : "ne");
    if (g >= 0) n += snprintf(responseBody + n, sizeof(responseBody) - n,
                              "  -> posledni vzor 0x%02X odpovida cislici %d\n", unkLog.lastPattern, g);
  }
  n += snprintf(responseBody + n, sizeof(responseBody) - n, "histogram:\n");
  if (!unkLog.distinct) n += snprintf(responseBody + n, sizeof(responseBody) - n, "  (zatim nic)\n");
  for (uint8_t i = 0; i < unkLog.distinct; i++) {
    const int8_t g = guessDigitFromSegments(unkLog.pattern[i]);
    if (g >= 0)
      n += snprintf(responseBody + n, sizeof(responseBody) - n,
                    "  0x%02X x%u  -> nejspis %d\n", unkLog.pattern[i], unkLog.count[i], g);
    else
      n += snprintf(responseBody + n, sizeof(responseBody) - n,
                    "  0x%02X x%u  -> ?\n", unkLog.pattern[i], unkLog.count[i]);
  }
  snprintf(responseBody + n, sizeof(responseBody) - n,
           "pozn.: predikce z bit-layoutu 9 = 0x7D (a,b,c,d,f,g; s teckou 0xFD). Po potvrzeni pridat do digitFromPattern().\n");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", responseBody);
}
void handleDigitScan() {
  if (!requireAuth()) return;
  sendDigitScan();
}

void sendIconScan() {
  int n = snprintf(responseBody, sizeof(responseBody),
                   "icon-scan ikon displeje (hledame V-nahoru/V-dolu pri prepeti/podpeti) - fw v1.11\n"
                   "celkem vzorku: %lu | normalni pasmo vstupu 207-253 V\n"
                   "mem[6]=mode, mem[8]=ikony; porovnej hodnoty pri prepeti/podpeti s normalem.\n",
                   (unsigned long)iconLog.total);
  if (iconLog.hiV >= 0)
    n += snprintf(responseBody + n, sizeof(responseBody) - n,
                  "PREPETI (>253V): mode=0x%02X icon=0x%02X pri %d V, pred %lu s\n",
                  iconLog.hiMode, iconLog.hiIcon, iconLog.hiV,
                  (unsigned long)(iconLog.hiEpoch ? (nowEpoch() - iconLog.hiEpoch) : 0));
  else n += snprintf(responseBody + n, sizeof(responseBody) - n, "PREPETI (>253V): zatim nezachyceno\n");
  if (iconLog.loV >= 0)
    n += snprintf(responseBody + n, sizeof(responseBody) - n,
                  "PODPETI (<207V): mode=0x%02X icon=0x%02X pri %d V, pred %lu s\n",
                  iconLog.loMode, iconLog.loIcon, iconLog.loV,
                  (unsigned long)(iconLog.loEpoch ? (nowEpoch() - iconLog.loEpoch) : 0));
  else n += snprintf(responseBody + n, sizeof(responseBody) - n, "PODPETI (<207V): zatim nezachyceno\n");
  n += snprintf(responseBody + n, sizeof(responseBody) - n, "histogram mem[6] (mode):\n");
  for (uint8_t i = 0; i < iconLog.modeDistinct; i++)
    n += snprintf(responseBody + n, sizeof(responseBody) - n, "  0x%02X x%u\n", iconLog.mode[i], iconLog.modeCount[i]);
  n += snprintf(responseBody + n, sizeof(responseBody) - n, "histogram mem[8] (ikony):\n");
  for (uint8_t i = 0; i < iconLog.iconDistinct; i++)
    n += snprintf(responseBody + n, sizeof(responseBody) - n, "  0x%02X x%u\n", iconLog.icon[i], iconLog.iconCount[i]);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", responseBody);
}

void handleIconScan() {
  if (!requireAuth()) return;
  sendIconScan();
}

// hostname pro mDNS i DHCP: z deviceName ponech jen [a-z0-9-], jinak "mhpower"
String deviceHostname() {
  String h;
  for (const char* p = settings.deviceName; *p; p++) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c += 32;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') h += c;
  }
  if (h.length() == 0) h = "mhpower";
  return h;
}

// (re)spustí mDNS responder na aktuální hostname (idempotentní – nejdřív ukončí starý)
void startMdns() {
  MDNS.end();
  if (MDNS.begin(deviceHostname().c_str())) MDNS.addService("http", "tcp", 80);   // -> http://<název>.local
}

void setupWifiAndWeb() {
  WiFi.persistent(false);
  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(deviceHostname().c_str());
  WiFi.setSleep(WIFI_PS_MIN_MODEM);        // modem-sleep: rádio spí mezi beacony = nižší odběr
  WiFi.setTxPower(WIFI_TX_POWER);          // snížit hned po nastavení módu
  WiFi.setAutoReconnect(true);
  WiFi.begin(settings.wifiSsid, settings.wifiPass);
  WiFi.setTxPower(WIFI_TX_POWER);          // a znovu po begin (některé verze ho resetují)

  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) {
    delay(100);
  }

  server.on("/", handleRoot);
  server.on("/api/status", handleStatus);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/settings", HTTP_POST, handleSettings);
  server.on("/restart", HTTP_POST, handleRestart);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.on("/api/events", handleEvents);
  server.on("/api/digitscan", handleDigitScan);
  server.on("/api/iconscan", handleIconScan);
  server.begin();
  bindSnmpUdp();
  startMdns();
  lastWifiOkMs = millis();
  if (WiFi.status() != WL_CONNECTED) startApFallback();   // WiFi nenajeta -> hned záchranný hotspot
}

void setup() {
  Serial.begin(115200);
  bootResetReason = esp_reset_reason();   // proč naběhl (brownout / watchdog / panika / …)

  setCpuFrequencyMhz(160);   // 240->160 MHz: nižší odběr; WiFi i capture v pohodě
  btStop();                  // Bluetooth nepoužíváme -> vypnout řadič (pár mA)

#if USE_TASK_WDT
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t twdt = {};
  twdt.timeout_ms = WDT_TIMEOUT_S * 1000;
  twdt.idle_core_mask = 0;
  twdt.trigger_panic = true;
  esp_task_wdt_init(&twdt);
  #else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  #endif
  esp_task_wdt_add(NULL);   // hlídej loop task -> při zaseknutí čistý reboot
#endif

  pinMode(PIN_CLK, INPUT);
  pinMode(PIN_DIN, INPUT);

  delay(300);
  Serial.printf("\n[BOOT] reset=%s heap=%u\n", resetReasonStr(bootResetReason), ESP.getFreeHeap());
  loadSettings();
  setupWifiAndWeb();
  configTime(0, 0, settings.ntpServer, "time.google.com");   // NTP (UTC) pro časové značky událostí
  logEvent("Start zařízení");
  minFreeHeapBoot = ESP.getFreeHeap();
}

void loop() {
#if USE_TASK_WDT
  esp_task_wdt_reset();
#endif
  maintainWifi();
  maintainWeb();
  keepSnmpUdpAlive();
  server.handleClient();
  handleSnmpBurst();
  WiFiClient probe = server.client();
  if (probe && probe.connected()) {
    server.handleClient();
    handleSnmpBurst();
    delay(1);
    return;
  }
  const uint32_t now = millis();
  if (now - lastCaptureAttemptMs >= CAPTURE_INTERVAL_MS) {   // throttle snímání
    lastCaptureAttemptMs = now;
    if (captureFast()) {
      state.captureOk++;
      state.lastCaptureMs = millis();
      decodeCapture();
    } else {
      state.captureTimeouts++;
    }
    server.handleClient();
    handleSnmpBurst();
  } else {
    delay(10);   // mezi snímky nech systém dýchat (nižší spin i odběr)
  }

  delay(1);   // pustí IDLE task a nakrmí watchdog (delay(0) na ESP32 NEpouští)
}
