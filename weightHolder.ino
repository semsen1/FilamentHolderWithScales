#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HX711.h>
#include <EEPROM.h>
#include <math.h>
#include <stddef.h>

// ---------------- Connections ----------------

const uint8_t PIN_HX_DT  = A0;
const uint8_t PIN_HX_SCK = A1;

const uint8_t PIN_INA_SDA = A2;
const uint8_t PIN_INA_SCL = A3;

const uint8_t PIN_NEW   = 2;
const uint8_t PIN_PLUS  = 3;
const uint8_t PIN_MINUS = 4;
const uint8_t PIN_SLEEP = 5;

const uint8_t LCD_ADDRESS = 0x27;
const uint8_t INA_ADDRESS = 0x44;

// ---------------- Settings ----------------

const float PLATFORM_G = 232.0f;
const uint16_t DEFAULT_FILAMENT_G = 1000;
const uint16_t MAX_FILAMENT_G = 5000;

const unsigned long DISPLAY_TIMEOUT_MS = 30000UL;
const unsigned long LONG_PRESS_MS = 5000UL;
const unsigned long LCD_UPDATE_MS = 250UL;
const unsigned long BATTERY_UPDATE_MS = 3000UL;

// Maximum waiting time for a single HX711 sample to be ready
const unsigned long HX_READY_TIMEOUT_MS = 3000UL;

// Countdown before a regular measurement
const unsigned long MEASURE_COUNTDOWN_MS = 5000UL;

// Repeated attempts to measure the new coil
const uint8_t NEW_COIL_RETRY_COUNT = 3;
const unsigned long NEW_COIL_RETRY_DELAY_MS = 5000UL;
const unsigned long NEW_COIL_LAST_RETRY_DELAY_MS = 10000UL;

const unsigned long HX_SAMPLE_MS = 100UL;

const uint16_t DEBOUNCE_MS = 35;
const uint16_t REPEAT_START_MS = 500;
const uint16_t REPEAT_SLOW_MS = 160;
const uint16_t REPEAT_FAST_MS = 55;

const uint8_t CAL_SAMPLES = 16;
const uint8_t NEW_COIL_SAMPLES = 12;

// The display step and the minimum change in readings
const float DISPLAY_STEP_G = 5.0f;
const float DISPLAY_HYSTERESIS_G = 7.0f;

// For 3S Li-ion: approximate resting voltages
const float BATTERY_LEVEL_1_V = 9.6f;
const float BATTERY_LEVEL_2_V = 10.5f;
const float BATTERY_LEVEL_3_V = 11.1f;
const float BATTERY_LEVEL_4_V = 11.7f;

// If the pressure still reduces totalG after re‑calibration, set it to true.
const bool REVERSE_HX_DIRECTION = false;

// The threshold must be lower than the mass of the empty coil.
const float COIL_PRESENT_THRESHOLD_G = 50.0f;
const float COIL_PRESENCE_HYSTERESIS_G = 20.0f;

enum ButtonIndex {
  BTN_NEW,
  BTN_MINUS,
  BTN_PLUS,
  BTN_SLEEP,
  BTN_COUNT
};

enum Screen {
  SCREEN_HOME,
  SCREEN_NEW_CONFIRM,
  SCREEN_WEIGHT_SELECT,
  SCREEN_CAL_REMOVE,
  SCREEN_CAL_PLATFORM,
  SCREEN_COUNTDOWN,
  SCREEN_MEASURING,
  SCREEN_CAL_ERROR,
  SCREEN_COIL_ERROR,
  SCREEN_HX_TIMEOUT
};

enum MeasureAction {
  ACTION_NONE,
  ACTION_CAL_ZERO,
  ACTION_CAL_PLATFORM,
  ACTION_NEW_COIL,
  ACTION_LIVE_WEIGHT
};

LiquidCrystal_I2C LCD(LCD_ADDRESS, 16, 2);
HX711 scale;

byte batteryCell[8] = {
  0x1f, 0x1f, 0x1f, 0x1f,
  0x1f, 0x1f, 0x1f, 0x1f
};

byte batteryCellEnd[8] = {
  0x1c, 0x1c, 0x1f, 0x1f,
  0x1f, 0x1f, 0x1c, 0x1c
};

// ---------------- EEPROM ----------------

const uint32_t SETTINGS_MAGIC = 0x46494C31UL;
const uint16_t SETTINGS_VERSION = 1;

