#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <new>
#include <ctype.h>
#include <ESPmDNS.h>
#include <esp_system.h>
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

#ifndef MATRIX_OUTPUT_COUNT
#define MATRIX_OUTPUT_COUNT 2
#endif

#ifndef MATRIX_ACTIVE_OUTPUTS_DEFAULT
#define MATRIX_ACTIVE_OUTPUTS_DEFAULT 2
#endif

#ifndef MATRIX_MAX_OUTPUTS
#define MATRIX_MAX_OUTPUTS 8
#endif

#ifndef MATRIX_SEGMENT_WIDTH
#define MATRIX_SEGMENT_WIDTH 8
#endif

#ifndef MATRIX_HEIGHT
#define MATRIX_HEIGHT 8
#endif

#ifndef MATRIX_PIN_0
#define MATRIX_PIN_0 14
#endif

#ifndef MATRIX_PIN_1
#define MATRIX_PIN_1 17
#endif

#ifndef MATRIX_PIN_2
#define MATRIX_PIN_2 4
#endif

#ifndef MATRIX_PIN_3
#define MATRIX_PIN_3 5
#endif

#ifndef MATRIX_PIN_4
#define MATRIX_PIN_4 6
#endif

#ifndef MATRIX_PIN_5
#define MATRIX_PIN_5 7
#endif

#ifndef MATRIX_PIN_6
#define MATRIX_PIN_6 15
#endif

#ifndef MATRIX_PIN_7
#define MATRIX_PIN_7 16
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

#ifndef MATRIX_SCAN_ORDER
#define MATRIX_SCAN_ORDER 1
#endif

#ifndef MATRIX_MAX_LEDS
#define MATRIX_MAX_LEDS 6720
#endif

#ifndef MATRIX_BRIGHTNESS_DEFAULT
#define MATRIX_BRIGHTNESS_DEFAULT 32
#endif

static const uint16_t kMatrixCompiledMaxLedCount = MATRIX_MAX_LEDS;
static const uint16_t kMatrixDefaultLedsPerOutput = MATRIX_SEGMENT_WIDTH * MATRIX_HEIGHT;

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

enum class MatrixScanOrder : uint8_t {
  RowMajor = 0,
  ColumnMajor = 1,
};

WebServer gWebServer(80);
uint32_t *gMatrixBuffer[MATRIX_OUTPUT_COUNT] = {nullptr};
Adafruit_NeoPixel *gMatrixStrips[MATRIX_OUTPUT_COUNT] = {nullptr};
const uint8_t kMatrixDefaultPins[MATRIX_MAX_OUTPUTS] = {
  MATRIX_PIN_0,
  MATRIX_PIN_1,
  MATRIX_PIN_2,
  MATRIX_PIN_3,
  MATRIX_PIN_4,
  MATRIX_PIN_5,
  MATRIX_PIN_6,
  MATRIX_PIN_7,
};
static_assert(MATRIX_OUTPUT_COUNT <= MATRIX_MAX_OUTPUTS, "MATRIX_OUTPUT_COUNT exceeds MATRIX_MAX_OUTPUTS");
uint8_t gMatrixPins[MATRIX_OUTPUT_COUNT] = {0};
uint16_t gMatrixLedsPerOutput[MATRIX_OUTPUT_COUNT] = {0};
uint16_t gMatrixColsPerOutput[MATRIX_OUTPUT_COUNT] = {0};
uint16_t gMatrixXOffsets[MATRIX_OUTPUT_COUNT + 1] = {0};
uint16_t gMatrixTotalWidth = MATRIX_OUTPUT_COUNT * MATRIX_SEGMENT_WIDTH;
uint8_t gMatrixActiveOutputs =
  (MATRIX_ACTIVE_OUTPUTS_DEFAULT < 1)
    ? 1
    : ((MATRIX_ACTIVE_OUTPUTS_DEFAULT > MATRIX_OUTPUT_COUNT) ? MATRIX_OUTPUT_COUNT : MATRIX_ACTIVE_OUTPUTS_DEFAULT);

RgbColor gLedColor = {0, 0, 0};
int gMatrixDataPin = MATRIX_PIN_0;
uint16_t gMatrixActiveLedCount = 0;
uint16_t gMatrixRuntimeMaxLedCount = kMatrixCompiledMaxLedCount;
uint8_t gMatrixBrightness = MATRIX_BRIGHTNESS_DEFAULT;
bool gMatrixReady = false;
bool gMatrixTestRunning = false;
uint16_t gMatrixTestIndex = 0;
unsigned long gMatrixLastStepMs = 0;
bool gMatrixScrollRunning = false;
String gMatrixScrollText = "HELLO";
int16_t gMatrixScrollOffsetX = 0;
unsigned long gMatrixScrollLastStepMs = 0;
uint16_t gMatrixScrollStepMs = 120;
ScrollDirection gMatrixScrollDirection = ScrollDirection::Left;
MatrixScanOrder gMatrixScanOrder = (MATRIX_SCAN_ORDER == 0) ? MatrixScanOrder::RowMajor
                                                            : MatrixScanOrder::ColumnMajor;
bool gMatrixXFlip = (MATRIX_X_FLIP != 0);
bool gMatrixYFlip = (MATRIX_Y_FLIP != 0);

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
bool gSafeMode = false;
String gSafeModeReason;
bool gBootMarkedStable = false;

RTC_DATA_ATTR uint32_t gBootGuardArmed = 0;
RTC_DATA_ATTR uint32_t gBootGuardAttempts = 0;
RTC_DATA_ATTR uint32_t gRecoveryBootToken = 0;
static const uint32_t kBootGuardMagic = 0xB007B007;
static const uint32_t kBootGuardThreshold = 3;
static const unsigned long kBootGuardStableMs = 30000;
static const uint32_t kRecoveryBootMagic = 0x5AFE1234;

void renderMatrixScrollFrame();
bool mapMatrixXY(uint16_t x, uint8_t y, uint8_t &output, uint16_t &index);

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

