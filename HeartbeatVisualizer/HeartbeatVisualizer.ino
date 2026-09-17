/*
  MR60BHA2 heartbeat visualiser for Seeed Studio XIAO ESP32C6.

  Required libraries (Arduino Library Manager / Seeed GitHub repo):
    - Adafruit NeoPixel
    - Seeed Arduino mmWave

  The sensor LED is a WS2812 on D1.  Open Serial Monitor at 115200 baud to
  inspect the raw and corrected readings while tuning the installation.
*/

#include <Arduino.h>
#include <HardwareSerial.h>
#include <Adafruit_NeoPixel.h>
#include "Seeed_Arduino_mmWave.h"

// ---- User-adjustable settings ------------------------------------------------
// The MR60BHA2 tends to read about 10 BPM high. Change this value to calibrate.
const float HEART_RATE_REDUCTION_BPM = 10.0f;
// The MR60BHA2 library reports its heart/breath distance in centimetres.
// The short-range distance estimate can read lower than the physical distance,
// so accept 20 cm to 130 cm (nominally 0.20 m to 1.30 m).
const float MIN_DETECTION_DISTANCE_CM = 20.0f;
const float MAX_DETECTION_DISTANCE_CM = 130.0f;

// Pin D1 is GPIO1 on the XIAO ESP32C6 / MR60BHA2 kit.
const uint8_t LED_PIN = D1;

// ---- Pattern specification ---------------------------------------------------
const uint16_t BRIGHT_FLASH_MS = 100;
const uint16_t BETWEEN_FLASHES_MS = 70;
const uint16_t SMALL_FLASH_MS = 100;
const uint8_t BRIGHTNESS_BRIGHT = 255;
const uint8_t BRIGHTNESS_SMALL = 128;
const uint8_t BRIGHTNESS_GLOW = 4;

const uint32_t CATEGORY_PERIODS_MS[] = {1050, 850, 666, 450};
const uint8_t CATEGORY_COUNT = sizeof(CATEGORY_PERIODS_MS) / sizeof(CATEGORY_PERIODS_MS[0]);
const uint8_t INITIAL_READINGS_REQUIRED = 3;
const uint8_t DEFAULT_START_CATEGORY = 1; // Category 2: 850 ms
const uint8_t ABSENT_READINGS_REQUIRED = 2;
const uint8_t CATEGORY_CHANGE_READINGS_REQUIRED = 4; // "more than 3"
const uint32_t CATEGORY_RAMP_MS = 3000;

HardwareSerial mmWaveSerial(0);
SEEED_MR60BHA2 mmWave;
Adafruit_NeoPixel pixel(1, LED_PIN, NEO_GRB + NEO_KHZ800);

bool active = false;
bool gatheringInitialReadings = false;
uint8_t initialCategories[INITIAL_READINGS_REQUIRED];
uint8_t initialCount = 0;
uint8_t currentCategory = 0;
uint8_t pendingCategory = 0;
uint8_t pendingCount = 0;
uint8_t missingReadings = 0;
float latestDistance = 0.0f;
bool haveDistance = false;

uint32_t patternStartedAt = 0;
uint32_t rampStartedAt = 0;
uint32_t rampFromPeriod = CATEGORY_PERIODS_MS[0];
uint32_t rampToPeriod = CATEGORY_PERIODS_MS[0];
bool ramping = false;
uint8_t lastLedBrightness = 255; // force the first LED update
uint32_t lastStatusAt = 0;

uint8_t categoryForBpm(float bpm) {
  if (bpm >= 50 && bpm <= 60) return 0;
  if (bpm <= 80) return 1;     // 61..80
  if (bpm <= 100) return 2;    // 81..100
  if (bpm <= 180) return 3;    // 101..180
  return 255;                  // invalid / outside the requested range
}

// Median is used for the first three readings so one odd initial reading does
// not select the starting category.
uint8_t medianOfInitialCategories() {
  uint8_t a = initialCategories[0];
  uint8_t b = initialCategories[1];
  uint8_t c = initialCategories[2];
  if (a > b) { uint8_t t = a; a = b; b = t; }
  if (b > c) { uint8_t t = b; b = c; c = t; }
  if (a > b) { uint8_t t = a; a = b; b = t; }
  return b;
}

