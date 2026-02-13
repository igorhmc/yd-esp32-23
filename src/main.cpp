#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include "soc/soc_caps.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

#ifndef DEVICE_HOSTNAME
#define DEVICE_HOSTNAME "esp32"
#endif

#ifndef MATRIX_DATA_PIN
#define MATRIX_DATA_PIN 17
#endif

#ifndef MATRIX_WIDTH
#define MATRIX_WIDTH 8
#endif

#ifndef MATRIX_HEIGHT
#define MATRIX_HEIGHT 8
#endif

#ifndef MATRIX_BRIGHTNESS_DEFAULT
#define MATRIX_BRIGHTNESS_DEFAULT 32
#endif

static const uint16_t kMatrixLedCount = MATRIX_WIDTH * MATRIX_HEIGHT;

#ifdef LED_BUILTIN
static const int kLedPin = LED_BUILTIN;
#endif

namespace {

struct RgbPinList {
  int pins[4];
  size_t count;
};

struct RgbColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

WebServer gWebServer(80);
Adafruit_NeoPixel gMatrix(kMatrixLedCount, MATRIX_DATA_PIN, NEO_GRB + NEO_KHZ800);

RgbColor gLedColor = {0, 0, 0};
uint8_t gMatrixBrightness = MATRIX_BRIGHTNESS_DEFAULT;
bool gMatrixReady = false;
bool gMatrixTestRunning = false;
uint16_t gMatrixTestIndex = 0;
unsigned long gMatrixLastStepMs = 0;

bool gMdnsStarted = false;
bool gWebServerStarted = false;

int getBuiltinRgbDataPin() {
#if defined(RGB_BUILTIN)
  if (RGB_BUILTIN >= SOC_GPIO_PIN_COUNT) {
    return RGB_BUILTIN - SOC_GPIO_PIN_COUNT;
  }
  return RGB_BUILTIN;
#else
  return -1;
#endif
}

void addUniquePin(RgbPinList &list, int pin) {
  if (pin < 0 || pin >= static_cast<int>(SOC_GPIO_PIN_COUNT)) {
    return;
  }
  for (size_t i = 0; i < list.count; i++) {
    if (list.pins[i] == pin) {
      return;
    }
  }
  if (list.count < (sizeof(list.pins) / sizeof(list.pins[0]))) {
    list.pins[list.count++] = pin;
  }
}

RgbPinList buildRgbPinList() {
  RgbPinList list = {{-1, -1, -1, -1}, 0};
  addUniquePin(list, getBuiltinRgbDataPin());
  addUniquePin(list, 48);
  addUniquePin(list, 38);
  return list;
}

void writeRgbAllPins(const RgbPinList &list, uint8_t r, uint8_t g, uint8_t b) {
  for (size_t i = 0; i < list.count; i++) {
    neopixelWrite(static_cast<uint8_t>(list.pins[i]), r, g, b);
  }
}

void applyBoardLedColor(const RgbColor &color) {
  // Evita conflito de RMT quando a matriz WS2812 estiver ativa.
  if (gMatrixReady) {
    return;
  }

  const RgbPinList rgbPins = buildRgbPinList();
  if (rgbPins.count > 0) {
    writeRgbAllPins(rgbPins, color.r, color.g, color.b);
    return;
  }

#ifdef LED_BUILTIN
  const bool on = (color.r > 0 || color.g > 0 || color.b > 0);
  digitalWrite(kLedPin, on ? HIGH : LOW);
#else
  (void)color;
#endif
}

void applyMatrixSolidColor(const RgbColor &color) {
  if (!gMatrixReady) {
    return;
  }

  gMatrix.setBrightness(gMatrixBrightness);
  const uint32_t packed = gMatrix.Color(color.r, color.g, color.b);
  for (uint16_t i = 0; i < kMatrixLedCount; i++) {
    gMatrix.setPixelColor(i, packed);
  }
  gMatrix.show();
}

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
  gLedColor = {r, g, b};
  gMatrixTestRunning = false;
  applyBoardLedColor(gLedColor);
  applyMatrixSolidColor(gLedColor);
}

void setMatrixBrightness(uint8_t value) {
  gMatrixBrightness = value;
  if (gMatrixReady && !gMatrixTestRunning) {
    applyMatrixSolidColor(gLedColor);
  }
}