struct Settings {
  uint32_t magic;
  uint16_t version;
  long zeroRaw;
  float countsPerGram;
  long coilTareG;
  uint16_t filamentStartG;
  uint16_t checksum;
};

Settings settings;
bool calibrated = false;

uint16_t settingsChecksum(const Settings &s) {
  const uint8_t *p = (const uint8_t *)&s;
  const size_t n = offsetof(Settings, checksum);
  uint16_t result = 0x5A3C;

  for (size_t i = 0; i < n; ++i) {
    result = (uint16_t)((result << 5) | (result >> 11));
    result ^= p[i];
  }

  return result;
}

void saveSettings() {
  settings.magic = SETTINGS_MAGIC;
  settings.version = SETTINGS_VERSION;
  settings.checksum = settingsChecksum(settings);

  EEPROM.put(0, settings);
}

void loadSettings() {
  EEPROM.get(0, settings);

  calibrated =
    settings.magic == SETTINGS_MAGIC &&
    settings.version == SETTINGS_VERSION &&
    settings.checksum == settingsChecksum(settings) &&
    isfinite(settings.countsPerGram) &&
    fabs(settings.countsPerGram) > 0.01f &&
    fabs(settings.countsPerGram) < 1000000.0f &&
    settings.filamentStartG <= MAX_FILAMENT_G &&
    settings.coilTareG >= -5000L &&
    settings.coilTareG <= 10000L;

  if (!calibrated) {
    settings.zeroRaw = 0;
    settings.countsPerGram = 1.0f;
    settings.coilTareG = 0;
    settings.filamentStartG = DEFAULT_FILAMENT_G;
  }
}

// ---------------- Butons ----------------

struct Button {
  uint8_t pin;
  bool stableDown;
  bool lastRawDown;
  unsigned long changedAt;
  unsigned long downAt;
  unsigned long repeatAt;
  bool longReported;
  bool wokeDisplay;
  bool shortEvent;
  bool longEvent;
  bool repeatEvent;
};

Button buttons[BTN_COUNT] = {
  {PIN_NEW,   false, false, 0, 0, 0, false, false, false, false, false},
  {PIN_MINUS, false, false, 0, 0, 0, false, false, false, false, false},
  {PIN_PLUS,  false, false, 0, 0, 0, false, false, false, false, false},
  {PIN_SLEEP, false, false, 0, 0, 0, false, false, false, false, false}
};

bool displayAwake = true;
unsigned long lastActivityAt = 0;
bool redraw = true;

void clearButtonEvents() {
  for (uint8_t i = 0; i < BTN_COUNT; ++i) {
    buttons[i].shortEvent = false;
    buttons[i].longEvent = false;
    buttons[i].repeatEvent = false;
  }
}

bool takeShort(ButtonIndex index) {
  const bool value = buttons[index].shortEvent;
  buttons[index].shortEvent = false;
  return value;
}

bool takeLong(ButtonIndex index) {
  const bool value = buttons[index].longEvent;
  buttons[index].longEvent = false;
  return value;
}

bool takeRepeat(ButtonIndex index) {
  const bool value = buttons[index].repeatEvent;
  buttons[index].repeatEvent = false;
  return value;
}

bool buttonDown(ButtonIndex index) {
  return buttons[index].stableDown;
}

void updateButtons() {
  const unsigned long now = millis();

  for (uint8_t i = 0; i < BTN_COUNT; ++i) {
    Button &b = buttons[i];
    const bool rawDown = digitalRead(b.pin) == LOW;

    if (rawDown != b.lastRawDown) {
      b.lastRawDown = rawDown;
      b.changedAt = now;
    }

    if (rawDown != b.stableDown &&
        now - b.changedAt >= DEBOUNCE_MS) {
      b.stableDown = rawDown;

      if (rawDown) {
        b.downAt = now;
        b.repeatAt = now;
        b.longReported = false;
        b.wokeDisplay = !displayAwake;

        if (!displayAwake) {
          displayAwake = true;
          LCD.backlight();
          redraw = true;
        }

        lastActivityAt = now;
      } else {
        if (!b.wokeDisplay && !b.longReported) {
          b.shortEvent = true;
        }

        b.wokeDisplay = false;
        lastActivityAt = now;
      }
    }

    if (!b.stableDown || b.wokeDisplay) {
      continue;
    }

    if (i == BTN_NEW &&
        !b.longReported &&
        now - b.downAt >= LONG_PRESS_MS) {
      b.longReported = true;
      b.longEvent = true;
      lastActivityAt = now;
    }

    if ((i == BTN_MINUS || i == BTN_PLUS) &&
        now - b.downAt >= REPEAT_START_MS) {
      const unsigned long interval =
        (now - b.downAt >= 2000UL)
          ? REPEAT_FAST_MS
          : REPEAT_SLOW_MS;

      if (now - b.repeatAt >= interval) {
        b.repeatAt = now;
        b.repeatEvent = true;
        lastActivityAt = now;
      }
    }
  }
}