uint32_t currentPatternPeriod(uint32_t now) {
  if (!ramping) return CATEGORY_PERIODS_MS[currentCategory];

  const uint32_t elapsed = now - rampStartedAt;
  if (elapsed >= CATEGORY_RAMP_MS) {
    ramping = false;
    return rampToPeriod;
  }

  // Integer interpolation avoids floating-point timing jitter on the MCU.
  const int32_t change = (int32_t)rampToPeriod - (int32_t)rampFromPeriod;
  return (uint32_t)((int32_t)rampFromPeriod + change * (int32_t)elapsed / (int32_t)CATEGORY_RAMP_MS);
}

void setOrange(uint8_t brightness) {
  if (brightness == lastLedBrightness) return;
  // Orange at the requested intensity: red is the intensity, green is 25%.
  pixel.setPixelColor(0, pixel.Color(brightness, brightness / 4, 0));
  pixel.show();
  lastLedBrightness = brightness;
}

void stopPattern() {
  active = false;
  gatheringInitialReadings = false;
  initialCount = 0;
  pendingCount = 0;
  ramping = false;
  setOrange(0);
}

void transitionToCategory(uint8_t category, uint32_t now) {
  if (category == currentCategory) return;

  rampFromPeriod = currentPatternPeriod(now);
  rampToPeriod = CATEGORY_PERIODS_MS[category];
  rampStartedAt = now;
  ramping = true;
  currentCategory = category;
  pendingCount = 0;
  Serial.printf("Changing to category %u over 3 seconds\n", category + 1);
}

void registerAbsentReading() {
  if (++missingReadings >= ABSENT_READINGS_REQUIRED) stopPattern();
}

void beginPattern(uint8_t category, uint32_t now) {
  active = true;
  currentCategory = category;
  pendingCount = 0;
  ramping = false;
  patternStartedAt = now;
  setOrange(BRIGHTNESS_BRIGHT);
  Serial.printf("Heartbeat display started: category %u (%lu ms)\n",
                category + 1, (unsigned long)CATEGORY_PERIODS_MS[category]);
}

void beginDefaultPattern(uint32_t now) {
  initialCount = 0;
  gatheringInitialReadings = true;
  beginPattern(DEFAULT_START_CATEGORY, now);
  Serial.println("Using Category 2 while initial heart-rate readings are gathered");
}

void requestCategory(uint8_t category, uint32_t now) {
  if (category == currentCategory) {
    pendingCount = 0;
    return;
  }

  if (category != pendingCategory) {
    pendingCategory = category;
    pendingCount = 1;
    return;
  }

  if (++pendingCount < CATEGORY_CHANGE_READINGS_REQUIRED) return;

  // Four consecutive readings in the new category are required. Then blend the
  // repeat period for three seconds, preserving the fixed two-flash timings.
  transitionToCategory(category, now);
}

void handleValidReading(float rawBpm, float distance, uint32_t now) {
  const float correctedBpm = rawBpm - HEART_RATE_REDUCTION_BPM;
  const uint8_t category = categoryForBpm(correctedBpm);

  Serial.printf("distance: %.0f cm (%.2f m), raw: %.1f BPM, corrected: %.1f BPM\n",
                distance, distance / 100.0f, rawBpm, correctedBpm);

  // A measurement outside the supplied BPM categories is ignored rather than
  // letting one implausible spike alter the display.
  if (category == 255) return;

  if (gatheringInitialReadings) {
    initialCategories[initialCount++] = category;
    if (initialCount == INITIAL_READINGS_REQUIRED) {
      gatheringInitialReadings = false;
      // The initial three readings establish the real category. Unlike later
      // changes, they are sufficient by themselves because the default pattern
      // is only a temporary response while the sensor settles.
      transitionToCategory(medianOfInitialCategories(), now);
    }
    return;
  }

  requestCategory(category, now);
}