String ledHexColor() {
  char hex[8];
  snprintf(hex, sizeof(hex), "#%02X%02X%02X", gLedColor.r, gLedColor.g, gLedColor.b);
  return String(hex);
}

bool parseHexColor(String hex, RgbColor &out) {
  hex.trim();
  if (hex.startsWith("#")) {
    hex.remove(0, 1);
  }
  if (hex.length() != 6) {
    return false;
  }

  char *endPtr = nullptr;
  const unsigned long value = strtoul(hex.c_str(), &endPtr, 16);
  if (endPtr == nullptr || *endPtr != '\0') {
    return false;
  }

  out.r = static_cast<uint8_t>((value >> 16) & 0xFF);
  out.g = static_cast<uint8_t>((value >> 8) & 0xFF);
  out.b = static_cast<uint8_t>(value & 0xFF);
  return true;
}

void saveSettings() {
  Preferences pref;
  if (!pref.begin("ledcfg", false)) {
    return;
  }
  pref.putUChar("r", gLedColor.r);
  pref.putUChar("g", gLedColor.g);
  pref.putUChar("b", gLedColor.b);
  pref.putUChar("br", gMatrixBrightness);
  pref.end();
}

void loadSettings() {
  Preferences pref;
  if (!pref.begin("ledcfg", true)) {
    return;
  }

  const uint8_t r = pref.getUChar("r", 0);
  const uint8_t g = pref.getUChar("g", 0);
  const uint8_t b = pref.getUChar("b", 0);
  const uint8_t br = pref.getUChar("br", MATRIX_BRIGHTNESS_DEFAULT);
  pref.end();

  gMatrixBrightness = br;
  setLedColor(r, g, b);
}

uint32_t colorWheel(uint8_t pos) {
  pos = 255 - pos;
  if (pos < 85) {
    return gMatrix.Color(255 - pos * 3, 0, pos * 3);
  }
  if (pos < 170) {
    pos -= 85;
    return gMatrix.Color(0, pos * 3, 255 - pos * 3);
  }
  pos -= 170;
  return gMatrix.Color(pos * 3, 255 - pos * 3, 0);
}

void startMatrixTest() {
  if (!gMatrixReady || kMatrixLedCount == 0) {
    return;
  }
  gMatrixTestRunning = true;
  gMatrixTestIndex = 0;
  gMatrixLastStepMs = 0;
  Serial.println("[OK] Teste da matriz iniciado.");
}

void tickMatrixTest() {
  if (!gMatrixReady || !gMatrixTestRunning) {
    return;
  }

  const unsigned long now = millis();
  if (now - gMatrixLastStepMs < 55) {
    return;
  }
  gMatrixLastStepMs = now;

  gMatrix.clear();
  const uint8_t wheel = static_cast<uint8_t>((gMatrixTestIndex * 255) / (kMatrixLedCount - 1));
  gMatrix.setPixelColor(gMatrixTestIndex, colorWheel(wheel));
  gMatrix.show();

  gMatrixTestIndex++;
  if (gMatrixTestIndex >= kMatrixLedCount) {
    gMatrixTestRunning = false;
    applyMatrixSolidColor(gLedColor);
    Serial.println("[OK] Teste da matriz concluido.");
  }
}

void initMatrix() {
  gMatrix.begin();
  gMatrixReady = true;
  gMatrix.setBrightness(gMatrixBrightness);
  gMatrix.clear();
  gMatrix.show();
  Serial.printf("[OK] Matriz WS2812 pronta | pin=%d | leds=%u | brilho=%u\n",
                MATRIX_DATA_PIN,
                static_cast<unsigned>(kMatrixLedCount),
                gMatrixBrightness);
}

void rgbTest() {
  const RgbPinList rgbPins = buildRgbPinList();
  if (rgbPins.count == 0) {
    Serial.println("[INFO] RGB onboard nao detectado no variant.");
    return;
  }

  struct ColorStep {
    const char *name;
    uint8_t r;
    uint8_t g;
    uint8_t b;
  };

  const ColorStep steps[] = {
    {"VERMELHO", 24, 0, 0},
    {"VERDE", 0, 24, 0},
    {"AZUL", 0, 0, 24},
    {"BRANCO", 18, 18, 18},
    {"APAGADO", 0, 0, 0},
  };

  Serial.print("Teste RGB onboard nos GPIOs:");
  for (size_t i = 0; i < rgbPins.count; i++) {
    Serial.printf(" %d", rgbPins.pins[i]);
  }
  Serial.println();
  Serial.println("Se nao acender, confira jumper de solda do WS2812 em IO48.");

  for (const auto &step : steps) {
    Serial.printf("  -> %s\n", step.name);
    writeRgbAllPins(rgbPins, step.r, step.g, step.b);
    delay(350);
  }
}