// Remove the events and ignore the held buttons until they are released
void suppressButtonsUntilRelease() {
  clearButtonEvents();

  for (uint8_t i = 0; i < BTN_COUNT; ++i) {
    Button &b = buttons[i];

    if (b.stableDown || digitalRead(b.pin) == LOW) {
      b.wokeDisplay = true;
      b.longReported = true;
    }
  }
}

// ---------------- I2C program for INA226 ----------------

void inaSdaLow() {
  digitalWrite(PIN_INA_SDA, LOW);
  pinMode(PIN_INA_SDA, OUTPUT);
}

void inaSdaRelease() {
  pinMode(PIN_INA_SDA, INPUT_PULLUP);
}

void inaSclLow() {
  digitalWrite(PIN_INA_SCL, LOW);
  pinMode(PIN_INA_SCL, OUTPUT);
}

void inaSclRelease() {
  pinMode(PIN_INA_SCL, INPUT_PULLUP);
}

void inaDelay() {
  delayMicroseconds(8);
}

bool inaClockHigh() {
  inaSclRelease();

  const unsigned long start = micros();

  while (digitalRead(PIN_INA_SCL) == LOW) {
    if ((unsigned long)(micros() - start) > 2000UL) {
      return false;
    }
  }

  inaDelay();
  return true;
}

bool inaStart() {
  inaSdaRelease();

  if (!inaClockHigh()) {
    return false;
  }

  inaSdaLow();
  inaDelay();
  inaSclLow();

  return true;
}

void inaStop() {
  inaSdaLow();
  inaDelay();
  inaClockHigh();
  inaSdaRelease();
  inaDelay();
}

bool inaWriteByte(uint8_t value) {
  for (uint8_t i = 0; i < 8; ++i) {
    if (value & 0x80) {
      inaSdaRelease();
    } else {
      inaSdaLow();
    }

    inaDelay();

    if (!inaClockHigh()) {
      return false;
    }

    inaSclLow();
    value <<= 1;
  }

  inaSdaRelease();
  inaDelay();

  if (!inaClockHigh()) {
    return false;
  }

  const bool acknowledged =
    digitalRead(PIN_INA_SDA) == LOW;

  inaSclLow();

  return acknowledged;
}

bool inaReadByte(uint8_t &value, bool sendAck) {
  value = 0;
  inaSdaRelease();

  for (uint8_t i = 0; i < 8; ++i) {
    value <<= 1;

    if (!inaClockHigh()) {
      return false;
    }

    if (digitalRead(PIN_INA_SDA) == HIGH) {
      value |= 1;
    }

    inaSclLow();
    inaDelay();
  }

  if (sendAck) {
    inaSdaLow();
  } else {
    inaSdaRelease();
  }

  inaDelay();

  if (!inaClockHigh()) {
    return false;
  }

  inaSclLow();
  inaSdaRelease();

  return true;
}

bool inaReadRegister(uint8_t reg, uint16_t &value) {
  if (!inaStart()) {
    inaStop();
    return false;
  }

  if (!inaWriteByte((INA_ADDRESS << 1) | 0) ||
      !inaWriteByte(reg)) {
    inaStop();
    return false;
  }

  if (!inaStart()) {
    inaStop();
    return false;
  }

  if (!inaWriteByte((INA_ADDRESS << 1) | 1)) {
    inaStop();
    return false;
  }

  uint8_t hi, lo;

  const bool ok =
    inaReadByte(hi, true) &&
    inaReadByte(lo, false);

  inaStop();

  if (ok) {
    value = ((uint16_t)hi << 8) | lo;
  }

  return ok;
}

bool batteryAvailable = false;
uint8_t batteryLevel = 0;
float batteryVoltage = 0.0f;
unsigned long lastBatteryReadAt = 0;

