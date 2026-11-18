/*
 * Smart Dual-Mode Optical Food Purity Analyzer  -  BENCH EDITION v2
 * -------------------------------------------------------------------------
 * ESP32 + TCS3448 (AS7343-compatible) | OLED Display | External Push Button
 */

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <Adafruit_AS7343.h>
#include <Adafruit_TCS3448.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ============================== CONFIG ==============================
#define PIN_LED_PASS     26
#define PIN_LED_FAIL     27
#define PIN_BTN_BOOT     0
#define PIN_EXTERNAL_BTN 4  // External push button trigger on GPIO 4

#define DEFAULT_TURMERIC_NORM  0.0621f 
#define DEFAULT_CHALK_BASELINE 0.0538f 

#define AS_GAIN        TCS3448_GAIN_256X
#define AS_ATIME       99
#define AS_ASTEP       899

#define N_AVG          5         
#define SAT_THRESHOLD  62000     
#define MIN_SIGNAL_SUM 80.0f     

#define METRIC_SELECT  1

#define GOOD_PCT       85.0f     
#define FAIL_PCT       60.0f     

// OLED Display Settings (128x64 I2C, Address 0x3C)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
static Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ============================== DATA ==============================
enum Mode : uint8_t { MODE_TURMERIC = 0, MODE_OLIVE = 1 };
enum Action : uint8_t { ACT_TEST, ACT_AVG, ACT_CAL_PURE, ACT_CAL_BLANK };
enum { CH_F1, CH_F2, CH_F3, CH_F4, CH_F5, CH_F6, CH_F7, CH_F8, N_CH };

static const uint8_t MODE_TARGET_CH[2] = {CH_F5, CH_F7};
static const char* MODE_NAME[2]       = {"TURMERIC", "OLIVE"};
static const char* KEY_PURE[2]        = {"pT", "pO"};
static const char* KEY_BLANK[2]       = {"bT", "bO"};

static Adafruit_TCS3448 sensor;
static Preferences       prefs;

static Mode     mode = MODE_TURMERIC;
static bool     sensorOK = false;
static uint32_t sampleCounter = 0;

static float    rawF[N_CH];            
static float    pureRef[2]  = {-1.0f, -1.0f};   
static float    blankRef[2] = {-1.0f, -1.0f};

static char     labelBuf[32] = "-";
static char     lineBuf[32];
static uint8_t  lineLen = 0;
static bool     inLabel = false;

static bool     lastBoot = HIGH;
static uint32_t bootT = 0;
static bool     bootDone = false;

static bool     lastExtBtn = HIGH;
static uint32_t extBtnT = 0;
static bool     extBtnDone = false;

// ============================== STORAGE ==============================
static void loadCalibration() {
  prefs.begin("analyzer", true);
  for (int m = 0; m < 2; m++) {
    pureRef[m]  = prefs.getFloat(KEY_PURE[m], -1.0f);
    blankRef[m] = prefs.getFloat(KEY_BLANK[m], -1.0f);
  }
  prefs.end();
}

static void saveValue(const char* key, float v) {
  prefs.begin("analyzer", false);
  prefs.putFloat(key, v);
  prefs.end();
}

static void resetCalibration() {
  prefs.begin("analyzer", false);
  prefs.clear();
  prefs.end();
  for (int m = 0; m < 2; m++) { pureRef[m] = -1.0f; blankRef[m] = -1.0f; }
  Serial.println(F(">>> Calibration cleared (pure + blank). Calibrate again with c and b."));
}

// ============================== SENSOR ==============================
static bool readFrame(uint16_t *dst) {
  uint16_t buf[TCS3448_CHANNEL_COUNT];
  if (!sensor.readAllChannels(buf)) return false;
  dst[CH_F1] = buf[TCS3448_CHANNEL_F1];
  dst[CH_F2] = buf[TCS3448_CHANNEL_F2];
  dst[CH_F3] = buf[TCS3448_CHANNEL_F3];
  dst[CH_F4] = buf[TCS3448_CHANNEL_F4];
  dst[CH_F5] = buf[TCS3448_CHANNEL_F5];
  dst[CH_F6] = buf[TCS3448_CHANNEL_F6];
  dst[CH_F7] = buf[TCS3448_CHANNEL_F7];
  dst[CH_F8] = buf[TCS3448_CHANNEL_F8];
  return true;
}