void printSystemInfo() {
  Serial.println("=== ESP32-S3 DIAGNOSTICO ===");
  Serial.printf("Chip Model: %s\n", ESP.getChipModel());
  Serial.printf("Chip Cores: %u\n", ESP.getChipCores());
  Serial.printf("Chip Revision: %u\n", ESP.getChipRevision());
  Serial.printf("CPU Freq: %lu MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("Flash Size: %u MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.printf("Free Heap: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("PSRAM Size: %u bytes\n", ESP.getPsramSize());
  Serial.printf("Free PSRAM: %u bytes\n", ESP.getFreePsram());
}

bool psramPatternTest() {
  const size_t psramSize = ESP.getPsramSize();
  if (psramSize == 0) {
    Serial.println("[FAIL] PSRAM nao detectada.");
    return false;
  }

  const size_t testSize = 256 * 1024;
  uint8_t *buffer = static_cast<uint8_t *>(ps_malloc(testSize));
  if (buffer == nullptr) {
    Serial.println("[FAIL] Nao foi possivel alocar na PSRAM.");
    return false;
  }

  for (size_t i = 0; i < testSize; i++) {
    buffer[i] = static_cast<uint8_t>((i ^ 0xA5) & 0xFF);
  }
  for (size_t i = 0; i < testSize; i++) {
    const uint8_t expected = static_cast<uint8_t>((i ^ 0xA5) & 0xFF);
    if (buffer[i] != expected) {
      Serial.printf("[FAIL] PSRAM mismatch em %u\n", static_cast<unsigned>(i));
      free(buffer);
      return false;
    }
  }

  free(buffer);
  Serial.printf("[OK] PSRAM testou %u bytes.\n", static_cast<unsigned>(testSize));
  return true;
}

bool nvsCounterTest() {
  Preferences pref;
  if (!pref.begin("diag", false)) {
    Serial.println("[FAIL] NVS nao abriu.");
    return false;
  }
  const uint32_t boots = pref.getUInt("boots", 0) + 1;
  const bool saved = pref.putUInt("boots", boots) > 0;
  pref.end();
  if (!saved) {
    Serial.println("[FAIL] NVS nao salvou contador.");
    return false;
  }
  Serial.printf("[OK] NVS contador de boots: %u\n", boots);
  return true;
}

bool connectConfiguredWifi() {
  if (strlen(WIFI_SSID) == 0) {
    Serial.println("[INFO] Wi-Fi nao configurado (WIFI_SSID vazio).");
    return false;
  }

  Serial.printf("Conectando em Wi-Fi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const unsigned long timeoutMs = 15000;
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[FAIL] Wi-Fi nao conectou (status=%d)\n", static_cast<int>(WiFi.status()));
    return false;
  }

  Serial.println("[OK] Wi-Fi conectado.");
  Serial.printf("SSID: %s\n", WiFi.SSID().c_str());
  Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
  return true;
}

bool startMdns() {
  if (!WiFi.isConnected()) {
    return false;
  }
  if (!MDNS.begin(DEVICE_HOSTNAME)) {
    Serial.println("[FAIL] mDNS nao iniciou.");
    return false;
  }
  MDNS.addService("http", "tcp", 80);
  Serial.printf("[OK] mDNS ativo em http://%s.local\n", DEVICE_HOSTNAME);
  return true;
}

String buildStateJson() {
  String json = "{";
  json += "\"r\":" + String(gLedColor.r) + ",";
  json += "\"g\":" + String(gLedColor.g) + ",";
  json += "\"b\":" + String(gLedColor.b) + ",";
  json += "\"hex\":\"" + ledHexColor() + "\",";
  json += "\"wifi\":" + String(static_cast<int>(WiFi.status())) + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"matrix_pin\":" + String(MATRIX_DATA_PIN) + ",";
  json += "\"matrix_count\":" + String(kMatrixLedCount) + ",";
  json += "\"matrix_brightness\":" + String(gMatrixBrightness) + ",";
  json += "\"matrix_test\":" + String(gMatrixTestRunning ? 1 : 0);
  json += "}";
  return json;
}

void handleRoot() {
  static const char kHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="pt-BR">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32 RGB + Matriz</title>
  <style>
    :root { --bg:#0b1220; --card:#101827; --text:#e6edf7; --muted:#96a3b8; --line:#223047; }
    * { box-sizing:border-box; }
    body {
      margin:0;
      min-height:100vh;
      font-family:Segoe UI,Arial,sans-serif;
      color:var(--text);
      background:radial-gradient(circle at top left,#16213b,#0a0f1a 70%);
      display:flex;
      align-items:center;
      justify-content:center;
      padding:20px;
    }
    .card {
      width:min(680px,100%);
      background:var(--card);
      border:1px solid var(--line);
      border-radius:16px;
      box-shadow:0 24px 40px rgba(0,0,0,.35);
      padding:20px;
    }
    h1 { margin:0 0 6px; font-size:24px; }
    p { margin:0 0 14px; color:var(--muted); }
    .row { display:flex; flex-wrap:wrap; gap:10px; align-items:center; margin:12px 0; }
    input[type=color] { width:120px; height:50px; border:0; background:none; padding:0; cursor:pointer; }
    button {
      border:1px solid #2b3a55;
      border-radius:10px;
      padding:9px 12px;
      cursor:pointer;
      color:var(--text);
      background:#152238;
    }
    button:hover { background:#1b2d48; }
    .status { margin-top:12px; color:var(--muted); font-size:14px; line-height:1.4; }
    .dot { width:16px; height:16px; border-radius:50%; border:1px solid #30415f; }
    .label { font-size:14px; color:var(--muted); min-width:130px; }
    input[type=range] { width:min(320px,100%); }
  </style>
</head>
<body>
  <main class="card">
    <h1>ESP32 RGB + Matriz 8x8</h1>
    <p>Cor controla o LED onboard e a matriz WS2812.</p>

    <div class="row">
      <input id="picker" type="color" value="#000000">
      <button data-color="#FF0000">Vermelho</button>
      <button data-color="#00FF00">Verde</button>
      <button data-color="#0000FF">Azul</button>
      <button id="off">Desligar</button>
    </div>

    <div class="row">
      <span class="label">Brilho da matriz</span>
      <input id="brightness" type="range" min="0" max="255" value="32">
      <span id="brightnessVal">32</span>
    </div>

    <div class="row">
      <button id="matrixTest">Rodar teste da matriz</button>
      <div class="dot" id="dot"></div>
    </div>

    <div class="status" id="status">Carregando...</div>
  </main>

  <script>
    const picker = document.getElementById('picker');
    const statusEl = document.getElementById('status');
    const dot = document.getElementById('dot');
    const brightness = document.getElementById('brightness');
    const brightnessVal = document.getElementById('brightnessVal');

    let colorTimer = null;
    let brightTimer = null;

    async function fetchState() {
      const res = await fetch('/api/state');
      const st = await res.json();
      picker.value = st.hex;
      dot.style.background = st.hex;
      brightness.value = st.matrix_brightness;
      brightnessVal.textContent = st.matrix_brightness;
      statusEl.textContent =
        `IP: ${st.ip} | WiFi: ${st.wifi} | Cor: ${st.hex} | Matriz: ${st.matrix_count} leds no GPIO ${st.matrix_pin} | Brilho: ${st.matrix_brightness}`;
    }

    async function sendColor(hex) {
      await fetch('/api/led?hex=' + encodeURIComponent(hex));
      fetchState();
    }

    async function setMatrixBrightness(value) {
      await fetch('/api/matrix?brightness=' + encodeURIComponent(value));
      fetchState();
    }

    picker.addEventListener('input', () => {
      clearTimeout(colorTimer);
      colorTimer = setTimeout(() => sendColor(picker.value), 120);
    });

    brightness.addEventListener('input', () => {
      brightnessVal.textContent = brightness.value;
      clearTimeout(brightTimer);
      brightTimer = setTimeout(() => setMatrixBrightness(brightness.value), 120);
    });

    document.querySelectorAll('button[data-color]').forEach(btn => {
      btn.addEventListener('click', () => sendColor(btn.dataset.color));
    });

    document.getElementById('off').addEventListener('click', () => sendColor('#000000'));

    document.getElementById('matrixTest').addEventListener('click', async () => {
      await fetch('/api/matrix?test=1');
      fetchState();
    });

    fetchState();
    setInterval(fetchState, 2000);
  </script>
</body>
</html>
)HTML";
  gWebServer.send_P(200, "text/html; charset=utf-8", kHtml);
}

void handleApiState() {
  gWebServer.send(200, "application/json", buildStateJson());
}

void handleApiLed() {
  RgbColor next = gLedColor;
  bool changed = false;

  if (gWebServer.hasArg("hex")) {
    changed = parseHexColor(gWebServer.arg("hex"), next);
  } else if (gWebServer.hasArg("r") && gWebServer.hasArg("g") && gWebServer.hasArg("b")) {
    next.r = static_cast<uint8_t>(constrain(gWebServer.arg("r").toInt(), 0, 255));
    next.g = static_cast<uint8_t>(constrain(gWebServer.arg("g").toInt(), 0, 255));
    next.b = static_cast<uint8_t>(constrain(gWebServer.arg("b").toInt(), 0, 255));
    changed = true;
  }

  if (!changed) {
    gWebServer.send(400, "application/json", "{\"error\":\"invalid_color\"}");
    return;
  }

  setLedColor(next.r, next.g, next.b);
  saveSettings();
  gWebServer.send(200, "application/json", buildStateJson());
}

void handleApiMatrix() {
  bool changed = false;

  if (gWebServer.hasArg("brightness")) {
    const int br = constrain(gWebServer.arg("brightness").toInt(), 0, 255);
    setMatrixBrightness(static_cast<uint8_t>(br));
    changed = true;
  }

  if (gWebServer.hasArg("test")) {
    if (gWebServer.arg("test") != "0") {
      startMatrixTest();
    }
    changed = true;
  }

  if (gWebServer.hasArg("hex")) {
    RgbColor next = gLedColor;
    if (!parseHexColor(gWebServer.arg("hex"), next)) {
      gWebServer.send(400, "application/json", "{\"error\":\"invalid_color\"}");
      return;
    }
    setLedColor(next.r, next.g, next.b);
    changed = true;
  }

  if (!changed) {
    gWebServer.send(400, "application/json", "{\"error\":\"invalid_params\"}");
    return;
  }

  if (gWebServer.hasArg("brightness") || gWebServer.hasArg("hex")) {
    saveSettings();
  }

  gWebServer.send(200, "application/json", buildStateJson());
}

bool startWebServer() {
  if (!WiFi.isConnected()) {
    return false;
  }

  gWebServer.on("/", HTTP_GET, handleRoot);
  gWebServer.on("/api/state", HTTP_GET, handleApiState);
  gWebServer.on("/api/led", HTTP_GET, handleApiLed);
  gWebServer.on("/api/matrix", HTTP_GET, handleApiMatrix);
  gWebServer.onNotFound([]() {
    gWebServer.send(404, "text/plain", "Not found");
  });
  gWebServer.begin();

  Serial.println("[OK] Web server iniciado.");
  Serial.printf("Acesse: http://%s.local ou http://%s\n",
                DEVICE_HOSTNAME,
                WiFi.localIP().toString().c_str());
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(800);

#ifdef LED_BUILTIN
  pinMode(kLedPin, OUTPUT);
#endif

  Serial.println();
  printSystemInfo();
  rgbTest();
  psramPatternTest();
  nvsCounterTest();

  initMatrix();
  loadSettings();

  if (connectConfiguredWifi()) {
    gMdnsStarted = startMdns();
    (void)gMdnsStarted;
    gWebServerStarted = startWebServer();
  }

  Serial.println("=== FIM DIAGNOSTICO ===");
}

void loop() {
  if (gWebServerStarted) {
    gWebServer.handleClient();
  }

  tickMatrixTest();

  static unsigned long lastPrint = 0;
  const unsigned long now = millis();
  if (now - lastPrint >= 3000) {
    lastPrint = now;
    Serial.printf(
      "Heartbeat | uptime=%lu ms | heap=%u | psram_free=%u | wifi=%d | ip=%s | led=%s | matrix_br=%u | matrix_test=%d\n",
      now,
      ESP.getFreeHeap(),
      ESP.getFreePsram(),
      static_cast<int>(WiFi.status()),
      WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "0.0.0.0",
      ledHexColor().c_str(),
      gMatrixBrightness,
      gMatrixTestRunning ? 1 : 0);
  }

  delay(10);
}