void updatePattern(uint32_t now) {
  if (!active) return;

  const uint32_t period = currentPatternPeriod(now);
  // Keep the beat cadence stable even if loop() is briefly delayed.
  const uint32_t elapsed = now - patternStartedAt;
  if (elapsed >= period) {
    patternStartedAt += (elapsed / period) * period;
  }

  const uint32_t phase = now - patternStartedAt;
  if (phase < BRIGHT_FLASH_MS) {
    setOrange(BRIGHTNESS_BRIGHT);
  } else if (phase < BRIGHT_FLASH_MS + BETWEEN_FLASHES_MS) {
    setOrange(0);
  } else if (phase < BRIGHT_FLASH_MS + BETWEEN_FLASHES_MS + SMALL_FLASH_MS) {
    setOrange(BRIGHTNESS_SMALL);
  } else {
    setOrange(BRIGHTNESS_GLOW);
  }
}

void pollSensor(uint32_t now) {
  // Do not block the LED state machine while waiting for the radar's next
  // packet. Heart and distance packets arrive much less often than an LED
  // pattern repeats (and not necessarily together).
  if (!mmWave.update(0)) return;

  float distance = 0.0f;
  float rawBpm = 0.0f;
  const bool gotDistance = mmWave.getDistance(distance);
  const bool gotHeartRate = mmWave.getHeartRate(rawBpm);

  // The library delivers distance and heart-rate as separate UART packets. Keep
  // the newest distance so a following heart-rate packet can use it; do not
  // mistake an otherwise unrelated packet for a missing target.
  if (gotDistance) {
    latestDistance = distance;
    haveDistance = true;
    if (distance < MIN_DETECTION_DISTANCE_CM ||
        distance > MAX_DETECTION_DISTANCE_CM) {
      registerAbsentReading();
    } else {
      missingReadings = 0;
      // Give immediate feedback on entry. The actual category will replace
      // this default after the first three valid heart-rate readings arrive.
      if (!active) beginDefaultPattern(now);
    }
  }

  if (!gotHeartRate) return;

  // Distance and heart rate have independent reporting intervals. Once an
  // in-range distance is known, retain it until the radar supplies a newer
  // distance reading. This keeps the flash cadence continuous between samples.
  const bool inZone = haveDistance &&
                      latestDistance >= MIN_DETECTION_DISTANCE_CM &&
                      latestDistance <= MAX_DETECTION_DISTANCE_CM;
  if (!inZone) {
    // Do not count a heart-rate packet as an absent target. Leaving is decided
    // exclusively by two consecutive new out-of-range distance readings.
    return;
  }

  missingReadings = 0;
  handleValidReading(rawBpm, latestDistance, now);
}

void setup() {
  Serial.begin(115200);
  pixel.begin();
  pixel.clear();
  pixel.show();
  lastLedBrightness = 0;

  // The MR60BHA2 connects to GPIO17 (RX) and GPIO16 (TX) on this kit.
  // mmWave.begin() initialises its parser and UART buffer; configure the pins
  // afterwards so they cannot depend on the ESP32 core's UART0 defaults.
  mmWave.begin(&mmWaveSerial);
  mmWaveSerial.begin(115200, SERIAL_8N1, 17, 16);

  // Visible at every reset/upload: confirms that the integrated D1 WS2812 is
  // alive even before the sensor has supplied three valid heart-rate readings.
  setOrange(BRIGHTNESS_SMALL);
  delay(150);
  setOrange(0);

  Serial.println("MR60BHA2 heartbeat visualiser ready");
  Serial.println("Waiting for readings in the 20 cm to 130 cm zone...");
}

void loop() {
  const uint32_t now = millis();
  pollSensor(now);
  updatePattern(now);

  // This proves the sketch is running even when the radar has not yet emitted
  // a complete heart-rate measurement (those can take several seconds).
  if (now - lastStatusAt >= 2000) {
    lastStatusAt = now;
    Serial.printf("status: %s, latest distance: %.0f cm (%.2f m)\n",
                  active ? "displaying" : "waiting", latestDistance,
                  latestDistance / 100.0f);
  }
}