static bool acquire(int n) {
  float acc[N_CH] = {0};
  uint16_t tmp[N_CH];
  for (int k = 0; k < n; k++) {
    if (!readFrame(tmp)) return false;
    for (int i = 0; i < N_CH; i++) acc[i] += tmp[i];
  }
  for (int i = 0; i < N_CH; i++) rawF[i] = acc[i] / n;   
  return true;
}

// ============================== MATH ==============================
static float sumAll() {
  float s = 0.0f;
  for (int i = 0; i < N_CH; i++) s += rawF[i];
  return (s < 1.0f) ? 1.0f : s;
}

static float metricSpec(Mode m) {
  return rawF[MODE_TARGET_CH[m]] / sumAll();
}

static float metricBand() {
  return (rawF[CH_F4] + rawF[CH_F5] + rawF[CH_F6] + rawF[CH_F7] + rawF[CH_F8]) / sumAll();
}

static float computeMetric(Mode m) {
  if (METRIC_SELECT == 1 && m == MODE_TURMERIC) return metricBand();
  return metricSpec(m);
}

// ============================== CSV ==============================
static void printHeader() {
  Serial.println(F("CSV,ID,Label,Mode,F1_405,F2_425,F3_475,F4_515,F5_550,F6_640,F7_690,F8_745,Inorm_target,Band_ratio,Metric_used,Purity_Pct,Result,Flags"));
}

// ============================== OLED DISPLAY UPDATE ==============================
static void updateDisplay(const char* statusStr, float purityVal) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  // Row 1: Active Mode
  display.setCursor(0, 0);
  display.print(F("Mode: "));
  display.println(MODE_NAME[mode]);
  
  // Row 2: Status / Result
  display.setCursor(0, 12);
  display.print(F("Status: "));
  display.println(statusStr);
  
  // Row 3: Purity Percentage (Large text)
  display.setCursor(0, 26);
  display.setTextSize(2);
  if (isnan(purityVal)) {
    display.println(F("---.--%"));
  } else {
    display.print(purityVal, 1);
    display.println(F("%"));
  }
  
  // Row 4: Active Label
  display.setTextSize(1);
  display.setCursor(0, 50);
  display.print(F("Lbl: "));
  display.println(labelBuf);
  
  display.display();
}

