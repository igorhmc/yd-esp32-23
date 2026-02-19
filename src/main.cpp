#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <ctype.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
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

#ifndef MATRIX_SERPENTINE
#define MATRIX_SERPENTINE 1
#endif

#ifndef MATRIX_X_FLIP
#define MATRIX_X_FLIP 1
#endif

#ifndef MATRIX_Y_FLIP
#define MATRIX_Y_FLIP 0
#endif

#ifndef MATRIX_MAX_LEDS
#define MATRIX_MAX_LEDS (MATRIX_WIDTH * MATRIX_HEIGHT)
#endif

#ifndef MATRIX_BRIGHTNESS_DEFAULT
#define MATRIX_BRIGHTNESS_DEFAULT 32
#endif

static const uint16_t kMatrixLedCount = MATRIX_MAX_LEDS;

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

enum class ScrollDirection : uint8_t {
  Left = 0,
  Right = 1,
};

WebServer gWebServer(80);
Adafruit_NeoPixel gMatrix(kMatrixLedCount, MATRIX_DATA_PIN, NEO_GRB + NEO_KHZ800);

RgbColor gLedColor = {0, 0, 0};
int gMatrixDataPin = MATRIX_DATA_PIN;
uint16_t gMatrixActiveLedCount = kMatrixLedCount;
uint16_t gMatrixRuntimeMaxLedCount = kMatrixLedCount;
uint8_t gMatrixBrightness = MATRIX_BRIGHTNESS_DEFAULT;
bool gMatrixReady = false;
bool gMatrixTestRunning = false;
uint16_t gMatrixTestIndex = 0;
unsigned long gMatrixLastStepMs = 0;
bool gMatrixScrollRunning = false;
String gMatrixScrollText = "HELLO";
int16_t gMatrixScrollOffsetX = MATRIX_WIDTH;
unsigned long gMatrixScrollLastStepMs = 0;
uint16_t gMatrixScrollStepMs = 120;
ScrollDirection gMatrixScrollDirection = ScrollDirection::Left;

static const uint8_t kScrollFontHeight = 6;
static const uint8_t kScrollGlyphWidth = 5;
static const uint8_t kScrollGlyphSpacing = 1;
static const size_t kScrollTextMaxLength = 64;
uint32_t gMatrixScrollCharColors[kScrollTextMaxLength] = {0};
bool gMatrixScrollUseCharColors = false;

bool gMdnsStarted = false;
bool gWebServerStarted = false;
bool gApMode = false;
bool gOtaHasError = false;
String gApSsid;
String gApPassword;

void renderMatrixScrollFrame();

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
  gMatrix.clear();
  const uint32_t packed = gMatrix.Color(color.r, color.g, color.b);
  for (uint16_t i = 0; i < gMatrixActiveLedCount; i++) {
    gMatrix.setPixelColor(i, packed);
  }
  gMatrix.show();
}

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
  gLedColor = {r, g, b};
  gMatrixTestRunning = false;
  applyBoardLedColor(gLedColor);
  if (gMatrixScrollRunning) {
    renderMatrixScrollFrame();
  } else {
    applyMatrixSolidColor(gLedColor);
  }
}