void updateBattery() {
  const unsigned long now = millis();

  if (now - lastBatteryReadAt < BATTERY_UPDATE_MS) {
    return;
  }

  lastBatteryReadAt = now;

  uint16_t rawBus;

  // INA226: bus voltage, 1 bit = 1.25 mV.
  if (!inaReadRegister(0x02, rawBus)) {
    batteryAvailable = false;
    batteryLevel = 0;
    return;
  }

  batteryVoltage = rawBus * 0.00125f;
  batteryAvailable = true;

  if (batteryVoltage >= BATTERY_LEVEL_4_V) {
    batteryLevel = 4;
  } else if (batteryVoltage >= BATTERY_LEVEL_3_V) {
    batteryLevel = 3;
  } else if (batteryVoltage >= BATTERY_LEVEL_2_V) {
    batteryLevel = 2;
  } else if (batteryVoltage >= BATTERY_LEVEL_1_V) {
    batteryLevel = 1;
  } else {
    batteryLevel = 0;
  }
}

// ---------------- Interface status ----------------

Screen screen = SCREEN_HOME;
MeasureAction pendingAction = ACTION_NONE;

long calibrationZeroRaw = 0;
unsigned long screenEnteredAt = 0;
unsigned long countdownStartedAt = 0;

// The current countdown duration.
// For a normal measurement, it takes 5 seconds.
// For the last retry of a new reel — 10 seconds.
unsigned long countdownDurationMs = MEASURE_COUNTDOWN_MS;

uint16_t selectedFilamentG = DEFAULT_FILAMENT_G;
bool selectingWeight = false;

// The number of repeated attempts already planned for measuring the new coil.
uint8_t newCoilRetryCount = 0;

// ---------------- Weighing ----------------

float filteredTotalG = 0.0f;
bool filterReady = false;

int displayedPlasticG = 0;
bool displayWeightReady = false;
bool coilPresent = false;

unsigned long lastHxSampleAt = 0;

// The moment of the last successfully read value.
// When entering the main screen, the countdown starts over.
unsigned long lastHxReadyAt = 0;

void showHxTimeout(MeasureAction action);

bool readAverageRaw(uint8_t samples, long &result) {
  int64_t sum = 0;

  // The first sample is discarded: during the counting process, a previously prepared value might remain in the HX711.
  // The following samples are averaged.
  for (uint16_t i = 0; i <= samples; ++i) {
    const unsigned long startedAt = millis();

    while (true) {
      updateButtons();
      suppressButtonsUntilRelease();

      if (scale.is_ready()) {
        break;
      }

      if (millis() - startedAt >= HX_READY_TIMEOUT_MS) {
        return false;
      }

      delay(2);
    }

    const long raw = scale.read();
    lastHxReadyAt = millis();

    if (i > 0) {
      sum += raw;
    }
  }

  result = (long)(sum / samples);
  return true;
}

void resetWeightFilter() {
  filterReady = false;
  displayWeightReady = false;
  coilPresent = false;
  displayedPlasticG = 0;
  redraw = true;
}

void updateWeight() {
  if (!calibrated) {
    return;
  }

  const unsigned long now = millis();

  // HX711 monitoring during normal weighing: the old reading should not remain on the screen indefinitely after the sensor is disconnected.
  if (!scale.is_ready()) {
    if (now - lastHxReadyAt >= HX_READY_TIMEOUT_MS) {
      showHxTimeout(ACTION_LIVE_WEIGHT);
    }

    return;
  }

  if (now - lastHxSampleAt < HX_SAMPLE_MS) {
    return;
  }

  lastHxSampleAt = now;

  const long raw = scale.read();
  lastHxReadyAt = millis();

  const float totalG =
    ((float)(raw - settings.zeroRaw)) /
    settings.countsPerGram;

  if (!isfinite(totalG) ||
      totalG < -1000.0f ||
      totalG > 7000.0f) {
    return;
  }

  if (!filterReady) {
    filteredTotalG = totalG;
    filterReady = true;
  } else {
    const float difference = fabs(totalG - filteredTotalG);
    const float alpha =
      difference > 150.0f ? 0.22f : 0.10f;

    filteredTotalG += alpha * (totalG - filteredTotalG);
  }

  const float rawLoadAbovePlatformG =
    totalG - PLATFORM_G;

  if (coilPresent) {
    if (rawLoadAbovePlatformG <
        COIL_PRESENT_THRESHOLD_G -
        COIL_PRESENCE_HYSTERESIS_G) {
      coilPresent = false;
    }
  } else {
    if (rawLoadAbovePlatformG >
        COIL_PRESENT_THRESHOLD_G +
        COIL_PRESENCE_HYSTERESIS_G) {
      coilPresent = true;
    }
  }

  if (!coilPresent) {
    if (!displayWeightReady || displayedPlasticG != 0) {
      displayedPlasticG = 0;
      displayWeightReady = true;
      redraw = true;
    }

    return;
  }

  float plasticG =
    filteredTotalG -
    PLATFORM_G -
    (float)settings.coilTareG;

  if (plasticG < 0.0f) {
    plasticG = 0.0f;
  }

  if (plasticG > settings.filamentStartG) {
    plasticG = settings.filamentStartG;
  }

  int rounded =
    (int)(floor((plasticG / DISPLAY_STEP_G) + 0.5f) *
          DISPLAY_STEP_G);

  if (rounded > settings.filamentStartG) {
    rounded = settings.filamentStartG;
  }

  if (!displayWeightReady ||
      fabs(plasticG - displayedPlasticG) >=
        DISPLAY_HYSTERESIS_G ||
      (rounded == 0 &&
       plasticG < 2.5f &&
       displayedPlasticG != 0)) {
    displayedPlasticG = rounded;
    displayWeightReady = true;
    redraw = true;
  }
}