// ============================== ANALYSIS ==============================
static void runAnalysis(Action act) {
  if (!sensorOK) { 
    Serial.println(F("ERR: sensor not found. Check wiring (SDA/SCL/3V3/GND).")); 
    updateDisplay("ERR: NO SENSOR", NAN);
    return; 
  }

  int n = (act == ACT_TEST) ? 1 : N_AVG;
  if (!acquire(n)) { 
    Serial.println(F("ERR: sensor read failed.")); 
    updateDisplay("READ FAIL", NAN);
    return; 
  }

  sampleCounter++;
  bool saturated = false;
  for (int i = 0; i < N_CH; i++) if (rawF[i] >= SAT_THRESHOLD) saturated = true;

  float total = sumAll();
  bool lowLight = (total < MIN_SIGNAL_SUM);

  float x = computeMetric(mode);
  const char* result = "";
  float purity = NAN;
  bool uncal = (pureRef[mode] < 0.0f);
  bool noBlank = (blankRef[mode] < 0.0f);

  if (act == ACT_CAL_PURE) {
    pureRef[mode] = x;
    saveValue(KEY_PURE[mode], x);
    purity = 100.0f;
    result = "CAL_PURE";
  } else if (act == ACT_CAL_BLANK) {
    blankRef[mode] = x;
    saveValue(KEY_BLANK[mode], x);
    purity = 0.0f;
    result = "CAL_BLANK";
  } else if (uncal) {
    result = "UNCALIBRATED";
  } else {
    float blank = noBlank ? 0.0f : blankRef[mode];
    float span  = pureRef[mode] - blank;
    if (fabsf(span) < 1e-6f) {
      result = "NO_SPAN";                
    } else {
      purity = (x - blank) / span * 100.0f;
      if (purity > 100.0f) purity = 100.0f;
      if (purity < 0.0f)   purity = 0.0f;
      if (purity >= GOOD_PCT)      result = "GOOD";
      else if (purity >= FAIL_PCT) result = "MODERATE";
      else                         result = "ADULTERATED";
    }
  }

  // LEDs: green = GOOD, red = ADULTERATED, both = MODERATE
  bool good = (strcmp(result, "GOOD") == 0);
  bool bad  = (strcmp(result, "ADULTERATED") == 0);
  bool mid  = (strcmp(result, "MODERATE") == 0);
  digitalWrite(PIN_LED_PASS, (good || mid) ? HIGH : LOW);
  digitalWrite(PIN_LED_FAIL, (bad  || mid) ? HIGH : LOW);

  // Update OLED screen
  updateDisplay(result, purity);

  // CSV line
  Serial.print(F("CSV,"));  Serial.print(sampleCounter);
  Serial.print(',');        Serial.print(labelBuf);
  Serial.print(',');        Serial.print(MODE_NAME[mode]);
  for (int i = 0; i < N_CH; i++) { Serial.print(','); Serial.print(rawF[i], 1); }
  Serial.print(',');        Serial.print(metricSpec(mode), 5);
  Serial.print(',');        Serial.print(metricBand(), 5);
  Serial.print(',');        Serial.print(x, 5);
  Serial.print(',');
  if (isnan(purity)) Serial.print(F("NA")); else Serial.print(purity, 1);
  Serial.print(',');        Serial.print(result);
  Serial.print(',');
  Serial.print(saturated ? "SATURATED" : "OK");
  if (lowLight) Serial.print("|LOWLIGHT");
  if (!uncal && noBlank && act != ACT_CAL_PURE && act != ACT_CAL_BLANK) Serial.print("|NO_BLANK");
  Serial.println();

  // Human-readable summary
  Serial.print(F(">>> ")); Serial.print(labelBuf);
  Serial.print(F(" | metric = ")); Serial.print(x, 5);
  if (act == ACT_CAL_PURE)       Serial.println(F("  -> SAVED as PURE reference (100 %)"));
  else if (act == ACT_CAL_BLANK) Serial.println(F("  -> SAVED as BLANK / chalk baseline (0 %)"));
  else if (uncal)                Serial.println(F("  -> not calibrated: put PURE sample and press c"));
  else if (isnan(purity))        Serial.println(F("  -> pure and blank readings are identical, cannot compute purity"));
  else { Serial.print(F("  purity = ")); Serial.print(purity, 1); Serial.print(F(" %  -> ")); Serial.println(result); }

  if (saturated) Serial.println(F(">>> WARNING: saturated. Move the torch back or lower AS_GAIN, then recalibrate."));
  if (lowLight)  Serial.println(F(">>> WARNING: very little light. Empty holder or torch off?"));
  if (!uncal && noBlank && act != ACT_CAL_PURE && act != ACT_CAL_BLANK)
    Serial.println(F(">>> NOTE: no blank stored. Put 100 % chalk and press b for a proper 0 % point."));
}