void setMatrixBrightness(uint8_t value) {
  gMatrixBrightness = value;
  if (gMatrixReady && !gMatrixTestRunning) {
    if (gMatrixScrollRunning) {
      renderMatrixScrollFrame();
    } else {
      applyMatrixSolidColor(gLedColor);
    }
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

const char *scrollDirectionToString(ScrollDirection direction) {
  return direction == ScrollDirection::Right ? "right" : "left";
}

bool parseScrollDirection(const String &value, ScrollDirection &out) {
  String dir = value;
  dir.trim();
  dir.toLowerCase();

  if (dir == "left" || dir == "rtl" || dir == "right_to_left") {
    out = ScrollDirection::Left;
    return true;
  }
  if (dir == "right" || dir == "ltr" || dir == "left_to_right") {
    out = ScrollDirection::Right;
    return true;
  }

  return false;
}

bool isValidMatrixPin(int pin) {
  return pin >= 0 && pin < static_cast<int>(SOC_GPIO_PIN_COUNT);
}

bool isValidMatrixLedCount(int count) {
  return count > 0 && count <= static_cast<int>(gMatrixRuntimeMaxLedCount);
}

uint16_t detectRuntimeMaxLedCount() {
  // WS2812 consome ~3 bytes por LED no buffer do NeoPixel.
  // Reservamos heap para Wi-Fi/WebServer e calculamos um teto seguro em runtime.
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t reservedHeap = 32 * 1024;

  if (freeHeap <= reservedHeap) {
    return 1;
  }

  uint32_t byHeap = (freeHeap - reservedHeap) / 3;
  if (byHeap < 1) {
    byHeap = 1;
  }
  if (byHeap > kMatrixLedCount) {
    byHeap = kMatrixLedCount;
  }
  return static_cast<uint16_t>(byHeap);
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
  pref.putInt("mpin", gMatrixDataPin);
  pref.putUShort("mcount", gMatrixActiveLedCount);
  pref.putUChar("msdir", static_cast<uint8_t>(gMatrixScrollDirection));
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
  const int pin = pref.getInt("mpin", MATRIX_DATA_PIN);
  const uint16_t count = pref.getUShort("mcount", kMatrixLedCount);
  const uint8_t scrollDirRaw = pref.getUChar("msdir", static_cast<uint8_t>(ScrollDirection::Left));
  pref.end();

  gMatrixDataPin = isValidMatrixPin(pin) ? pin : MATRIX_DATA_PIN;
  gMatrixActiveLedCount = isValidMatrixLedCount(count) ? count : gMatrixRuntimeMaxLedCount;
  gMatrixBrightness = br;
  gMatrixScrollDirection = (scrollDirRaw == static_cast<uint8_t>(ScrollDirection::Right))
                             ? ScrollDirection::Right
                             : ScrollDirection::Left;
  setLedColor(r, g, b);
}

String jsonEscape(const String &value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    const char c = value.charAt(i);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

void loadWifiSettings(String &ssid, String &password) {
  ssid = "";
  password = "";

  Preferences pref;
  if (!pref.begin("wifi", true)) {
    return;
  }
  ssid = pref.getString("ssid", "");
  password = pref.getString("pass", "");
  pref.end();
}

void saveWifiSettings(const String &ssid, const String &password) {
  Preferences pref;
  if (!pref.begin("wifi", false)) {
    return;
  }
  pref.putString("ssid", ssid);
  pref.putString("pass", password);
  pref.end();
}

void clearWifiSettings() {
  Preferences pref;
  if (!pref.begin("wifi", false)) {
    return;
  }
  pref.remove("ssid");
  pref.remove("pass");
  pref.end();
}

bool connectWifiWithCredentials(const String &ssid, const String &password, unsigned long timeoutMs = 15000) {
  if (ssid.length() == 0) {
    return false;
  }

  Serial.printf("Conectando em Wi-Fi: %s\n", ssid.c_str());
  WiFi.mode(gApMode ? WIFI_AP_STA : WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(ssid.c_str(), password.c_str());

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

bool connectConfiguredWifi() {
  String storedSsid;
  String storedPassword;
  loadWifiSettings(storedSsid, storedPassword);

  if (storedSsid.length() > 0) {
    return connectWifiWithCredentials(storedSsid, storedPassword);
  }

  if (strlen(WIFI_SSID) == 0) {
    Serial.println("[INFO] Wi-Fi nao configurado (sem credenciais salvas e WIFI_SSID vazio).");
    return false;
  }

  return connectWifiWithCredentials(String(WIFI_SSID), String(WIFI_PASSWORD));
}

bool startConfigAp() {
  if (gApMode) {
    return true;
  }

  gApSsid = String(DEVICE_HOSTNAME) + "-setup";
  gApPassword = "12345678";

  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(gApSsid.c_str(), gApPassword.c_str())) {
    Serial.println("[FAIL] Nao foi possivel iniciar AP de configuracao.");
    return false;
  }

  gApMode = true;
  Serial.printf("[OK] AP ativo: %s | senha: %s | IP: %s\n",
                gApSsid.c_str(),
                gApPassword.c_str(),
                WiFi.softAPIP().toString().c_str());
  return true;
}

void stopConfigAp() {
  if (!gApMode) {
    return;
  }
  WiFi.softAPdisconnect(true);
  gApMode = false;
  Serial.println("[OK] AP de configuracao desligado.");
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

bool mapMatrixXY(uint8_t x, uint8_t y, uint16_t &index) {
  if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) {
    return false;
  }

  const uint8_t mappedX = MATRIX_X_FLIP ? (MATRIX_WIDTH - 1 - x) : x;
  const uint8_t mappedY = MATRIX_Y_FLIP ? (MATRIX_HEIGHT - 1 - y) : y;

  const uint16_t rowBase = static_cast<uint16_t>(mappedY) * MATRIX_WIDTH;
  if (MATRIX_SERPENTINE && ((mappedY & 0x01) != 0)) {
    index = rowBase + (MATRIX_WIDTH - 1 - mappedX);
  } else {
    index = rowBase + mappedX;
  }

  return index < gMatrixActiveLedCount;
}

void setMatrixPixel(uint8_t x, uint8_t y, uint32_t color) {
  uint16_t index = 0;
  if (mapMatrixXY(x, y, index)) {
    gMatrix.setPixelColor(index, color);
  }
}

bool loadGlyphRows(char c, uint8_t rows[kScrollFontHeight]) {
  memset(rows, 0, kScrollFontHeight);

  // 5x6 lowercase glyphs.
  switch (c) {
    case 'a': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'b': {
      const uint8_t data[kScrollFontHeight] = {0x10, 0x10, 0x1E, 0x11, 0x11, 0x1E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'c': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x0E, 0x11, 0x10, 0x11, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'd': {
      const uint8_t data[kScrollFontHeight] = {0x01, 0x01, 0x0F, 0x11, 0x11, 0x0F};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'e': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'f': {
      const uint8_t data[kScrollFontHeight] = {0x06, 0x08, 0x1E, 0x08, 0x08, 0x08};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'g': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x0F, 0x11, 0x0F, 0x01, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'h': {
      const uint8_t data[kScrollFontHeight] = {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'i': {
      const uint8_t data[kScrollFontHeight] = {0x04, 0x00, 0x0C, 0x04, 0x04, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'j': {
      const uint8_t data[kScrollFontHeight] = {0x02, 0x00, 0x02, 0x02, 0x12, 0x0C};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'k': {
      const uint8_t data[kScrollFontHeight] = {0x10, 0x12, 0x14, 0x18, 0x14, 0x12};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'l': {
      const uint8_t data[kScrollFontHeight] = {0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'm': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x1A, 0x15, 0x15, 0x15, 0x15};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'n': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x1E, 0x11, 0x11, 0x11, 0x11};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'o': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'p': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x1E, 0x11, 0x1E, 0x10, 0x10};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'q': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x0F, 0x11, 0x0F, 0x01, 0x01};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'r': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x16, 0x19, 0x10, 0x10, 0x10};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 's': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 't': {
      const uint8_t data[kScrollFontHeight] = {0x08, 0x1E, 0x08, 0x08, 0x08, 0x06};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'u': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x11, 0x11, 0x11, 0x13, 0x0D};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'v': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x11, 0x11, 0x11, 0x0A, 0x04};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'w': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x11, 0x11, 0x15, 0x15, 0x0A};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'x': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'y': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x11, 0x11, 0x0F, 0x01, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'z': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    default:
      break;
  }

  const char up = static_cast<char>(toupper(static_cast<unsigned char>(c)));

  switch (up) {
    case 'A': {
      const uint8_t data[kScrollFontHeight] = {0x0E, 0x11, 0x1F, 0x11, 0x11, 0x11};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'B': {
      const uint8_t data[kScrollFontHeight] = {0x1E, 0x11, 0x1E, 0x11, 0x11, 0x1E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'C': {
      const uint8_t data[kScrollFontHeight] = {0x0E, 0x11, 0x10, 0x10, 0x11, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'D': {
      const uint8_t data[kScrollFontHeight] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x1E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'E': {
      const uint8_t data[kScrollFontHeight] = {0x1F, 0x10, 0x1E, 0x10, 0x10, 0x1F};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'F': {
      const uint8_t data[kScrollFontHeight] = {0x1F, 0x10, 0x1E, 0x10, 0x10, 0x10};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'G': {
      const uint8_t data[kScrollFontHeight] = {0x0E, 0x11, 0x10, 0x13, 0x11, 0x0F};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'H': {
      const uint8_t data[kScrollFontHeight] = {0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'I': {
      const uint8_t data[kScrollFontHeight] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x1F};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'J': {
      const uint8_t data[kScrollFontHeight] = {0x01, 0x01, 0x01, 0x11, 0x11, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'K': {
      const uint8_t data[kScrollFontHeight] = {0x11, 0x12, 0x1C, 0x12, 0x11, 0x11};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'L': {
      const uint8_t data[kScrollFontHeight] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'M': {
      const uint8_t data[kScrollFontHeight] = {0x11, 0x1B, 0x15, 0x11, 0x11, 0x11};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'N': {
      const uint8_t data[kScrollFontHeight] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'O': {
      const uint8_t data[kScrollFontHeight] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'P': {
      const uint8_t data[kScrollFontHeight] = {0x1E, 0x11, 0x1E, 0x10, 0x10, 0x10};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'Q': {
      const uint8_t data[kScrollFontHeight] = {0x0E, 0x11, 0x11, 0x15, 0x12, 0x0D};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'R': {
      const uint8_t data[kScrollFontHeight] = {0x1E, 0x11, 0x1E, 0x12, 0x11, 0x11};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'S': {
      const uint8_t data[kScrollFontHeight] = {0x0F, 0x10, 0x0E, 0x01, 0x11, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'T': {
      const uint8_t data[kScrollFontHeight] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'U': {
      const uint8_t data[kScrollFontHeight] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'V': {
      const uint8_t data[kScrollFontHeight] = {0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'W': {
      const uint8_t data[kScrollFontHeight] = {0x11, 0x11, 0x11, 0x15, 0x1B, 0x11};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'X': {
      const uint8_t data[kScrollFontHeight] = {0x11, 0x0A, 0x04, 0x04, 0x0A, 0x11};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'Y': {
      const uint8_t data[kScrollFontHeight] = {0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case 'Z': {
      const uint8_t data[kScrollFontHeight] = {0x1F, 0x02, 0x04, 0x08, 0x10, 0x1F};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case '0': {
      const uint8_t data[kScrollFontHeight] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case '1': {
      const uint8_t data[kScrollFontHeight] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case '2': {
      const uint8_t data[kScrollFontHeight] = {0x0E, 0x11, 0x01, 0x06, 0x08, 0x1F};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case '3': {
      const uint8_t data[kScrollFontHeight] = {0x1E, 0x01, 0x06, 0x01, 0x11, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case '4': {
      const uint8_t data[kScrollFontHeight] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case '5': {
      const uint8_t data[kScrollFontHeight] = {0x1F, 0x10, 0x1E, 0x01, 0x11, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case '6': {
      const uint8_t data[kScrollFontHeight] = {0x07, 0x08, 0x1E, 0x11, 0x11, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case '7': {
      const uint8_t data[kScrollFontHeight] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case '8': {
      const uint8_t data[kScrollFontHeight] = {0x0E, 0x11, 0x0E, 0x11, 0x11, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case '9': {
      const uint8_t data[kScrollFontHeight] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x0E};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case '!': {
      const uint8_t data[kScrollFontHeight] = {0x04, 0x04, 0x04, 0x04, 0x00, 0x04};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case '?': {
      const uint8_t data[kScrollFontHeight] = {0x0E, 0x11, 0x02, 0x04, 0x00, 0x04};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case '.': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x04};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case ':': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x04, 0x00, 0x00, 0x04, 0x00};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case '-': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case '_': {
      const uint8_t data[kScrollFontHeight] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x1F};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case '/': {
      const uint8_t data[kScrollFontHeight] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x00};
      memcpy(rows, data, kScrollFontHeight);
      return true;
    }
    case ' ': {
      return true;
    }
    default: {
      const uint8_t data[kScrollFontHeight] = {0x0E, 0x11, 0x02, 0x04, 0x00, 0x04};
      memcpy(rows, data, kScrollFontHeight);
      return false;
    }
  }
}

void drawGlyphAt(int16_t x, int16_t y, char c, uint32_t color) {
  uint8_t rows[kScrollFontHeight] = {0};
  loadGlyphRows(c, rows);

  for (uint8_t row = 0; row < kScrollFontHeight; row++) {
    const uint8_t rowBits = rows[row];
    for (uint8_t col = 0; col < kScrollGlyphWidth; col++) {
      if ((rowBits & (1 << (kScrollGlyphWidth - 1 - col))) == 0) {
        continue;
      }

      const int16_t px = x + col;
      const int16_t py = y + row;
      if (px < 0 || py < 0 || px >= MATRIX_WIDTH || py >= MATRIX_HEIGHT) {
        continue;
      }

      setMatrixPixel(static_cast<uint8_t>(px), static_cast<uint8_t>(py), color);
    }
  }
}

int16_t scrollTextPixelWidth(const String &text) {
  if (text.length() == 0) {
    return 0;
  }

  return static_cast<int16_t>(text.length() * (kScrollGlyphWidth + kScrollGlyphSpacing));
}

int16_t scrollStartOffsetX(ScrollDirection direction, const String &text) {
  if (direction == ScrollDirection::Right) {
    return -scrollTextPixelWidth(text);
  }
  return MATRIX_WIDTH;
}

bool buildMulticolorScrollText(const String &payload, String &outText) {
  outText = "";
  memset(gMatrixScrollCharColors, 0, sizeof(gMatrixScrollCharColors));

  bool hasAny = false;
  int start = 0;
  while (start <= static_cast<int>(payload.length())) {
    const int separator = payload.indexOf('|', start);
    String item = separator < 0 ? payload.substring(start) : payload.substring(start, separator);
    item.trim();

    if (item.length() > 0) {
      const int colon = item.indexOf(':');
      if (colon <= 0) {
        return false;
      }

      String colorText = item.substring(0, colon);
      colorText.trim();
      String segmentText = item.substring(colon + 1);

      if (segmentText.length() > 0) {
        RgbColor segmentColor = {0, 0, 0};
        if (!parseHexColor(colorText, segmentColor)) {
          return false;
        }

        const uint32_t packed = gMatrix.Color(segmentColor.r, segmentColor.g, segmentColor.b);
        for (size_t i = 0; i < segmentText.length(); i++) {
          if (outText.length() >= kScrollTextMaxLength) {
            break;
          }
          gMatrixScrollCharColors[outText.length()] = packed;
          outText += segmentText.charAt(i);
        }

        hasAny = outText.length() > 0;
      }
    }

    if (separator < 0) {
      break;
    }
    start = separator + 1;
  }

  return hasAny;
}

void renderMatrixScrollFrame() {
  if (!gMatrixReady || !gMatrixScrollRunning) {
    return;
  }

  gMatrix.setBrightness(gMatrixBrightness);
  gMatrix.clear();

  const int16_t yOffset = MATRIX_HEIGHT > kScrollFontHeight ? (MATRIX_HEIGHT - kScrollFontHeight) / 2 : 0;
  const uint32_t color = gMatrix.Color(gLedColor.r, gLedColor.g, gLedColor.b);

  for (size_t i = 0; i < gMatrixScrollText.length(); i++) {
    const int16_t x = gMatrixScrollOffsetX + static_cast<int16_t>(i * (kScrollGlyphWidth + kScrollGlyphSpacing));
    if (x > (MATRIX_WIDTH - 1) || (x + kScrollGlyphWidth) < 0) {
      continue;
    }
    const uint32_t glyphColor =
      (gMatrixScrollUseCharColors && i < kScrollTextMaxLength) ? gMatrixScrollCharColors[i] : color;
    drawGlyphAt(x, yOffset, gMatrixScrollText.charAt(i), glyphColor);
  }

  gMatrix.show();
}

bool startMatrixScrollCore(String text, uint16_t speedMs) {
  if (!gMatrixReady || gMatrixActiveLedCount == 0) {
    return false;
  }

  text.replace("\r", "");
  text.replace("\n", " ");
  if (text.length() == 0) {
    return false;
  }

  if (text.length() > kScrollTextMaxLength) {
    text.remove(kScrollTextMaxLength);
  }

  gMatrixScrollText = text;
  gMatrixScrollStepMs = static_cast<uint16_t>(constrain(static_cast<int>(speedMs), 40, 1000));
  gMatrixScrollOffsetX = scrollStartOffsetX(gMatrixScrollDirection, gMatrixScrollText);
  gMatrixScrollLastStepMs = millis();
  gMatrixScrollRunning = true;
  gMatrixTestRunning = false;
  renderMatrixScrollFrame();
  Serial.printf("[OK] Scroll text started: \"%s\" | speed=%u ms | dir=%s\n",
                gMatrixScrollText.c_str(),
                gMatrixScrollStepMs,
                scrollDirectionToString(gMatrixScrollDirection));
  return true;
}

bool startMatrixScroll(String text, uint16_t speedMs) {
  gMatrixScrollUseCharColors = false;
  return startMatrixScrollCore(text, speedMs);
}

bool startMatrixScrollSegments(String payload, uint16_t speedMs) {
  String multicolorText;
  if (!buildMulticolorScrollText(payload, multicolorText)) {
    return false;
  }
  gMatrixScrollUseCharColors = true;
  return startMatrixScrollCore(multicolorText, speedMs);
}

void stopMatrixScroll() {
  if (!gMatrixScrollRunning) {
    return;
  }
  gMatrixScrollRunning = false;
  applyMatrixSolidColor(gLedColor);
  Serial.println("[OK] Scroll text stopped.");
}

void tickMatrixScroll() {
  if (!gMatrixReady || !gMatrixScrollRunning) {
    return;
  }

  const unsigned long now = millis();
  if ((now - gMatrixScrollLastStepMs) < gMatrixScrollStepMs) {
    return;
  }
  gMatrixScrollLastStepMs = now;

  const int16_t textWidth = scrollTextPixelWidth(gMatrixScrollText);
  if (gMatrixScrollDirection == ScrollDirection::Right) {
    gMatrixScrollOffsetX++;
    if (gMatrixScrollOffsetX > MATRIX_WIDTH) {
      gMatrixScrollOffsetX = -textWidth;
    }
  } else {
    gMatrixScrollOffsetX--;
    if (gMatrixScrollOffsetX < -textWidth) {
      gMatrixScrollOffsetX = MATRIX_WIDTH;
    }
  }

  renderMatrixScrollFrame();
}

void startMatrixTest() {
  if (!gMatrixReady || gMatrixActiveLedCount == 0) {
    return;
  }
  gMatrixScrollRunning = false;
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
  uint8_t wheel = 0;
  if (gMatrixActiveLedCount > 1) {
    wheel = static_cast<uint8_t>((gMatrixTestIndex * 255) / (gMatrixActiveLedCount - 1));
  }
  gMatrix.setPixelColor(gMatrixTestIndex, colorWheel(wheel));
  gMatrix.show();

  gMatrixTestIndex++;
  if (gMatrixTestIndex >= gMatrixActiveLedCount) {
    gMatrixTestRunning = false;
    applyMatrixSolidColor(gLedColor);
    Serial.println("[OK] Teste da matriz concluido.");
  }
}

void initMatrix() {
  gMatrix.setPin(static_cast<int16_t>(gMatrixDataPin));
  gMatrix.begin();
  gMatrixReady = true;
  gMatrix.setBrightness(gMatrixBrightness);
  gMatrix.clear();
  gMatrix.show();
  Serial.printf("[OK] Matriz WS2812 pronta | pin=%d | leds=%u | brilho=%u\n",
                gMatrixDataPin,
                static_cast<unsigned>(gMatrixActiveLedCount),
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
  json += "\"sta_connected\":" + String(WiFi.isConnected() ? 1 : 0) + ",";
  json += "\"wifi_ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",";
  json += "\"hostname\":\"" + String(DEVICE_HOSTNAME) + "\",";
  json += "\"ap_mode\":" + String(gApMode ? 1 : 0) + ",";
  json += "\"ap_ssid\":\"" + jsonEscape(gApSsid) + "\",";
  json += "\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"matrix_pin\":" + String(gMatrixDataPin) + ",";
  json += "\"matrix_count\":" + String(gMatrixActiveLedCount) + ",";
  json += "\"matrix_max_count\":" + String(gMatrixRuntimeMaxLedCount) + ",";
  json += "\"matrix_brightness\":" + String(gMatrixBrightness) + ",";
  json += "\"matrix_test\":" + String(gMatrixTestRunning ? 1 : 0) + ",";
  json += "\"matrix_scroll\":" + String(gMatrixScrollRunning ? 1 : 0) + ",";
  json += "\"matrix_scroll_speed\":" + String(gMatrixScrollStepMs) + ",";
  json += "\"matrix_scroll_multicolor\":" + String(gMatrixScrollUseCharColors ? 1 : 0) + ",";
  json += "\"matrix_scroll_direction\":\"" + String(scrollDirectionToString(gMatrixScrollDirection)) + "\",";
  json += "\"matrix_scroll_text\":\"" + jsonEscape(gMatrixScrollText) + "\"";
  json += "}";
  return json;
}

void handleRoot() {
  static const char kHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32 RGB + Matrix</title>
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
    <h1>ESP32 RGB + WS2812 Matrix</h1>
    <p>Color controls both the onboard LED and the WS2812 matrix.</p>

    <div class="row">
      <input id="picker" type="color" value="#000000">
      <button data-color="#FF0000">Red</button>
      <button data-color="#00FF00">Green</button>
      <button data-color="#0000FF">Blue</button>
      <button id="off">Off</button>
    </div>

    <div class="row">
      <span class="label">Matrix brightness</span>
      <input id="brightness" type="range" min="0" max="255" value="32">
      <span id="brightnessVal">32</span>
    </div>

    <div class="row">
      <span class="label">Matrix GPIO</span>
      <input id="matrixPin" type="number" min="0" max="48" value="17" style="width:110px;padding:8px;border-radius:8px;border:1px solid #2b3a55;background:#0f1729;color:#e6edf7;">
      <button id="matrixPinApply">Apply GPIO</button>
    </div>

    <div class="row">
      <span class="label">LED count</span>
      <input id="matrixCount" type="number" min="1" max="256" value="256" style="width:110px;padding:8px;border-radius:8px;border:1px solid #2b3a55;background:#0f1729;color:#e6edf7;">
      <button id="matrixCountApply">Apply LEDs</button>
    </div>

    <div class="row">
      <button id="matrixTest">Run matrix test</button>
      <div class="dot" id="dot"></div>
    </div>

    <h3>8x8 Text Scroll</h3>
    <div class="row">
      <span class="label">Message</span>
      <input id="matrixText" type="text" maxlength="64" placeholder="Type your message" style="flex:1;min-width:220px;padding:8px;border-radius:8px;border:1px solid #2b3a55;background:#0f1729;color:#e6edf7;">
    </div>
    <div class="row">
      <span class="label">Color mode</span>
      <select id="matrixScrollMode" style="padding:8px;border-radius:8px;border:1px solid #2b3a55;background:#0f1729;color:#e6edf7;">
        <option value="single">Single color</option>
        <option value="multi">Color per word/phrase</option>
      </select>
    </div>
    <div id="segmentsPanel" style="display:none;border:1px solid #2b3a55;border-radius:10px;padding:10px;margin:10px 0;">
      <div id="segmentsList"></div>
      <div class="row" style="margin-top:8px;">
        <button id="addSegment" type="button">Add word/phrase color</button>
      </div>
    </div>
    <div class="row">
      <span class="label">Speed (ms)</span>
      <input id="matrixScrollSpeed" type="range" min="40" max="1000" value="120">
      <span id="matrixScrollSpeedVal">120</span>
    </div>
    <div class="row">
      <span class="label">Direction</span>
      <select id="matrixScrollDirection" style="padding:8px;border-radius:8px;border:1px solid #2b3a55;background:#0f1729;color:#e6edf7;">
        <option value="left">Right to left</option>
        <option value="right">Left to right</option>
      </select>
    </div>
    <div class="row">
      <button id="matrixTextStart">Start scroll</button>
      <button id="matrixTextStop">Stop scroll</button>
    </div>

    <h3>Wi-Fi</h3>
    <div class="row">
      <span class="label">SSID</span>
      <input id="wifiSsid" type="text" placeholder="Network name" style="flex:1;min-width:220px;padding:8px;border-radius:8px;border:1px solid #2b3a55;background:#0f1729;color:#e6edf7;">
    </div>
    <div class="row">
      <span class="label">Password</span>
      <input id="wifiPass" type="password" placeholder="Network password" style="flex:1;min-width:220px;padding:8px;border-radius:8px;border:1px solid #2b3a55;background:#0f1729;color:#e6edf7;">
    </div>
    <div class="row">
      <button id="wifiSave">Save + Connect</button>
      <button id="wifiForget">Forget Wi-Fi</button>
    </div>

    <h3>OTA (firmware update)</h3>
    <div class="row">
      <input id="otaFile" type="file" accept=".bin,application/octet-stream" style="flex:1;min-width:220px;">
      <button id="otaUpload">Upload firmware</button>
    </div>

    <div class="status" id="status">Loading...</div>
  </main>

  <script>
    const picker = document.getElementById('picker');
    const statusEl = document.getElementById('status');
    const dot = document.getElementById('dot');
    const brightness = document.getElementById('brightness');
    const brightnessVal = document.getElementById('brightnessVal');
    const matrixPin = document.getElementById('matrixPin');
    const matrixCount = document.getElementById('matrixCount');
    const matrixText = document.getElementById('matrixText');
    const matrixScrollMode = document.getElementById('matrixScrollMode');
    const matrixScrollSpeed = document.getElementById('matrixScrollSpeed');
    const matrixScrollSpeedVal = document.getElementById('matrixScrollSpeedVal');
    const matrixScrollDirection = document.getElementById('matrixScrollDirection');
    const segmentsPanel = document.getElementById('segmentsPanel');
    const segmentsList = document.getElementById('segmentsList');
    const addSegment = document.getElementById('addSegment');
    const wifiSsid = document.getElementById('wifiSsid');
    const wifiPass = document.getElementById('wifiPass');

    let colorTimer = null;
    let brightTimer = null;
    let scrollTimer = null;
    let uiInitialized = false;

    function toggleScrollMode() {
      segmentsPanel.style.display = matrixScrollMode.value === 'multi' ? 'block' : 'none';
    }

    function addSegmentRow(text = '', color = '#FF0000') {
      const row = document.createElement('div');
      row.className = 'row';
      row.dataset.segmentRow = '1';
      row.innerHTML =
        `<input class="segText" type="text" maxlength="24" placeholder="Word or phrase" value="${text.replace(/"/g, '&quot;')}" style="flex:1;min-width:160px;padding:8px;border-radius:8px;border:1px solid #2b3a55;background:#0f1729;color:#e6edf7;">` +
        `<input class="segColor" type="color" value="${color}" style="width:56px;height:38px;border:0;background:none;padding:0;cursor:pointer;">` +
        `<button class="segRemove" type="button">Remove</button>`;

      row.querySelector('.segRemove').addEventListener('click', () => {
        row.remove();
      });

      segmentsList.appendChild(row);
    }

    function collectSegmentsPayload() {
      const rows = Array.from(segmentsList.querySelectorAll('[data-segment-row]'));
      const parts = [];

      rows.forEach(row => {
        const textEl = row.querySelector('.segText');
        const colorEl = row.querySelector('.segColor');
        const text = (textEl.value || '').replace(/[|:]/g, ' ');
        const color = colorEl.value || '#FFFFFF';
        if (text.length > 0) {
          parts.push(`${color}:${text}`);
        }
      });

      return parts.join('|');
    }

    async function fetchState() {
      const res = await fetch('/api/state');
      const st = await res.json();
      picker.value = st.hex;
      dot.style.background = st.hex;
      brightness.value = st.matrix_brightness;
      brightnessVal.textContent = st.matrix_brightness;
      matrixPin.value = st.matrix_pin;
      matrixCount.max = st.matrix_max_count;
      matrixCount.value = st.matrix_count;
      if (document.activeElement !== matrixText) {
        matrixText.value = st.matrix_scroll_text || '';
      }
      if (!uiInitialized) {
        matrixScrollMode.value = st.matrix_scroll_multicolor ? 'multi' : 'single';
        if (matrixScrollMode.value === 'multi' && segmentsList.children.length === 0) {
          addSegmentRow('', '#FF0000');
        }
        toggleScrollMode();
        uiInitialized = true;
      }
      matrixScrollSpeed.value = st.matrix_scroll_speed;
      matrixScrollSpeedVal.textContent = st.matrix_scroll_speed;
      matrixScrollDirection.value = st.matrix_scroll_direction || 'left';
      wifiSsid.value = st.wifi_ssid || '';
      statusEl.textContent =
        `STA IP: ${st.ip} | Wi-Fi: ${st.wifi} | AP: ${st.ap_mode ? (st.ap_ssid + ' @ ' + st.ap_ip) : 'off'} | DNS: http://${st.hostname}.local | Color: ${st.hex} | Matrix: ${st.matrix_count}/${st.matrix_max_count} LEDs on GPIO ${st.matrix_pin} | Brightness: ${st.matrix_brightness} | Scroll: ${st.matrix_scroll ? ('on "' + (st.matrix_scroll_text || '') + '" @ ' + st.matrix_scroll_speed + ' ms / ' + (st.matrix_scroll_direction || 'left') + (st.matrix_scroll_multicolor ? ' / multicolor' : ' / single')) : 'off'}`;
    }

    async function sendColor(hex) {
      await fetch('/api/led?hex=' + encodeURIComponent(hex));
      fetchState();
    }

    async function setMatrixBrightness(value) {
      await fetch('/api/matrix?brightness=' + encodeURIComponent(value));
      fetchState();
    }

    async function setMatrixPin(value) {
      const pin = parseInt(value, 10);
      if (Number.isNaN(pin)) {
        alert('Invalid GPIO');
        return;
      }
      const res = await fetch('/api/matrix?pin=' + encodeURIComponent(pin));
      const data = await res.json();
      if (!res.ok) {
        alert(data.error || 'Failed to apply GPIO');
      }
      fetchState();
    }

    async function setMatrixCount(value) {
      const count = parseInt(value, 10);
      if (Number.isNaN(count) || count < 1) {
        alert('Invalid LED count');
        return;
      }
      const res = await fetch('/api/matrix?count=' + encodeURIComponent(count));
      const data = await res.json();
      if (!res.ok) {
        alert(data.error || 'Failed to apply LED count');
      }
      fetchState();
    }

    async function setScrollSpeed(value) {
      const speed = parseInt(value, 10);
      if (Number.isNaN(speed)) {
        return;
      }
      await fetch('/api/matrix?scroll_speed=' + encodeURIComponent(speed));
      fetchState();
    }

    async function setScrollDirection(value) {
      await fetch('/api/matrix?scroll_dir=' + encodeURIComponent(value));
      fetchState();
    }

    async function startScroll() {
      const speed = parseInt(matrixScrollSpeed.value, 10);
      const dir = matrixScrollDirection.value;
      let query = '/api/matrix?scroll=1&scroll_speed=' + encodeURIComponent(speed) + '&scroll_dir=' + encodeURIComponent(dir);

      if (matrixScrollMode.value === 'multi') {
        const segments = collectSegmentsPayload();
        if (!segments) {
          alert('Add at least one colored word or phrase.');
          return;
        }
        query += '&segments=' + encodeURIComponent(segments);
      } else {
        const text = matrixText.value;
        if (!text.trim()) {
          alert('Type a message first.');
          return;
        }
        query += '&text=' + encodeURIComponent(text);
      }

      const res = await fetch(query);
      const data = await res.json();
      if (!res.ok) {
        alert(data.error || 'Failed to start text scroll');
      }
      fetchState();
    }

    async function stopScroll() {
      const res = await fetch('/api/matrix?scroll=0');
      const data = await res.json();
      if (!res.ok) {
        alert(data.error || 'Failed to stop text scroll');
      }
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

    matrixScrollSpeed.addEventListener('input', () => {
      matrixScrollSpeedVal.textContent = matrixScrollSpeed.value;
      clearTimeout(scrollTimer);
      scrollTimer = setTimeout(() => setScrollSpeed(matrixScrollSpeed.value), 120);
    });

    matrixScrollDirection.addEventListener('change', async () => {
      await setScrollDirection(matrixScrollDirection.value);
    });

    matrixScrollMode.addEventListener('change', () => {
      toggleScrollMode();
      if (matrixScrollMode.value === 'multi' && segmentsList.children.length === 0) {
        addSegmentRow('', '#FF0000');
      }
    });

    addSegment.addEventListener('click', () => {
      addSegmentRow('', '#00FF00');
    });

    document.querySelectorAll('button[data-color]').forEach(btn => {
      btn.addEventListener('click', () => sendColor(btn.dataset.color));
    });

    document.getElementById('off').addEventListener('click', () => sendColor('#000000'));

    document.getElementById('matrixTest').addEventListener('click', async () => {
      await fetch('/api/matrix?test=1');
      fetchState();
    });

    document.getElementById('matrixPinApply').addEventListener('click', async () => {
      await setMatrixPin(matrixPin.value);
    });

    document.getElementById('matrixCountApply').addEventListener('click', async () => {
      await setMatrixCount(matrixCount.value);
    });

    document.getElementById('matrixTextStart').addEventListener('click', async () => {
      await startScroll();
    });

    document.getElementById('matrixTextStop').addEventListener('click', async () => {
      await stopScroll();
    });

    document.getElementById('wifiSave').addEventListener('click', async () => {
      const ssid = wifiSsid.value.trim();
      const pass = wifiPass.value;
      if (!ssid) {
        alert('Please provide an SSID.');
        return;
      }
      statusEl.textContent = 'Connecting to Wi-Fi...';
      const res = await fetch('/api/wifi?ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(pass) + '&save=1');
      const data = await res.json();
      if (!res.ok) {
        alert(data.error || 'Failed to configure Wi-Fi');
      }
      fetchState();
    });

    document.getElementById('wifiForget').addEventListener('click', async () => {
      const res = await fetch('/api/wifi?forget=1');
      const data = await res.json();
      if (!res.ok) {
        alert(data.error || 'Failed to forget Wi-Fi');
      }
      fetchState();
    });

    document.getElementById('otaUpload').addEventListener('click', async () => {
      const fileInput = document.getElementById('otaFile');
      if (!fileInput.files || fileInput.files.length === 0) {
        alert('Select a .bin file');
        return;
      }

      const form = new FormData();
      form.append('firmware', fileInput.files[0]);
      statusEl.textContent = 'Uploading OTA firmware...';

      const res = await fetch('/api/update', { method: 'POST', body: form });
      let data = {};
      try { data = await res.json(); } catch (_) {}
      if (!res.ok) {
        alert(data.error || 'OTA failed');
        statusEl.textContent = 'OTA failed';
        return;
      }
      statusEl.textContent = 'OTA complete. Rebooting ESP...';
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

void handleApiWifi() {
  if (gWebServer.hasArg("forget") && gWebServer.arg("forget") != "0") {
    clearWifiSettings();
    WiFi.disconnect(true, true);
    startConfigAp();
    gMdnsStarted = false;
    gWebServer.send(200, "application/json", "{\"ok\":true,\"forgot\":true}");
    return;
  }

  if (!gWebServer.hasArg("ssid")) {
    gWebServer.send(200, "application/json", buildStateJson());
    return;
  }

  const String ssid = gWebServer.arg("ssid");
  const String password = gWebServer.hasArg("password") ? gWebServer.arg("password") : "";
  const bool save = !gWebServer.hasArg("save") || gWebServer.arg("save") != "0";

  if (ssid.length() == 0) {
    gWebServer.send(400, "application/json", "{\"error\":\"ssid_empty\"}");
    return;
  }

  if (!gApMode) {
    startConfigAp();
  }

  if (!connectWifiWithCredentials(ssid, password, 20000)) {
    gWebServer.send(502, "application/json", "{\"error\":\"wifi_connect_failed\"}");
    return;
  }

  if (save) {
    saveWifiSettings(ssid, password);
  }

  if (!gMdnsStarted) {
    gMdnsStarted = startMdns();
  }

  stopConfigAp();
  gWebServer.send(200, "application/json", buildStateJson());
}

void handleApiUpdateFinished() {
  if (gOtaHasError || Update.hasError()) {
    gWebServer.send(500, "application/json", "{\"error\":\"ota_failed\"}");
    return;
  }

  gWebServer.send(200, "application/json", "{\"ok\":true,\"message\":\"restarting\"}");
  delay(300);
  ESP.restart();
}

void handleApiUpdateUpload() {
  HTTPUpload &upload = gWebServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    gOtaHasError = !Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!gOtaHasError && Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      gOtaHasError = true;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!gOtaHasError) {
      gOtaHasError = !Update.end(true);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    gOtaHasError = true;
    Update.abort();
  }
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
  bool savePersistentSettings = false;

  if (gWebServer.hasArg("brightness")) {
    const int br = constrain(gWebServer.arg("brightness").toInt(), 0, 255);
    setMatrixBrightness(static_cast<uint8_t>(br));
    changed = true;
    savePersistentSettings = true;
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
    savePersistentSettings = true;
  }

  if (gWebServer.hasArg("scroll_speed")) {
    String speedArg = gWebServer.arg("scroll_speed");
    speedArg.trim();
    char *endPtr = nullptr;
    const long speedVal = strtol(speedArg.c_str(), &endPtr, 10);
    if (endPtr == speedArg.c_str() || endPtr == nullptr || *endPtr != '\0') {
      gWebServer.send(400, "application/json", "{\"error\":\"invalid_scroll_speed\"}");
      return;
    }

    gMatrixScrollStepMs = static_cast<uint16_t>(constrain(static_cast<int>(speedVal), 40, 1000));
    if (gMatrixScrollRunning) {
      gMatrixScrollLastStepMs = millis();
      renderMatrixScrollFrame();
    }
    changed = true;
  }

  if (gWebServer.hasArg("scroll_dir")) {
    ScrollDirection nextDirection = gMatrixScrollDirection;
    if (!parseScrollDirection(gWebServer.arg("scroll_dir"), nextDirection)) {
      gWebServer.send(400, "application/json", "{\"error\":\"invalid_scroll_direction\"}");
      return;
    }

    if (nextDirection != gMatrixScrollDirection) {
      gMatrixScrollDirection = nextDirection;
      if (gMatrixScrollRunning) {
        gMatrixScrollOffsetX = scrollStartOffsetX(gMatrixScrollDirection, gMatrixScrollText);
        gMatrixScrollLastStepMs = millis();
        renderMatrixScrollFrame();
      }
      savePersistentSettings = true;
    }
    changed = true;
  }

  if (gWebServer.hasArg("pin")) {
    String pinArg = gWebServer.arg("pin");
    pinArg.trim();
    char *endPtr = nullptr;
    const long pinVal = strtol(pinArg.c_str(), &endPtr, 10);
    if (endPtr == pinArg.c_str() || endPtr == nullptr || *endPtr != '\0') {
      gWebServer.send(400, "application/json", "{\"error\":\"invalid_pin\"}");
      return;
    }

    if (!isValidMatrixPin(static_cast<int>(pinVal))) {
      gWebServer.send(400, "application/json", "{\"error\":\"pin_out_of_range\"}");
      return;
    }

    gMatrixDataPin = static_cast<int>(pinVal);
    gMatrix.setPin(static_cast<int16_t>(gMatrixDataPin));
    gMatrix.begin();
    gMatrixReady = true;
    gMatrix.clear();
    gMatrix.show();
    if (gMatrixScrollRunning) {
      renderMatrixScrollFrame();
    } else if (!gMatrixTestRunning) {
      applyMatrixSolidColor(gLedColor);
    }
    Serial.printf("[OK] GPIO da matriz atualizado para %d\n", gMatrixDataPin);
    changed = true;
    savePersistentSettings = true;
  }

  if (gWebServer.hasArg("count")) {
    String countArg = gWebServer.arg("count");
    countArg.trim();
    char *endPtr = nullptr;
    const long countVal = strtol(countArg.c_str(), &endPtr, 10);
    if (endPtr == countArg.c_str() || endPtr == nullptr || *endPtr != '\0') {
      gWebServer.send(400, "application/json", "{\"error\":\"invalid_count\"}");
      return;
    }

    if (!isValidMatrixLedCount(static_cast<int>(countVal))) {
      gWebServer.send(400, "application/json", "{\"error\":\"count_out_of_range\"}");
      return;
    }

    gMatrixActiveLedCount = static_cast<uint16_t>(countVal);
    gMatrixTestRunning = false;
    if (gMatrixScrollRunning) {
      renderMatrixScrollFrame();
    } else {
      applyMatrixSolidColor(gLedColor);
    }
    Serial.printf("[OK] Quantidade de LEDs da matriz atualizada para %u\n", static_cast<unsigned>(gMatrixActiveLedCount));
    changed = true;
    savePersistentSettings = true;
  }

  const bool hasTextArg = gWebServer.hasArg("text");
  const bool hasSegmentsArg = gWebServer.hasArg("segments");
  if (gWebServer.hasArg("scroll")) {
    if (gWebServer.arg("scroll") != "0") {
      if (hasSegmentsArg) {
        if (!startMatrixScrollSegments(gWebServer.arg("segments"), gMatrixScrollStepMs)) {
          gWebServer.send(400, "application/json", "{\"error\":\"invalid_segments\"}");
          return;
        }
      } else {
        const String text = hasTextArg ? gWebServer.arg("text") : gMatrixScrollText;
        if (!startMatrixScroll(text, gMatrixScrollStepMs)) {
          gWebServer.send(400, "application/json", "{\"error\":\"text_empty\"}");
          return;
        }
      }
    } else {
      stopMatrixScroll();
    }
    changed = true;
  } else if (hasSegmentsArg) {
    if (!startMatrixScrollSegments(gWebServer.arg("segments"), gMatrixScrollStepMs)) {
      gWebServer.send(400, "application/json", "{\"error\":\"invalid_segments\"}");
      return;
    }
    changed = true;
  } else if (hasTextArg) {
    if (!startMatrixScroll(gWebServer.arg("text"), gMatrixScrollStepMs)) {
      gWebServer.send(400, "application/json", "{\"error\":\"text_empty\"}");
      return;
    }
    changed = true;
  }

  if (!changed) {
    gWebServer.send(400, "application/json", "{\"error\":\"invalid_params\"}");
    return;
  }

  if (savePersistentSettings) {
    saveSettings();
  }

  gWebServer.send(200, "application/json", buildStateJson());
}

bool startWebServer() {
  gWebServer.on("/", HTTP_GET, handleRoot);
  gWebServer.on("/api/state", HTTP_GET, handleApiState);
  gWebServer.on("/api/led", HTTP_GET, handleApiLed);
  gWebServer.on("/api/matrix", HTTP_GET, handleApiMatrix);
  gWebServer.on("/api/wifi", HTTP_GET, handleApiWifi);
  gWebServer.on("/api/update", HTTP_POST, handleApiUpdateFinished, handleApiUpdateUpload);
  gWebServer.onNotFound([]() {
    gWebServer.send(404, "text/plain", "Not found");
  });
  gWebServer.begin();

  Serial.println("[OK] Web server iniciado.");
  if (WiFi.isConnected()) {
    Serial.printf("Acesse: http://%s.local ou http://%s\n",
                  DEVICE_HOSTNAME,
                  WiFi.localIP().toString().c_str());
  }
  if (gApMode) {
    Serial.printf("Config AP: SSID=%s senha=%s URL=http://%s\n",
                  gApSsid.c_str(),
                  gApPassword.c_str(),
                  WiFi.softAPIP().toString().c_str());
  }
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

  gMatrixRuntimeMaxLedCount = detectRuntimeMaxLedCount();
  gMatrixActiveLedCount = gMatrixRuntimeMaxLedCount;
  Serial.printf("[OK] Limite automatico de LEDs em runtime: %u (teto compilado: %u)\n",
                static_cast<unsigned>(gMatrixRuntimeMaxLedCount),
                static_cast<unsigned>(kMatrixLedCount));

  loadSettings();
  initMatrix();
  applyMatrixSolidColor(gLedColor);

  if (connectConfiguredWifi()) {
    gMdnsStarted = startMdns();
    (void)gMdnsStarted;
  } else {
    startConfigAp();
  }

  gWebServerStarted = startWebServer();

  Serial.println("=== FIM DIAGNOSTICO ===");
}

void loop() {
  if (gWebServerStarted) {
    gWebServer.handleClient();
  }

  tickMatrixScroll();
  tickMatrixTest();

  static unsigned long lastPrint = 0;
  const unsigned long now = millis();
  if (now - lastPrint >= 3000) {
    lastPrint = now;
    Serial.printf(
      "Heartbeat | uptime=%lu ms | heap=%u | psram_free=%u | wifi=%d | ip=%s | led=%s | matrix_br=%u | matrix_test=%d | matrix_scroll=%d | scroll_dir=%s\n",
      now,
      ESP.getFreeHeap(),
      ESP.getFreePsram(),
      static_cast<int>(WiFi.status()),
      WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "0.0.0.0",
      ledHexColor().c_str(),
      gMatrixBrightness,
      gMatrixTestRunning ? 1 : 0,
      gMatrixScrollRunning ? 1 : 0,
      scrollDirectionToString(gMatrixScrollDirection));
  }

  delay(10);
}