uint32_t packColor(uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

void clearMatrixBuffer() {
  for (uint8_t output = 0; output < gMatrixActiveOutputs; output++) {
    if (gMatrixBuffer[output] == nullptr) {
      continue;
    }
    for (uint16_t i = 0; i < gMatrixLedsPerOutput[output]; i++) {
      gMatrixBuffer[output][i] = 0;
    }
  }
}

void showMatrix() {
  for (uint8_t output = 0; output < gMatrixActiveOutputs; output++) {
    Adafruit_NeoPixel *strip = gMatrixStrips[output];
    if (strip == nullptr) {
      continue;
    }
    strip->setBrightness(gMatrixBrightness);
    if (gMatrixBuffer[output] == nullptr) {
      continue;
    }
    for (uint16_t i = 0; i < gMatrixLedsPerOutput[output]; i++) {
      strip->setPixelColor(i, gMatrixBuffer[output][i]);
    }
    strip->show();
  }
}

void applyMatrixSolidColor(const RgbColor &color) {
  if (!gMatrixReady) {
    return;
  }

  clearMatrixBuffer();

  const uint32_t packed = packColor(color.r, color.g, color.b);
  for (uint8_t output = 0; output < gMatrixActiveOutputs; output++) {
    if (gMatrixBuffer[output] == nullptr) {
      continue;
    }
    for (uint16_t i = 0; i < gMatrixLedsPerOutput[output]; i++) {
      gMatrixBuffer[output][i] = packed;
    }
  }
  showMatrix();
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

const char *matrixScanOrderToString(MatrixScanOrder order) {
  return order == MatrixScanOrder::ColumnMajor ? "column" : "row";
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

bool parseMatrixScanOrder(const String &value, MatrixScanOrder &out) {
  String scan = value;
  scan.trim();
  scan.toLowerCase();

  if (scan == "row" || scan == "rows" || scan == "row_major") {
    out = MatrixScanOrder::RowMajor;
    return true;
  }
  if (scan == "column" || scan == "columns" || scan == "col" || scan == "column_major") {
    out = MatrixScanOrder::ColumnMajor;
    return true;
  }

  return false;
}

bool parseBoolArg(const String &value, bool &out) {
  String v = value;
  v.trim();
  v.toLowerCase();

  if (v == "1" || v == "true" || v == "on" || v == "yes") {
    out = true;
    return true;
  }
  if (v == "0" || v == "false" || v == "off" || v == "no") {
    out = false;
    return true;
  }
  return false;
}

void applyMatrixFlips(bool xFlip, bool yFlip) {
  gMatrixXFlip = xFlip;
  gMatrixYFlip = yFlip;
  if (!gMatrixReady) {
    return;
  }

  if (gMatrixScrollRunning) {
    renderMatrixScrollFrame();
  } else {
    applyMatrixSolidColor(gLedColor);
  }
  Serial.printf("[OK] Matrix flip updated | x=%d | y=%d\n",
                gMatrixXFlip ? 1 : 0,
                gMatrixYFlip ? 1 : 0);
}

void applyMatrixScanOrder(MatrixScanOrder order) {
  gMatrixScanOrder = order;
  if (!gMatrixReady) {
    return;
  }

  if (gMatrixScrollRunning) {
    renderMatrixScrollFrame();
  } else {
    applyMatrixSolidColor(gLedColor);
  }
  Serial.printf("[OK] Matrix scan mapping set to %s-major\n", matrixScanOrderToString(order));
}

const char *resetReasonToString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "power_on";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interrupt_wdt";
    case ESP_RST_TASK_WDT:
      return "task_wdt";
    case ESP_RST_WDT:
      return "other_wdt";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_DEEPSLEEP:
      return "deepsleep";
    default:
      return "other";
  }
}

void armBootGuard() {
  if (gBootGuardArmed == kBootGuardMagic) {
    gBootGuardAttempts++;
  } else {
    gBootGuardAttempts = 1;
  }
  gBootGuardArmed = kBootGuardMagic;
}

void clearBootGuard() {
  gBootGuardArmed = 0;
  gBootGuardAttempts = 0;
}

String matrixPinsCsv() {
  String pins;
  for (uint8_t i = 0; i < gMatrixActiveOutputs; i++) {
    if (i > 0) {
      pins += ",";
    }
    pins += String(gMatrixPins[i]);
  }
  return pins;
}

String matrixCountsCsv() {
  String counts;
  for (uint8_t i = 0; i < gMatrixActiveOutputs; i++) {
    if (i > 0) {
      counts += ",";
    }
    counts += String(gMatrixLedsPerOutput[i]);
  }
  return counts;
}

uint16_t matrixWidth() {
  return gMatrixTotalWidth;
}

uint8_t clampActiveOutputs(int value) {
  if (value < 1) {
    return 1;
  }
  if (value > MATRIX_OUTPUT_COUNT) {
    return MATRIX_OUTPUT_COUNT;
  }
  return static_cast<uint8_t>(value);
}

bool isValidMatrixPin(int pin) {
  return pin >= 0 && pin < static_cast<int>(SOC_GPIO_PIN_COUNT);
}

void loadDefaultMatrixPins() {
  for (uint8_t i = 0; i < MATRIX_OUTPUT_COUNT; i++) {
    gMatrixPins[i] = kMatrixDefaultPins[i];
  }
  gMatrixDataPin = gMatrixPins[0];
}

void loadDefaultMatrixCounts() {
  for (uint8_t i = 0; i < MATRIX_OUTPUT_COUNT; i++) {
    gMatrixLedsPerOutput[i] = kMatrixDefaultLedsPerOutput;
  }
}

bool matrixCountsAreValid(const uint16_t counts[MATRIX_OUTPUT_COUNT],
                         uint8_t activeOutputs,
                         String &errorCode) {
  uint32_t total = 0;
  for (uint8_t i = 0; i < activeOutputs; i++) {
    const uint16_t count = counts[i];
    if (count == 0) {
      errorCode = "count_zero_not_allowed";
      return false;
    }
    if ((count % MATRIX_HEIGHT) != 0) {
      errorCode = "count_must_be_multiple_of_matrix_height";
      return false;
    }
    total += count;
  }

  if (total > gMatrixRuntimeMaxLedCount) {
    errorCode = "counts_exceed_runtime_limit";
    return false;
  }
  return true;
}

bool rebuildMatrixGeometry(const uint16_t counts[MATRIX_OUTPUT_COUNT],
                          uint8_t activeOutputs,
                          String &errorCode) {
  if (!matrixCountsAreValid(counts, activeOutputs, errorCode)) {
    return false;
  }

  uint16_t x = 0;
  uint32_t total = 0;
  gMatrixXOffsets[0] = 0;
  for (uint8_t i = 0; i < activeOutputs; i++) {
    gMatrixColsPerOutput[i] = static_cast<uint16_t>(counts[i] / MATRIX_HEIGHT);
    x = static_cast<uint16_t>(x + gMatrixColsPerOutput[i]);
    gMatrixXOffsets[i + 1] = x;
    total += counts[i];
  }
  for (uint8_t i = activeOutputs; i < MATRIX_OUTPUT_COUNT; i++) {
    gMatrixColsPerOutput[i] = 0;
    gMatrixXOffsets[i + 1] = x;
  }

  gMatrixTotalWidth = x;
  gMatrixActiveLedCount = static_cast<uint16_t>(total);
  return true;
}

bool parseMatrixCountsCsv(String csv,
                         uint8_t expectedCount,
                         uint16_t outCounts[MATRIX_OUTPUT_COUNT],
                         String &errorCode) {
  csv.trim();
  if (csv.length() == 0) {
    errorCode = "counts_empty";
    return false;
  }

  uint8_t count = 0;
  int start = 0;
  while (start <= csv.length()) {
    const int separator = csv.indexOf(',', start);
    String token = (separator < 0) ? csv.substring(start) : csv.substring(start, separator);
    token.trim();
    if (token.length() == 0) {
      errorCode = "invalid_counts_format";
      return false;
    }

    char *endPtr = nullptr;
    const long val = strtol(token.c_str(), &endPtr, 10);
    if (endPtr == token.c_str() || endPtr == nullptr || *endPtr != '\0') {
      errorCode = "invalid_count_value";
      return false;
    }
    if (val <= 0 || val > 65535) {
      errorCode = "count_out_of_range";
      return false;
    }
    if (count >= expectedCount) {
      errorCode = "too_many_counts";
      return false;
    }

    outCounts[count++] = static_cast<uint16_t>(val);

    if (separator < 0) {
      break;
    }
    start = separator + 1;
  }

  if (count != expectedCount) {
    errorCode = "counts_count_mismatch";
    return false;
  }
  return true;
}

bool matrixPinsAreUnique(const uint8_t pins[MATRIX_OUTPUT_COUNT], uint8_t activeOutputs) {
  for (uint8_t i = 0; i < activeOutputs; i++) {
    for (uint8_t j = i + 1; j < activeOutputs; j++) {
      if (pins[i] == pins[j]) {
        return false;
      }
    }
  }
  return true;
}

bool parseMatrixPinsCsv(String csv,
                       uint8_t expectedCount,
                       uint8_t outPins[MATRIX_OUTPUT_COUNT],
                       String &errorCode) {
  csv.trim();
  if (csv.length() == 0) {
    errorCode = "pins_empty";
    return false;
  }

  uint8_t count = 0;
  int start = 0;
  while (start <= csv.length()) {
    const int separator = csv.indexOf(',', start);
    String token = (separator < 0) ? csv.substring(start) : csv.substring(start, separator);
    token.trim();
    if (token.length() == 0) {
      errorCode = "invalid_pins_format";
      return false;
    }

    char *endPtr = nullptr;
    const long pin = strtol(token.c_str(), &endPtr, 10);
    if (endPtr == token.c_str() || endPtr == nullptr || *endPtr != '\0') {
      errorCode = "invalid_pin_value";
      return false;
    }
    if (!isValidMatrixPin(static_cast<int>(pin))) {
      errorCode = "pin_out_of_range";
      return false;
    }
    if (count >= expectedCount) {
      errorCode = "too_many_pins";
      return false;
    }

    outPins[count++] = static_cast<uint8_t>(pin);

    if (separator < 0) {
      break;
    }
    start = separator + 1;
  }

  if (count != expectedCount) {
    errorCode = "pins_count_mismatch";
    return false;
  }
  if (!matrixPinsAreUnique(outPins, expectedCount)) {
    errorCode = "duplicate_pins";
    return false;
  }
  return true;
}

uint16_t detectRuntimeMaxLedCount() {
  // WS2812 + buffer interno + buffer de frame: ~7-8 bytes por LED.
  // Reservamos heap para Wi-Fi/WebServer e calculamos um teto seguro em runtime.
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t reservedHeap = 48 * 1024;
  const uint32_t minReasonable = gMatrixActiveOutputs * MATRIX_HEIGHT;

  if (freeHeap <= reservedHeap) {
    return static_cast<uint16_t>(minReasonable);
  }

  uint32_t byHeap = (freeHeap - reservedHeap) / 8;
  if (byHeap < minReasonable) {
    byHeap = minReasonable;
  }
  if (byHeap > kMatrixCompiledMaxLedCount) {
    byHeap = kMatrixCompiledMaxLedCount;
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
  pref.putUChar("mout", gMatrixActiveOutputs);
  pref.putUChar("mscan", static_cast<uint8_t>(gMatrixScanOrder));
  pref.putUChar("mxf", gMatrixXFlip ? 1 : 0);
  pref.putUChar("myf", gMatrixYFlip ? 1 : 0);
  for (uint8_t i = 0; i < MATRIX_OUTPUT_COUNT; i++) {
    char pinKey[6];
    char countKey[6];
    snprintf(pinKey, sizeof(pinKey), "mp%u", static_cast<unsigned>(i));
    snprintf(countKey, sizeof(countKey), "mc%u", static_cast<unsigned>(i));
    pref.putUChar(pinKey, gMatrixPins[i]);
    pref.putUShort(countKey, gMatrixLedsPerOutput[i]);
  }
  pref.putUShort("mcount", gMatrixActiveLedCount);
  pref.putUChar("msdir", static_cast<uint8_t>(gMatrixScrollDirection));
  pref.end();
}

void loadSettings() {
  loadDefaultMatrixPins();
  loadDefaultMatrixCounts();

  Preferences pref;
  if (!pref.begin("ledcfg", true)) {
    return;
  }

  const uint8_t r = pref.getUChar("r", 0);
  const uint8_t g = pref.getUChar("g", 0);
  const uint8_t b = pref.getUChar("b", 0);
  const uint8_t br = pref.getUChar("br", MATRIX_BRIGHTNESS_DEFAULT);
  const uint8_t activeOutputsRaw = pref.getUChar("mout", gMatrixActiveOutputs);
  const uint16_t legacyTotalCount = pref.getUShort("mcount", 0);
  const uint8_t scrollDirRaw = pref.getUChar("msdir", static_cast<uint8_t>(ScrollDirection::Left));
  const uint8_t scanRaw = pref.getUChar("mscan", static_cast<uint8_t>(gMatrixScanOrder));
  const uint8_t xFlipRaw = pref.getUChar("mxf", gMatrixXFlip ? 1 : 0);
  const uint8_t yFlipRaw = pref.getUChar("myf", gMatrixYFlip ? 1 : 0);

  gMatrixActiveOutputs = clampActiveOutputs(static_cast<int>(activeOutputsRaw));
  gMatrixRuntimeMaxLedCount = detectRuntimeMaxLedCount();

  bool hasAnyPerOutputCount = false;
  for (uint8_t i = 0; i < MATRIX_OUTPUT_COUNT; i++) {
    char pinKey[6];
    char countKey[6];
    snprintf(pinKey, sizeof(pinKey), "mp%u", static_cast<unsigned>(i));
    snprintf(countKey, sizeof(countKey), "mc%u", static_cast<unsigned>(i));

    int loadedPin = kMatrixDefaultPins[i];
    if (pref.isKey(pinKey)) {
      loadedPin = static_cast<int>(pref.getUChar(pinKey, static_cast<uint8_t>(kMatrixDefaultPins[i])));
    } else if (i == 0) {
      loadedPin = pref.getInt("mpin", kMatrixDefaultPins[0]);
    }

    if (isValidMatrixPin(loadedPin)) {
      gMatrixPins[i] = static_cast<uint8_t>(loadedPin);
    }

    if (pref.isKey(countKey)) {
      const uint16_t loadedCount = pref.getUShort(countKey, kMatrixDefaultLedsPerOutput);
      gMatrixLedsPerOutput[i] = loadedCount;
      hasAnyPerOutputCount = true;
    }
  }

  if (!hasAnyPerOutputCount && legacyTotalCount > 0) {
    if ((legacyTotalCount % gMatrixActiveOutputs) == 0) {
      const uint16_t each = static_cast<uint16_t>(legacyTotalCount / gMatrixActiveOutputs);
      if ((each % MATRIX_HEIGHT) == 0) {
        for (uint8_t i = 0; i < gMatrixActiveOutputs; i++) {
          gMatrixLedsPerOutput[i] = each;
        }
      }
    }
  }
  pref.end();

  if (!matrixPinsAreUnique(gMatrixPins, gMatrixActiveOutputs)) {
    loadDefaultMatrixPins();
  } else {
    gMatrixDataPin = gMatrixPins[0];
  }

  String geometryError;
  if (!rebuildMatrixGeometry(gMatrixLedsPerOutput, gMatrixActiveOutputs, geometryError)) {
    loadDefaultMatrixCounts();
    if (!rebuildMatrixGeometry(gMatrixLedsPerOutput, gMatrixActiveOutputs, geometryError)) {
      gMatrixActiveLedCount = gMatrixActiveOutputs * MATRIX_HEIGHT;
      gMatrixTotalWidth = gMatrixActiveOutputs;
    }
  }

  gMatrixBrightness = br;
  gMatrixScrollDirection = (scrollDirRaw == static_cast<uint8_t>(ScrollDirection::Right))
                             ? ScrollDirection::Right
                             : ScrollDirection::Left;
  gMatrixScanOrder = (scanRaw == static_cast<uint8_t>(MatrixScanOrder::RowMajor))
                       ? MatrixScanOrder::RowMajor
                       : MatrixScanOrder::ColumnMajor;
  gMatrixXFlip = (xFlipRaw != 0);
  gMatrixYFlip = (yFlipRaw != 0);
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
    return packColor(255 - pos * 3, 0, pos * 3);
  }
  if (pos < 170) {
    pos -= 85;
    return packColor(0, pos * 3, 255 - pos * 3);
  }
  pos -= 170;
  return packColor(pos * 3, 255 - pos * 3, 0);
}

bool mapMatrixXY(uint16_t x, uint8_t y, uint8_t &output, uint16_t &index) {
  if (matrixWidth() == 0 || x >= matrixWidth() || y >= MATRIX_HEIGHT) {
    return false;
  }

  uint16_t mappedX = gMatrixXFlip ? (matrixWidth() - 1 - x) : x;
  const uint8_t mappedY = gMatrixYFlip ? (MATRIX_HEIGHT - 1 - y) : y;

  const uint32_t logical = static_cast<uint32_t>(mappedY) * matrixWidth() + mappedX;
  if (logical >= gMatrixActiveLedCount) {
    return false;
  }

  output = 0;
  while (output < gMatrixActiveOutputs && mappedX >= gMatrixXOffsets[output + 1]) {
    output++;
  }
  if (output >= gMatrixActiveOutputs) {
    return false;
  }

  uint16_t localX = static_cast<uint16_t>(mappedX - gMatrixXOffsets[output]);
  const uint16_t outputWidth = gMatrixColsPerOutput[output];
  if (outputWidth == 0 || localX >= outputWidth) {
    return false;
  }

  uint8_t localY = mappedY;
  if (gMatrixScanOrder == MatrixScanOrder::ColumnMajor) {
    if (MATRIX_SERPENTINE && ((localX & 0x01) != 0)) {
      localY = MATRIX_HEIGHT - 1 - localY;
    }
    index = static_cast<uint16_t>(localX) * MATRIX_HEIGHT + localY;
  } else {
    if (MATRIX_SERPENTINE && ((localY & 0x01) != 0)) {
      localX = outputWidth - 1 - localX;
    }
    index = static_cast<uint16_t>(localY) * outputWidth + localX;
  }
  return index < gMatrixLedsPerOutput[output];
}

void setMatrixPixel(uint16_t x, uint8_t y, uint32_t color) {
  uint8_t output = 0;
  uint16_t index = 0;
  if (mapMatrixXY(x, y, output, index)) {
    gMatrixBuffer[output][index] = color;
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
      if (px < 0 || py < 0 || px >= matrixWidth() || py >= MATRIX_HEIGHT) {
        continue;
      }

      setMatrixPixel(static_cast<uint16_t>(px), static_cast<uint8_t>(py), color);
    }
  }
}