// ---------------- Interface ----------------

void setScreen(Screen next) {
  if (next == SCREEN_HOME) {
    lastActivityAt = millis();

    // Do not take into account the time spent in the menu
    // when controlling the timeout of a regular weighing
    lastHxReadyAt = millis();
    lastHxSampleAt = millis() - HX_SAMPLE_MS;

    pendingAction = ACTION_NONE;
  }

  screen = next;
  screenEnteredAt = millis();
  redraw = true;
}

void wakeDisplay() {
  if (!displayAwake) {
    displayAwake = true;
    LCD.backlight();
  }

  lastActivityAt = millis();
  redraw = true;
}

void showHxTimeout(MeasureAction action) {
  pendingAction = action;

  suppressButtonsUntilRelease();
  resetWeightFilter();
  wakeDisplay();

  setScreen(SCREEN_HX_TIMEOUT);
}

void showMeasurementError(Screen errorScreen) {
  pendingAction = ACTION_NONE;

  suppressButtonsUntilRelease();
  wakeDisplay();

  setScreen(errorScreen);
}

void printLine(uint8_t row, const char *text) {
  LCD.setCursor(0, row);

  uint8_t i = 0;

  while (text[i] && i < 16) {
    LCD.print(text[i]);
    ++i;
  }

  while (i++ < 16) {
    LCD.print(' ');
  }
}

void showBattery(uint8_t level) {
  LCD.setCursor(12, 1);

  if (level >= 1) LCD.write((uint8_t)0);
  else            LCD.print(' ');

  if (level >= 2) LCD.write((uint8_t)0);
  else            LCD.print(' ');

  if (level >= 3) LCD.write((uint8_t)0);
  else            LCD.print(' ');

  if (level >= 4) LCD.write((uint8_t)1);
  else            LCD.print(' ');
}

uint8_t countdownSecondsLeft() {
  const unsigned long elapsed =
    millis() - countdownStartedAt;

  if (elapsed >= countdownDurationMs) {
    return 0;
  }

  return (uint8_t)(
    (countdownDurationMs - elapsed + 999UL) / 1000UL
  );
}