// ============================== SETUP / LOOP ==============================
void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(PIN_LED_PASS, OUTPUT); digitalWrite(PIN_LED_PASS, LOW);
  pinMode(PIN_LED_FAIL, OUTPUT); digitalWrite(PIN_LED_FAIL, LOW);
  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);
  pinMode(PIN_EXTERNAL_BTN, INPUT_PULLUP); // External push button trigger on GPIO 4

  // Initialize I2C on explicit ESP32 pins (GPIO 21 = SDA, GPIO 22 = SCL)
  Wire.begin(21, 22);   

  Serial.println(F("\n=================================================="));
  Serial.println(F("  FOOD PURITY ANALYZER - BENCH EDITION v2 + OLED"));
  Serial.println(F("=================================================="));

  // Initialize OLED (Address 0x3C)
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("[ERR] SSD1306 allocation failed (check SDA/SCL/Power)"));
  } else {
    Serial.println(F("[OK] SSD1306 initialized"));
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("PURE-SPEC Ready"));
    display.setCursor(0, 16);
    display.println(F("Waiting for test..."));
    display.display();
  }

  sensorOK = sensor.begin();
  if (sensorOK) {
    sensor.setATIME(AS_ATIME);
    sensor.setASTEP(AS_ASTEP);
    sensor.setGain(AS_GAIN);
    sensor.setSMUXMode(TCS3448_SMUX_18CH);
    sensor.stopMeasurement();
    Serial.println(F("[OK] TCS3448 initialised"));
  } else {
    Serial.println(F("[ERR] TCS3448 not detected (check 3V3, GND, SDA, SCL)"));
  }

  loadCalibration();
  Serial.print(F("Turmeric PURE  ref: "));
  if (pureRef[0] < 0) Serial.println(F("not set")); else Serial.println(pureRef[0], 5);
  Serial.print(F("Turmeric BLANK ref: "));
  if (blankRef[0] < 0) Serial.println(F("not set")); else Serial.println(blankRef[0], 5);

  Serial.println(F("\nCommands: t=test  a=average test  c=cal PURE  b=cal BLANK(chalk)"));
  Serial.println(F("          r=reset  m=mode  h=header  =label (then Enter)"));
  Serial.println(F("Order: 1) chalk -> b   2) pure turmeric -> c   3) test samples with 'a' or Push Button\n"));
  printHeader();
}

void loop() {
  while (Serial.available() > 0) {
    char c = Serial.read();

    if (inLabel) {                             
      if (c == '\n' || c == '\r') {
        if (lineLen > 0) {
          lineBuf[lineLen] = 0;
          strncpy(labelBuf, lineBuf, sizeof(labelBuf) - 1);
          labelBuf[sizeof(labelBuf) - 1] = 0;
        }
        inLabel = false;
        Serial.print(F(">>> Label set to: ")); Serial.println(labelBuf);
      } else if (lineLen < sizeof(lineBuf) - 1 && c != ',') {
        lineBuf[lineLen++] = c;
      }
      continue;
    }

    switch (c) {
      case '=': inLabel = true; lineLen = 0; break;
      case 't': case 'T': runAnalysis(ACT_TEST);      break;
      case 'a': case 'A': runAnalysis(ACT_AVG);        break;
      case 'c': case 'C': runAnalysis(ACT_CAL_PURE);   break;
      case 'b': case 'B': runAnalysis(ACT_CAL_BLANK);  break;
      case 'r': case 'R': resetCalibration();          break;
      case 'h': case 'H': printHeader();               break;
      case 'm': case 'M':
        mode = (mode == MODE_TURMERIC) ? MODE_OLIVE : MODE_TURMERIC;
        Serial.print(F(">>> Mode: ")); Serial.println(MODE_NAME[mode]);
        break;
      default: break;                                  
    }
  }

  // BOOT button handling (GPIO 0)
  bool b = digitalRead(PIN_BTN_BOOT);
  if (b == LOW && lastBoot == HIGH) { bootT = millis(); bootDone = false; }
  if (b == LOW && !bootDone && millis() - bootT > 50) { runAnalysis(ACT_AVG); bootDone = true; }
  lastBoot = b;

  // External Metal Push Button handling (GPIO 4)
  bool extB = digitalRead(PIN_EXTERNAL_BTN);
  if (extB == LOW && lastExtBtn == HIGH) { extBtnT = millis(); extBtnDone = false; }
  if (extB == LOW && !extBtnDone && millis() - extBtnT > 50) { 
    runAnalysis(ACT_AVG); // Triggers average test on button press
    extBtnDone = true; 
  }
  lastExtBtn = extB;
}