int16_t scrollTextPixelWidth(const String &text) {
  if (text.length() == 0) {
    return 0;
  }

  return static_cast<int16_t>(text.length() * (kScrollGlyphWidth + kScrollGlyphSpacing));
}

int16_t scrollLoopPeriodPx(const String &text) {
  const int16_t textWidth = scrollTextPixelWidth(text);
  if (textWidth <= 0) {
    return 1;
  }
  return textWidth;
}

int16_t scrollStartOffsetX(ScrollDirection direction, const String &text) {
  if (direction == ScrollDirection::Right) {
    return -scrollTextPixelWidth(text);
  }
  return matrixWidth();
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

        const uint32_t packed = packColor(segmentColor.r, segmentColor.g, segmentColor.b);
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

  clearMatrixBuffer();

  const int16_t yOffset = MATRIX_HEIGHT > kScrollFontHeight ? (MATRIX_HEIGHT - kScrollFontHeight) / 2 : 0;
  const uint32_t color = packColor(gLedColor.r, gLedColor.g, gLedColor.b);
  const int16_t textWidth = scrollTextPixelWidth(gMatrixScrollText);
  const int16_t period = scrollLoopPeriodPx(gMatrixScrollText);
  if (textWidth <= 0 || period <= 0) {
    showMatrix();
    return;
  }

  if (gMatrixScrollDirection == ScrollDirection::Right) {
    for (int16_t baseX = gMatrixScrollOffsetX; baseX + textWidth >= 0; baseX -= period) {
      for (size_t i = 0; i < gMatrixScrollText.length(); i++) {
        const int16_t x = baseX + static_cast<int16_t>(i * (kScrollGlyphWidth + kScrollGlyphSpacing));
        if (x > (matrixWidth() - 1) || (x + kScrollGlyphWidth) < 0) {
          continue;
        }
        const uint32_t glyphColor =
          (gMatrixScrollUseCharColors && i < kScrollTextMaxLength) ? gMatrixScrollCharColors[i] : color;
        drawGlyphAt(x, yOffset, gMatrixScrollText.charAt(i), glyphColor);
      }
    }
  } else {
    for (int16_t baseX = gMatrixScrollOffsetX; baseX < matrixWidth(); baseX += period) {
      for (size_t i = 0; i < gMatrixScrollText.length(); i++) {
        const int16_t x = baseX + static_cast<int16_t>(i * (kScrollGlyphWidth + kScrollGlyphSpacing));
        if (x > (matrixWidth() - 1) || (x + kScrollGlyphWidth) < 0) {
          continue;
        }
        const uint32_t glyphColor =
          (gMatrixScrollUseCharColors && i < kScrollTextMaxLength) ? gMatrixScrollCharColors[i] : color;
        drawGlyphAt(x, yOffset, gMatrixScrollText.charAt(i), glyphColor);
      }
    }
  }

  showMatrix();
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

  const int16_t period = scrollLoopPeriodPx(gMatrixScrollText);
  if (period <= 0) {
    return;
  }
  if (gMatrixScrollDirection == ScrollDirection::Right) {
    gMatrixScrollOffsetX++;
    if (gMatrixScrollOffsetX >= period) {
      gMatrixScrollOffsetX -= period;
    }
  } else {
    gMatrixScrollOffsetX--;
    if (gMatrixScrollOffsetX <= -period) {
      gMatrixScrollOffsetX += period;
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

  clearMatrixBuffer();
  uint8_t wheel = 0;
  if (gMatrixActiveLedCount > 1) {
    wheel = static_cast<uint8_t>((gMatrixTestIndex * 255) / (gMatrixActiveLedCount - 1));
  }
  const uint16_t x = static_cast<uint16_t>(gMatrixTestIndex % matrixWidth());
  const uint8_t y = static_cast<uint8_t>(gMatrixTestIndex / matrixWidth());
  setMatrixPixel(x, y, colorWheel(wheel));
  showMatrix();

  gMatrixTestIndex++;
  if (gMatrixTestIndex >= gMatrixActiveLedCount) {
    gMatrixTestRunning = false;
    applyMatrixSolidColor(gLedColor);
    Serial.println("[OK] Teste da matriz concluido.");
  }
}

void releaseMatrixControllers(Adafruit_NeoPixel *controllers[MATRIX_OUTPUT_COUNT]) {
  for (uint8_t output = 0; output < MATRIX_OUTPUT_COUNT; output++) {
    if (controllers[output] != nullptr) {
      controllers[output]->clear();
      controllers[output]->show();
      delete controllers[output];
      controllers[output] = nullptr;
    }
  }
}

void releaseMatrixBuffers(uint32_t *buffers[MATRIX_OUTPUT_COUNT]) {
  for (uint8_t output = 0; output < MATRIX_OUTPUT_COUNT; output++) {
    if (buffers[output] != nullptr) {
      delete[] buffers[output];
      buffers[output] = nullptr;
    }
  }
}

bool createMatrixResources(const uint8_t pins[MATRIX_OUTPUT_COUNT],
                           uint8_t activeOutputs,
                           const uint16_t counts[MATRIX_OUTPUT_COUNT],
                           Adafruit_NeoPixel *controllers[MATRIX_OUTPUT_COUNT],
                           uint32_t *buffers[MATRIX_OUTPUT_COUNT]) {
  for (uint8_t output = 0; output < MATRIX_OUTPUT_COUNT; output++) {
    controllers[output] = nullptr;
    buffers[output] = nullptr;
  }

  for (uint8_t output = 0; output < activeOutputs; output++) {
    const uint8_t pin = pins[output];
    const uint16_t ledCount = counts[output];
    if (!isValidMatrixPin(pin)) {
      Serial.printf("[FAIL] Invalid matrix pin on output %u: %u\n",
                    static_cast<unsigned>(output),
                    static_cast<unsigned>(pin));
      releaseMatrixControllers(controllers);
      releaseMatrixBuffers(buffers);
      return false;
    }

    if (ledCount == 0) {
      Serial.printf("[FAIL] Invalid LED count on output %u\n",
                    static_cast<unsigned>(output));
      releaseMatrixControllers(controllers);
      releaseMatrixBuffers(buffers);
      return false;
    }

    buffers[output] = new (std::nothrow) uint32_t[ledCount];
    if (buffers[output] == nullptr) {
      Serial.printf("[FAIL] Matrix buffer allocation failed on output %u (count=%u)\n",
                    static_cast<unsigned>(output),
                    static_cast<unsigned>(ledCount));
      releaseMatrixControllers(controllers);
      releaseMatrixBuffers(buffers);
      return false;
    }
    for (uint16_t i = 0; i < ledCount; i++) {
      buffers[output][i] = 0;
    }

    controllers[output] = new (std::nothrow) Adafruit_NeoPixel(
      ledCount, pin, NEO_GRB + NEO_KHZ800);
    if (controllers[output] == nullptr) {
      Serial.printf("[FAIL] Matrix strip allocation failed on output %u (pin=%u)\n",
                    static_cast<unsigned>(output),
                    static_cast<unsigned>(pin));
      releaseMatrixControllers(controllers);
      releaseMatrixBuffers(buffers);
      return false;
    }

    controllers[output]->begin();
    controllers[output]->clear();
    controllers[output]->show();
  }

  return true;
}

bool initMatrix() {
  if (!matrixPinsAreUnique(gMatrixPins, gMatrixActiveOutputs)) {
    Serial.println("[FAIL] Matrix outputs must use unique GPIO pins.");
    gMatrixReady = false;
    return false;
  }

  String geometryError;
  if (!rebuildMatrixGeometry(gMatrixLedsPerOutput, gMatrixActiveOutputs, geometryError)) {
    Serial.printf("[FAIL] Matrix geometry invalid: %s\n", geometryError.c_str());
    gMatrixReady = false;
    return false;
  }

  Adafruit_NeoPixel *newControllers[MATRIX_OUTPUT_COUNT] = {nullptr};
  uint32_t *newBuffers[MATRIX_OUTPUT_COUNT] = {nullptr};
  if (!createMatrixResources(gMatrixPins, gMatrixActiveOutputs, gMatrixLedsPerOutput, newControllers, newBuffers)) {
    gMatrixReady = false;
    return false;
  }

  releaseMatrixControllers(gMatrixStrips);
  releaseMatrixBuffers(gMatrixBuffer);
  for (uint8_t output = 0; output < MATRIX_OUTPUT_COUNT; output++) {
    gMatrixStrips[output] = newControllers[output];
    gMatrixBuffer[output] = newBuffers[output];
    newControllers[output] = nullptr;
    newBuffers[output] = nullptr;
  }

  gMatrixDataPin = gMatrixPins[0];
  gMatrixReady = true;
  clearMatrixBuffer();
  showMatrix();
  Serial.printf("[OK] Matriz WS2812 paralela pronta | outputs=%u/%u | pins=[%s] | counts=[%s] | width=%u | leds=%u | brilho=%u\n",
                static_cast<unsigned>(gMatrixActiveOutputs),
                static_cast<unsigned>(MATRIX_OUTPUT_COUNT),
                matrixPinsCsv().c_str(),
                matrixCountsCsv().c_str(),
                static_cast<unsigned>(matrixWidth()),
                static_cast<unsigned>(gMatrixActiveLedCount),
                gMatrixBrightness);
  return true;
}

bool applyMatrixPins(const uint8_t newPins[MATRIX_OUTPUT_COUNT]) {
  if (!matrixPinsAreUnique(newPins, gMatrixActiveOutputs)) {
    return false;
  }

  bool changed = false;
  for (uint8_t i = 0; i < gMatrixActiveOutputs; i++) {
    if (gMatrixPins[i] != newPins[i]) {
      changed = true;
      break;
    }
  }
  if (!changed) {
    return true;
  }

  uint8_t previousPins[MATRIX_OUTPUT_COUNT] = {0};
  for (uint8_t i = 0; i < gMatrixActiveOutputs; i++) {
    previousPins[i] = gMatrixPins[i];
    gMatrixPins[i] = newPins[i];
  }
  gMatrixDataPin = gMatrixPins[0];

  const bool wasScrollRunning = gMatrixScrollRunning;
  gMatrixTestRunning = false;
  gMatrixScrollRunning = false;

  if (!initMatrix()) {
    for (uint8_t i = 0; i < gMatrixActiveOutputs; i++) {
      gMatrixPins[i] = previousPins[i];
    }
    gMatrixDataPin = gMatrixPins[0];
    if (!initMatrix()) {
      gMatrixReady = false;
    }
    return false;
  }

  if (wasScrollRunning) {
    gMatrixScrollRunning = true;
    gMatrixScrollOffsetX = scrollStartOffsetX(gMatrixScrollDirection, gMatrixScrollText);
    gMatrixScrollLastStepMs = millis();
    renderMatrixScrollFrame();
  } else {
    applyMatrixSolidColor(gLedColor);
  }
  Serial.printf("[OK] Matrix pins updated to [%s]\n", matrixPinsCsv().c_str());
  return true;
}

bool applyMatrixCounts(const uint16_t newCounts[MATRIX_OUTPUT_COUNT]) {
  String errorCode;
  if (!matrixCountsAreValid(newCounts, gMatrixActiveOutputs, errorCode)) {
    return false;
  }

  bool changed = false;
  for (uint8_t i = 0; i < gMatrixActiveOutputs; i++) {
    if (gMatrixLedsPerOutput[i] != newCounts[i]) {
      changed = true;
      break;
    }
  }
  if (!changed) {
    return true;
  }

  uint16_t previousCounts[MATRIX_OUTPUT_COUNT] = {0};
  for (uint8_t i = 0; i < gMatrixActiveOutputs; i++) {
    previousCounts[i] = gMatrixLedsPerOutput[i];
    gMatrixLedsPerOutput[i] = newCounts[i];
  }

  const bool wasScrollRunning = gMatrixScrollRunning;
  gMatrixTestRunning = false;
  gMatrixScrollRunning = false;

  if (!initMatrix()) {
    for (uint8_t i = 0; i < gMatrixActiveOutputs; i++) {
      gMatrixLedsPerOutput[i] = previousCounts[i];
    }
    if (!initMatrix()) {
      gMatrixReady = false;
    }
    return false;
  }

  if (wasScrollRunning) {
    gMatrixScrollRunning = true;
    gMatrixScrollOffsetX = scrollStartOffsetX(gMatrixScrollDirection, gMatrixScrollText);
    gMatrixScrollLastStepMs = millis();
    renderMatrixScrollFrame();
  } else {
    applyMatrixSolidColor(gLedColor);
  }
  Serial.printf("[OK] Matrix LED counts updated to [%s]\n", matrixCountsCsv().c_str());
  return true;
}

bool applyMatrixActiveOutputs(uint8_t newActiveOutputs, String &errorCode) {
  newActiveOutputs = clampActiveOutputs(static_cast<int>(newActiveOutputs));
  if (newActiveOutputs == gMatrixActiveOutputs) {
    return true;
  }

  if (!matrixPinsAreUnique(gMatrixPins, newActiveOutputs)) {
    errorCode = "duplicate_pins_for_active_outputs";
    return false;
  }
  if (!matrixCountsAreValid(gMatrixLedsPerOutput, newActiveOutputs, errorCode)) {
    return false;
  }

  const uint8_t previousActiveOutputs = gMatrixActiveOutputs;
  gMatrixActiveOutputs = newActiveOutputs;
  gMatrixRuntimeMaxLedCount = detectRuntimeMaxLedCount();

  const bool wasScrollRunning = gMatrixScrollRunning;
  gMatrixTestRunning = false;
  gMatrixScrollRunning = false;

  if (!initMatrix()) {
    gMatrixActiveOutputs = previousActiveOutputs;
    gMatrixRuntimeMaxLedCount = detectRuntimeMaxLedCount();
    if (!initMatrix()) {
      gMatrixReady = false;
    }
    errorCode = "matrix_active_outputs_apply_failed";
    return false;
  }

  if (wasScrollRunning) {
    gMatrixScrollRunning = true;
    gMatrixScrollOffsetX = scrollStartOffsetX(gMatrixScrollDirection, gMatrixScrollText);
    gMatrixScrollLastStepMs = millis();
    renderMatrixScrollFrame();
  } else {
    applyMatrixSolidColor(gLedColor);
  }

  Serial.printf("[OK] Active outputs updated to %u/%u\n",
                static_cast<unsigned>(gMatrixActiveOutputs),
                static_cast<unsigned>(MATRIX_OUTPUT_COUNT));
  return true;
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
  json += "\"safe_mode\":" + String(gSafeMode ? 1 : 0) + ",";
  json += "\"safe_reason\":\"" + jsonEscape(gSafeModeReason) + "\",";
  json += "\"boot_attempts\":" + String(gBootGuardAttempts) + ",";
  json += "\"ap_mode\":" + String(gApMode ? 1 : 0) + ",";
  json += "\"ap_ssid\":\"" + jsonEscape(gApSsid) + "\",";
  json += "\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"matrix_pin\":" + String(gMatrixDataPin) + ",";
  json += "\"matrix_outputs\":" + String(static_cast<unsigned>(MATRIX_OUTPUT_COUNT)) + ",";
  json += "\"matrix_active_outputs\":" + String(static_cast<unsigned>(gMatrixActiveOutputs)) + ",";
  json += "\"matrix_pins\":\"" + matrixPinsCsv() + "\",";
  json += "\"matrix_counts\":\"" + matrixCountsCsv() + "\",";
  json += "\"matrix_scan\":\"" + String(matrixScanOrderToString(gMatrixScanOrder)) + "\",";
  json += "\"matrix_x_flip\":" + String(gMatrixXFlip ? 1 : 0) + ",";
  json += "\"matrix_y_flip\":" + String(gMatrixYFlip ? 1 : 0) + ",";
  json += "\"matrix_width\":" + String(matrixWidth()) + ",";
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
      <span class="label">Matrix pins (CSV)</span>
      <input id="matrixPins" type="text" value="14,17" placeholder="14,13" style="flex:1;min-width:220px;padding:8px;border-radius:8px;border:1px solid #2b3a55;background:#0f1729;color:#e6edf7;">
      <button id="matrixPinsApply">Apply pins</button>
    </div>

    <div class="row">
      <span class="label">Active outputs</span>
      <input id="matrixActiveOutputs" type="number" min="1" max="8" value="2" style="width:110px;padding:8px;border-radius:8px;border:1px solid #2b3a55;background:#0f1729;color:#e6edf7;">
      <button id="matrixActiveOutputsApply">Apply outputs</button>
    </div>

    <div class="row">
      <span class="label">Matrix wiring map</span>
      <select id="matrixScan" style="padding:8px;border-radius:8px;border:1px solid #2b3a55;background:#0f1729;color:#e6edf7;">
        <option value="column">Column serpentine</option>
        <option value="row">Row serpentine</option>
      </select>
    </div>

    <div class="row">
      <span class="label">Mirror</span>
      <label style="display:flex;align-items:center;gap:6px;"><input id="matrixFlipX" type="checkbox"> X</label>
      <label style="display:flex;align-items:center;gap:6px;"><input id="matrixFlipY" type="checkbox"> Y</label>
    </div>

    <div class="row">
      <span class="label">LEDs per output</span>
      <input id="matrixCounts" type="text" value="64,64" placeholder="64,256" style="flex:1;min-width:220px;padding:8px;border-radius:8px;border:1px solid #2b3a55;background:#0f1729;color:#e6edf7;">
      <button id="matrixCountsApply">Apply LED counts</button>
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
    <div class="row" id="recoverRow" style="display:none;">
      <button id="recoverBtn">Exit safe mode + reboot</button>
    </div>

    <div class="status" id="status">Loading...</div>
  </main>

  <script>
    const picker = document.getElementById('picker');
    const statusEl = document.getElementById('status');
    const dot = document.getElementById('dot');
    const brightness = document.getElementById('brightness');
    const brightnessVal = document.getElementById('brightnessVal');
    const matrixPins = document.getElementById('matrixPins');
    const matrixPinsApply = document.getElementById('matrixPinsApply');
    const matrixActiveOutputs = document.getElementById('matrixActiveOutputs');
    const matrixActiveOutputsApply = document.getElementById('matrixActiveOutputsApply');
    const matrixScan = document.getElementById('matrixScan');
    const matrixFlipX = document.getElementById('matrixFlipX');
    const matrixFlipY = document.getElementById('matrixFlipY');
    const matrixCounts = document.getElementById('matrixCounts');
    const matrixCountsApply = document.getElementById('matrixCountsApply');
    const matrixText = document.getElementById('matrixText');
    const matrixScrollMode = document.getElementById('matrixScrollMode');
    const matrixScrollSpeed = document.getElementById('matrixScrollSpeed');
    const matrixScrollSpeedVal = document.getElementById('matrixScrollSpeedVal');
    const matrixScrollDirection = document.getElementById('matrixScrollDirection');
    const segmentsPanel = document.getElementById('segmentsPanel');
    const segmentsList = document.getElementById('segmentsList');
    const addSegment = document.getElementById('addSegment');
    const recoverRow = document.getElementById('recoverRow');
    const recoverBtn = document.getElementById('recoverBtn');
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
      if (document.activeElement !== matrixPins) {
        matrixPins.value = (st.matrix_pins || String(st.matrix_pin || ''));
      }
      matrixActiveOutputs.max = String(st.matrix_outputs || 1);
      if (document.activeElement !== matrixActiveOutputs) {
        matrixActiveOutputs.value = String(st.matrix_active_outputs || 1);
      }
      matrixScan.value = st.matrix_scan || 'column';
      matrixFlipX.checked = !!st.matrix_x_flip;
      matrixFlipY.checked = !!st.matrix_y_flip;
      if (document.activeElement !== matrixCounts) {
        matrixCounts.value = st.matrix_counts || '';
      }
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
      recoverRow.style.display = st.safe_mode ? 'flex' : 'none';
      wifiSsid.value = st.wifi_ssid || '';
      const safePrefix = st.safe_mode
        ? `SAFE MODE (${st.safe_reason || 'unstable boot'}, attempts=${st.boot_attempts}) | `
        : '';
      statusEl.textContent =
        safePrefix + `STA IP: ${st.ip} | Wi-Fi: ${st.wifi} | AP: ${st.ap_mode ? (st.ap_ssid + ' @ ' + st.ap_ip) : 'off'} | DNS: http://${st.hostname}.local | Color: ${st.hex} | Matrix: ${st.matrix_count}/${st.matrix_max_count} LEDs, width=${st.matrix_width}, outputs=${st.matrix_active_outputs || 1}/${st.matrix_outputs || 1} [pins=${st.matrix_pins || st.matrix_pin}] [counts=${st.matrix_counts || '-'}] (${st.matrix_scan || 'column'} map, flipX=${st.matrix_x_flip ? 1 : 0}, flipY=${st.matrix_y_flip ? 1 : 0}) | Brightness: ${st.matrix_brightness} | Scroll: ${st.matrix_scroll ? ('on "' + (st.matrix_scroll_text || '') + '" @ ' + st.matrix_scroll_speed + ' ms / ' + (st.matrix_scroll_direction || 'left') + (st.matrix_scroll_multicolor ? ' / multicolor' : ' / single')) : 'off'}`;
    }

    async function sendColor(hex) {
      await fetch('/api/led?hex=' + encodeURIComponent(hex));
      fetchState();
    }

    async function setMatrixBrightness(value) {
      await fetch('/api/matrix?brightness=' + encodeURIComponent(value));
      fetchState();
    }

    async function setMatrixPins(value) {
      const pins = (value || '').trim();
      if (!pins) {
        alert('Type the GPIO list in CSV format, e.g. 14,13');
        return;
      }
      const res = await fetch('/api/matrix?pins=' + encodeURIComponent(pins));
      const data = await res.json();
      if (!res.ok) {
        alert(data.error || 'Failed to apply matrix pins');
      }
      fetchState();
    }

    async function setMatrixActiveOutputs(value) {
      const outputs = parseInt(value, 10);
      if (Number.isNaN(outputs) || outputs < 1) {
        alert('Invalid number of active outputs');
        return;
      }
      const res = await fetch('/api/matrix?active_outputs=' + encodeURIComponent(outputs));
      const data = await res.json();
      if (!res.ok) {
        alert(data.error || 'Failed to apply active outputs');
      }
      fetchState();
    }

    async function setMatrixScan(value) {
      const mode = (value || '').trim();
      const res = await fetch('/api/matrix?map=' + encodeURIComponent(mode));
      const data = await res.json();
      if (!res.ok) {
        alert(data.error || 'Failed to apply matrix mapping');
      }
      fetchState();
    }

    async function setMatrixFlips() {
      const x = matrixFlipX.checked ? 1 : 0;
      const y = matrixFlipY.checked ? 1 : 0;
      const res = await fetch('/api/matrix?xflip=' + encodeURIComponent(x) + '&yflip=' + encodeURIComponent(y));
      const data = await res.json();
      if (!res.ok) {
        alert(data.error || 'Failed to apply matrix mirror');
      }
      fetchState();
    }

    async function setMatrixCounts(value) {
      const counts = (value || '').trim();
      if (!counts) {
        alert('Type the LED counts in CSV format, e.g. 64,256');
        return;
      }
      const res = await fetch('/api/matrix?counts=' + encodeURIComponent(counts));
      const data = await res.json();
      if (!res.ok) {
        alert(data.error || 'Failed to apply LED counts');
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

    matrixCountsApply.addEventListener('click', async () => {
      await setMatrixCounts(matrixCounts.value);
    });

    matrixCounts.addEventListener('keydown', async ev => {
      if (ev.key === 'Enter') {
        ev.preventDefault();
        await setMatrixCounts(matrixCounts.value);
      }
    });

    matrixPinsApply.addEventListener('click', async () => {
      await setMatrixPins(matrixPins.value);
    });

    matrixPins.addEventListener('keydown', async ev => {
      if (ev.key === 'Enter') {
        ev.preventDefault();
        await setMatrixPins(matrixPins.value);
      }
    });

    matrixActiveOutputsApply.addEventListener('click', async () => {
      await setMatrixActiveOutputs(matrixActiveOutputs.value);
    });

    matrixActiveOutputs.addEventListener('keydown', async ev => {
      if (ev.key === 'Enter') {
        ev.preventDefault();
        await setMatrixActiveOutputs(matrixActiveOutputs.value);
      }
    });

    matrixScan.addEventListener('change', async () => {
      await setMatrixScan(matrixScan.value);
    });

    matrixFlipX.addEventListener('change', async () => {
      await setMatrixFlips();
    });

    matrixFlipY.addEventListener('change', async () => {
      await setMatrixFlips();
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

    recoverBtn.addEventListener('click', async () => {
      statusEl.textContent = 'Clearing safe mode and rebooting...';
      await fetch('/api/recover');
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

void handleApiRecover() {
  clearBootGuard();
  gRecoveryBootToken = kRecoveryBootMagic;
  gSafeMode = false;
  gSafeModeReason = "";
  gWebServer.send(200, "application/json", "{\"ok\":true,\"message\":\"restarting\"}");
  delay(300);
  ESP.restart();
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
  if (gSafeMode) {
    gWebServer.send(503, "application/json", "{\"error\":\"safe_mode_active\"}");
    return;
  }

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

  if (gWebServer.hasArg("map")) {
    MatrixScanOrder nextOrder = gMatrixScanOrder;
    if (!parseMatrixScanOrder(gWebServer.arg("map"), nextOrder)) {
      gWebServer.send(400, "application/json", "{\"error\":\"invalid_matrix_map\"}");
      return;
    }
    if (nextOrder != gMatrixScanOrder) {
      applyMatrixScanOrder(nextOrder);
      savePersistentSettings = true;
    }
    changed = true;
  }

  if (gWebServer.hasArg("xflip") || gWebServer.hasArg("yflip")) {
    bool nextXFlip = gMatrixXFlip;
    bool nextYFlip = gMatrixYFlip;

    if (gWebServer.hasArg("xflip") && !parseBoolArg(gWebServer.arg("xflip"), nextXFlip)) {
      gWebServer.send(400, "application/json", "{\"error\":\"invalid_xflip\"}");
      return;
    }
    if (gWebServer.hasArg("yflip") && !parseBoolArg(gWebServer.arg("yflip"), nextYFlip)) {
      gWebServer.send(400, "application/json", "{\"error\":\"invalid_yflip\"}");
      return;
    }

    if (nextXFlip != gMatrixXFlip || nextYFlip != gMatrixYFlip) {
      applyMatrixFlips(nextXFlip, nextYFlip);
      savePersistentSettings = true;
    }
    changed = true;
  }

  if (gWebServer.hasArg("active_outputs")) {
    String outputsArg = gWebServer.arg("active_outputs");
    outputsArg.trim();
    char *endPtr = nullptr;
    const long outputsVal = strtol(outputsArg.c_str(), &endPtr, 10);
    if (endPtr == outputsArg.c_str() || endPtr == nullptr || *endPtr != '\0') {
      gWebServer.send(400, "application/json", "{\"error\":\"invalid_active_outputs\"}");
      return;
    }
    if (outputsVal < 1 || outputsVal > MATRIX_OUTPUT_COUNT) {
      gWebServer.send(400, "application/json", "{\"error\":\"active_outputs_out_of_range\"}");
      return;
    }

    String outputsError;
    if (!applyMatrixActiveOutputs(static_cast<uint8_t>(outputsVal), outputsError)) {
      if (outputsError.length() == 0) {
        outputsError = "matrix_active_outputs_apply_failed";
      }
      gWebServer.send(500, "application/json", "{\"error\":\"" + outputsError + "\"}");
      return;
    }
    changed = true;
    savePersistentSettings = true;
  }

  if (gWebServer.hasArg("pins")) {
    uint8_t nextPins[MATRIX_OUTPUT_COUNT] = {0};
    String pinError;
    if (!parseMatrixPinsCsv(gWebServer.arg("pins"), gMatrixActiveOutputs, nextPins, pinError)) {
      gWebServer.send(400, "application/json", "{\"error\":\"" + pinError + "\"}");
      return;
    }

    if (!applyMatrixPins(nextPins)) {
      gWebServer.send(500, "application/json", "{\"error\":\"matrix_pin_apply_failed\"}");
      return;
    }
    changed = true;
    savePersistentSettings = true;
  } else if (gWebServer.hasArg("pin")) {
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

    uint8_t nextPins[MATRIX_OUTPUT_COUNT] = {0};
    for (uint8_t i = 0; i < MATRIX_OUTPUT_COUNT; i++) {
      nextPins[i] = gMatrixPins[i];
    }
    nextPins[0] = static_cast<uint8_t>(pinVal);

    if (!matrixPinsAreUnique(nextPins, gMatrixActiveOutputs)) {
      gWebServer.send(400, "application/json", "{\"error\":\"duplicate_pins\"}");
      return;
    }

    if (!applyMatrixPins(nextPins)) {
      gWebServer.send(500, "application/json", "{\"error\":\"matrix_pin_apply_failed\"}");
      return;
    }
    changed = true;
    savePersistentSettings = true;
  }

  if (gWebServer.hasArg("counts")) {
    uint16_t nextCounts[MATRIX_OUTPUT_COUNT] = {0};
    String countsError;
    if (!parseMatrixCountsCsv(gWebServer.arg("counts"), gMatrixActiveOutputs, nextCounts, countsError)) {
      gWebServer.send(400, "application/json", "{\"error\":\"" + countsError + "\"}");
      return;
    }
    if (!matrixCountsAreValid(nextCounts, gMatrixActiveOutputs, countsError)) {
      gWebServer.send(400, "application/json", "{\"error\":\"" + countsError + "\"}");
      return;
    }

    if (!applyMatrixCounts(nextCounts)) {
      gWebServer.send(500, "application/json", "{\"error\":\"matrix_counts_apply_failed\"}");
      return;
    }
    changed = true;
    savePersistentSettings = true;
  } else if (gWebServer.hasArg("count")) {
    gWebServer.send(400, "application/json", "{\"error\":\"use_counts_csv\"}");
    return;
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
  gWebServer.on("/api/recover", HTTP_GET, handleApiRecover);
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

  const esp_reset_reason_t resetReason = esp_reset_reason();
  const bool recoveryBoot = (gRecoveryBootToken == kRecoveryBootMagic);
  if (recoveryBoot) {
    gRecoveryBootToken = 0;
  }
  armBootGuard();
  Serial.printf("[BOOT] reset_reason=%s | boot_attempts=%u\n",
                resetReasonToString(resetReason),
                static_cast<unsigned>(gBootGuardAttempts));
  if (recoveryBoot) {
    Serial.println("[BOOT] Recovery boot requested (diagnostics skipped once).");
  }
  const bool criticalReset =
    (resetReason == ESP_RST_BROWNOUT ||
     resetReason == ESP_RST_PANIC ||
     resetReason == ESP_RST_WDT ||
     resetReason == ESP_RST_TASK_WDT ||
     resetReason == ESP_RST_INT_WDT);
  if (criticalReset ||
      (gBootGuardAttempts >= kBootGuardThreshold &&
       resetReason != ESP_RST_POWERON &&
       resetReason != ESP_RST_DEEPSLEEP)) {
    gSafeMode = true;
    gSafeModeReason = resetReasonToString(resetReason);
    Serial.printf("[SAFE] Entering safe mode (reason=%s, attempts=%u).\n",
                  gSafeModeReason.c_str(),
                  static_cast<unsigned>(gBootGuardAttempts));
  }

#ifdef LED_BUILTIN
  pinMode(kLedPin, OUTPUT);
#endif

  Serial.println();
  printSystemInfo();
  if (!gSafeMode && !recoveryBoot) {
    rgbTest();
    psramPatternTest();
    nvsCounterTest();
  } else {
    Serial.println("[SAFE] Diagnostic stress tests skipped.");
  }

  gMatrixRuntimeMaxLedCount = detectRuntimeMaxLedCount();
  loadDefaultMatrixCounts();
  String bootGeometryError;
  (void)rebuildMatrixGeometry(gMatrixLedsPerOutput, gMatrixActiveOutputs, bootGeometryError);
  Serial.printf("[OK] Limite automatico de LEDs em runtime: %u (teto compilado: %u)\n",
                static_cast<unsigned>(gMatrixRuntimeMaxLedCount),
                static_cast<unsigned>(kMatrixCompiledMaxLedCount));

  loadSettings();
  if (!gSafeMode) {
    if (initMatrix()) {
      applyMatrixSolidColor(gLedColor);
    } else {
      gSafeMode = true;
      gSafeModeReason = "matrix_init_failed";
      gMatrixReady = false;
      gMatrixTestRunning = false;
      gMatrixScrollRunning = false;
      Serial.println("[SAFE] Matrix init failed, entering safe mode.");
    }
  } else {
    gMatrixReady = false;
    gMatrixTestRunning = false;
    gMatrixScrollRunning = false;
    Serial.println("[SAFE] Matrix output disabled for recovery.");
  }

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

  if (!gSafeMode) {
    tickMatrixScroll();
    tickMatrixTest();
  }

  static unsigned long lastPrint = 0;
  const unsigned long now = millis();
  if (!gSafeMode && !gBootMarkedStable && now >= kBootGuardStableMs) {
    clearBootGuard();
    gBootMarkedStable = true;
    Serial.println("[BOOT] Marked as stable, boot guard reset.");
  }

  if (now - lastPrint >= 3000) {
    lastPrint = now;
    Serial.printf(
      "Heartbeat | uptime=%lu ms | heap=%u | psram_free=%u | wifi=%d | ip=%s | led=%s | matrix_br=%u | matrix_test=%d | matrix_scroll=%d | scroll_dir=%s | safe_mode=%d\n",
      now,
      ESP.getFreeHeap(),
      ESP.getFreePsram(),
      static_cast<int>(WiFi.status()),
      WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "0.0.0.0",
      ledHexColor().c_str(),
      gMatrixBrightness,
      gMatrixTestRunning ? 1 : 0,
      gMatrixScrollRunning ? 1 : 0,
      scrollDirectionToString(gMatrixScrollDirection),
      gSafeMode ? 1 : 0);
  }

  delay(10);
}