void drawScreen() {
  char line[17];

  switch (screen) {
    case SCREEN_HOME:
      if (!calibrated) {
        printLine(0, "Calibration");
        printLine(1, "required o-5 sec");
      } else {
        if (displayWeightReady) {
          snprintf(line, sizeof(line),
                   "%dg", displayedPlasticG);
          printLine(0, line);
        } else {
          printLine(0, "Waiting HX711...");
        }

        printLine(1, "");

        if (batteryAvailable) {
          showBattery(batteryLevel);
        } else {
          LCD.setCursor(12, 1);
          LCD.print("----");
        }
      }
      break;

    case SCREEN_NEW_CONFIRM:
      printLine(0, "Install new coil?");
      printLine(1, "+ok -cancel");
      break;

    case SCREEN_WEIGHT_SELECT:
      if (!selectingWeight) {
        printLine(0, "1000g? +ok");
        printLine(1, "o change -cancel");
      } else {
        snprintf(line, sizeof(line),
                 "Start: %ug",
                 (unsigned int)selectedFilamentG);

        printLine(0, line);
        printLine(1, "+/-  o OK");
      }
      break;

    case SCREEN_CAL_REMOVE:
      printLine(0, "Remove platform");
      printLine(1, "+done -cancel");
      break;

    case SCREEN_CAL_PLATFORM:
      printLine(0, "Install platform");
      printLine(1, "+done -cancel");
      break;

    case SCREEN_COUNTDOWN:
      if (pendingAction == ACTION_NEW_COIL &&
          newCoilRetryCount > 0) {
        printLine(0, "New coil retry");
      } else {
        switch (pendingAction) {
          case ACTION_CAL_ZERO:
            printLine(0, "Without platform");
            break;

          case ACTION_CAL_PLATFORM:
            printLine(0, "Platform calib.");
            break;

          case ACTION_NEW_COIL:
            printLine(0, "New coil weight");
            break;

          default:
            printLine(0, "Prepare measure");
            break;
        }
      }

      if (pendingAction == ACTION_NEW_COIL &&
          newCoilRetryCount > 0) {
        snprintf(line, sizeof(line),
                 "Retry %u/%u %us",
                 (unsigned int)newCoilRetryCount,
                 (unsigned int)NEW_COIL_RETRY_COUNT,
                 (unsigned int)countdownSecondsLeft());
      } else {
        snprintf(line, sizeof(line),
                 "Start in %us",
                 (unsigned int)countdownSecondsLeft());
      }

      printLine(1, line);
      break;

    case SCREEN_MEASURING:
      switch (pendingAction) {
        case ACTION_CAL_ZERO:
          printLine(0, "Without platform");
          printLine(1, "Measuring...");
          break;

        case ACTION_CAL_PLATFORM:
          printLine(0, "Measuring");

          snprintf(line, sizeof(line),
                   "%ug platform",
                   (unsigned int)lround(PLATFORM_G));

          printLine(1, line);
          break;

        case ACTION_NEW_COIL:
          printLine(0, "Measuring coil");
          printLine(1, "Please wait...");
          break;

        default:
          printLine(0, "Measuring...");
          printLine(1, "Please wait...");
          break;
      }
      break;

    case SCREEN_CAL_ERROR:
      printLine(0, "Calib. failed");
      printLine(1, "Check / retry");
      break;

    case SCREEN_COIL_ERROR:
      printLine(0, "Coil calc. error");
      printLine(1, "Check weight");
      break;

    case SCREEN_HX_TIMEOUT:
      printLine(0, "HX711 timeout");
      printLine(1, "+retry -back");
      break;
  }

  redraw = false;
}

// ---------------- Counting and measurements ----------------

void startCountdown(
  MeasureAction action,
  unsigned long durationMs = MEASURE_COUNTDOWN_MS
) {
  suppressButtonsUntilRelease();

  pendingAction = action;
  countdownDurationMs = durationMs;
  countdownStartedAt = millis();

  wakeDisplay();
  setScreen(SCREEN_COUNTDOWN);
}

void startNewCoilMeasurement() {
  newCoilRetryCount = 0;
  startCountdown(ACTION_NEW_COIL, MEASURE_COUNTDOWN_MS);
}

//Planning a retry attempt to measure the new coil.
//
// Sequence:
// initial measurement error -> wait 5 seconds -> retry 1
// retry 1 error -> wait 5 seconds -> retry 2
// retry 2 error -> wait 10 seconds -> retry 3
// retry 3 error -> SCREEN_COIL_ERROR
void retryNewCoilMeasurement() {
  if (newCoilRetryCount >= NEW_COIL_RETRY_COUNT) {
    showMeasurementError(SCREEN_COIL_ERROR);
    return;
  }

  ++newCoilRetryCount;

  const unsigned long retryDelay =
    newCoilRetryCount == NEW_COIL_RETRY_COUNT
      ? NEW_COIL_LAST_RETRY_DELAY_MS
      : NEW_COIL_RETRY_DELAY_MS;

  startCountdown(ACTION_NEW_COIL, retryDelay);
}

void startCalibration() {
  calibrationZeroRaw = 0;

  suppressButtonsUntilRelease();
  setScreen(SCREEN_CAL_REMOVE);
}

void measureCalibrationZero() {
  pendingAction = ACTION_CAL_ZERO;

  setScreen(SCREEN_MEASURING);
  drawScreen();

  long raw;

  if (!readAverageRaw(CAL_SAMPLES, raw)) {
    showHxTimeout(ACTION_CAL_ZERO);
    return;
  }

  calibrationZeroRaw = raw;
  pendingAction = ACTION_NONE;

  suppressButtonsUntilRelease();
  setScreen(SCREEN_CAL_PLATFORM);
}

void finishCalibration() {
  pendingAction = ACTION_CAL_PLATFORM;

  setScreen(SCREEN_MEASURING);
  drawScreen();

  long platformRaw;

  if (!readAverageRaw(CAL_SAMPLES, platformRaw)) {
    showHxTimeout(ACTION_CAL_PLATFORM);
    return;
  }

  const float counts =
    ((float)(platformRaw - calibrationZeroRaw)) /
    PLATFORM_G;

  if (!isfinite(counts) ||
      fabs(counts) < 0.01f ||
      fabs(counts) > 1000000.0f) {
    showMeasurementError(SCREEN_CAL_ERROR);
    return;
  }

  settings.zeroRaw = calibrationZeroRaw;

  settings.countsPerGram =
    REVERSE_HX_DIRECTION ? -counts : counts;

  // After the new calibration, the packaging of the old coil is invalid
  settings.coilTareG = 0;
  settings.filamentStartG = DEFAULT_FILAMENT_G;

  saveSettings();

  calibrated = true;

  resetWeightFilter();
  suppressButtonsUntilRelease();
  setScreen(SCREEN_HOME);
}

void confirmNewCoil() {
  pendingAction = ACTION_NEW_COIL;

  setScreen(SCREEN_MEASURING);
  drawScreen();

  long raw;

  // If HX711 fails to produce a result,
  // launch another attempt instead of immediately returning an error.
  if (!readAverageRaw(NEW_COIL_SAMPLES, raw)) {
    retryNewCoilMeasurement();
    return;
  }

  const float totalG =
    ((float)(raw - settings.zeroRaw)) /
    settings.countsPerGram;

  const float tareG =
    totalG -
    PLATFORM_G -
    (float)selectedFilamentG;

  // If the calculation is incorrect, for example due to the holder wobbling,
  // repeat the measurement with an additional pause.
  if (!isfinite(tareG) ||
      tareG < -5000.0f ||
      tareG > 10000.0f) {
    retryNewCoilMeasurement();
    return;
  }

  settings.coilTareG = lround(tareG);
  settings.filamentStartG = selectedFilamentG;

  saveSettings();

  newCoilRetryCount = 0;

  resetWeightFilter();
  suppressButtonsUntilRelease();
  setScreen(SCREEN_HOME);
}

void updateCountdown() {
  if (screen != SCREEN_COUNTDOWN) {
    return;
  }

  suppressButtonsUntilRelease();

  if (millis() - countdownStartedAt <
      countdownDurationMs) {
    return;
  }

  switch (pendingAction) {
    case ACTION_CAL_ZERO:
      measureCalibrationZero();
      break;

    case ACTION_CAL_PLATFORM:
      finishCalibration();
      break;

    case ACTION_NEW_COIL:
      confirmNewCoil();
      break;

    default:
      setScreen(SCREEN_HOME);
      break;
  }
}

void retryHxMeasurement() {
  const MeasureAction action = pendingAction;

  suppressButtonsUntilRelease();

  switch (action) {
    case ACTION_CAL_ZERO:
    case ACTION_CAL_PLATFORM:
      startCountdown(action, MEASURE_COUNTDOWN_MS);
      break;

    case ACTION_NEW_COIL:
      retryNewCoilMeasurement();
      break;

    case ACTION_LIVE_WEIGHT:
    default:
      resetWeightFilter();
      setScreen(SCREEN_HOME);
      break;
  }
}

void changeSelectedWeight(int delta) {
  int value = (int)selectedFilamentG + delta;

  if (value < 0) {
    value = 0;
  }

  if (value > MAX_FILAMENT_G) {
    value = MAX_FILAMENT_G;
  }

  if (selectedFilamentG != (uint16_t)value) {
    selectedFilamentG = (uint16_t)value;
    redraw = true;
  }
}

// ---------------- Interface processing ----------------

void handleUi() {
  if (screen == SCREEN_COUNTDOWN ||
      screen == SCREEN_MEASURING) {
    suppressButtonsUntilRelease();
    return;
  }

  takeShort(BTN_SLEEP);

  if (takeLong(BTN_NEW)) {
    if (screen == SCREEN_HOME) {
      startCalibration();
      return;
    }
  }

  switch (screen) {
    case SCREEN_HOME:
      if (takeShort(BTN_NEW) && calibrated) {
        setScreen(SCREEN_NEW_CONFIRM);
      }
      break;

    case SCREEN_NEW_CONFIRM:
      if (takeShort(BTN_MINUS)) {
        setScreen(SCREEN_HOME);
      } else if (takeShort(BTN_PLUS)) {
        selectedFilamentG = DEFAULT_FILAMENT_G;
        selectingWeight = false;

        setScreen(SCREEN_WEIGHT_SELECT);
      }
      break;

    case SCREEN_WEIGHT_SELECT:
      if (!selectingWeight) {
        if (takeShort(BTN_MINUS)) {
          setScreen(SCREEN_HOME);
        } else if (takeShort(BTN_PLUS)) {
          startNewCoilMeasurement();
        } else if (takeShort(BTN_NEW)) {
          selectingWeight = true;
          redraw = true;
        }
      } else {
        if (takeShort(BTN_NEW)) {
          startNewCoilMeasurement();
          return;
        }

        // Short press: 5 g.
        if (takeShort(BTN_PLUS)) {
          changeSelectedWeight(5);
        }

        if (takeShort(BTN_MINUS)) {
          changeSelectedWeight(-5);
        }

        // Retention: acceleration up to 25 g per repetition
        if (takeRepeat(BTN_PLUS)) {
          changeSelectedWeight(
            millis() - buttons[BTN_PLUS].downAt >= 2000UL
              ? 25
              : 5
          );
        }

        if (takeRepeat(BTN_MINUS)) {
          changeSelectedWeight(
            millis() - buttons[BTN_MINUS].downAt >= 2000UL
              ? -25
              : -5
          );
        }
      }
      break;

    case SCREEN_CAL_REMOVE:
      if (takeShort(BTN_MINUS)) {
        setScreen(SCREEN_HOME);
      } else if (takeShort(BTN_PLUS)) {
        startCountdown(ACTION_CAL_ZERO, MEASURE_COUNTDOWN_MS);
      }
      break;

    case SCREEN_CAL_PLATFORM:
      if (takeShort(BTN_MINUS)) {
        setScreen(SCREEN_HOME);
      } else if (takeShort(BTN_PLUS)) {
        startCountdown(ACTION_CAL_PLATFORM, MEASURE_COUNTDOWN_MS);
      }
      break;

    case SCREEN_HX_TIMEOUT:
      if (takeShort(BTN_MINUS) ||
          takeShort(BTN_NEW)) {
        suppressButtonsUntilRelease();
        setScreen(SCREEN_HOME);
      } else if (takeShort(BTN_PLUS)) {
        retryHxMeasurement();
      }
      break;

    case SCREEN_CAL_ERROR:
    case SCREEN_COIL_ERROR:
      if (takeShort(BTN_MINUS) ||
          takeShort(BTN_NEW) ||
          millis() - screenEnteredAt >= 5000UL) {
        suppressButtonsUntilRelease();
        setScreen(SCREEN_HOME);
      }
      break;

    case SCREEN_COUNTDOWN:
    case SCREEN_MEASURING:
      break;
  }

  // Don’t save unprocessed events for another screen.
  clearButtonEvents();
}

void setup() {
  for (uint8_t i = 0; i < BTN_COUNT; ++i) {
    pinMode(buttons[i].pin, INPUT_PULLUP);
  }

  inaSdaRelease();
  inaSclRelease();

  Wire.begin();

  LCD.init();
  LCD.backlight();

  LCD.createChar(0, batteryCell);
  LCD.createChar(1, batteryCellEnd);

  scale.begin(PIN_HX_DT, PIN_HX_SCK);

  loadSettings();

  lastActivityAt = millis();
  lastHxReadyAt = millis();
  lastHxSampleAt = millis() - HX_SAMPLE_MS;

  lastBatteryReadAt = millis() - BATTERY_UPDATE_MS;

  updateBattery();
  drawScreen();
}


void loop() {
  updateButtons();
  handleUi();

  updateCountdown();

  if (screen == SCREEN_HOME) {
    updateWeight();
  }

  updateBattery();

  const unsigned long now = millis();

  if (screen == SCREEN_HOME &&
      displayAwake &&
      now - lastActivityAt >= DISPLAY_TIMEOUT_MS) {
    displayAwake = false;
    LCD.noBacklight();
  }

  static unsigned long lastLcdUpdateAt = 0;

  if (displayAwake &&
      (redraw ||
       now - lastLcdUpdateAt >= LCD_UPDATE_MS)) {
    drawScreen();
    lastLcdUpdateAt = now;
  }
}