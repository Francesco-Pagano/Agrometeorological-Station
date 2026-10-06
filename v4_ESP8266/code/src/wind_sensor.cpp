#include "wind_sensor.h"
#include "config.h"

static volatile unsigned long windClicks = 0;
static volatile unsigned long gustClicks = 0;
static volatile unsigned long lastWindClickTime = 0;
static const unsigned long WIND_DEBOUNCE = 10;

static unsigned long lastGustCalcTime = 0;
static float maxGustKmh = 0.0;

static void ICACHE_RAM_ATTR handleWindSpeedInterrupt() {
  unsigned long currentTime = millis();
  if (currentTime - lastWindClickTime > WIND_DEBOUNCE) {
    windClicks++;
    gustClicks++;
    lastWindClickTime = currentTime;
  }
}

void windSensorBegin() {
  pinMode(WIND_SPEED_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(WIND_SPEED_PIN), handleWindSpeedInterrupt, FALLING);
  lastGustCalcTime = millis();
}

void windSensorUpdateGust() {
  unsigned long now = millis();
  if (now - lastGustCalcTime >= GUST_CALC_INTERVAL) {
    noInterrupts();
    unsigned long currentGustClicks = gustClicks;
    gustClicks = 0;
    interrupts();

    float currentGust = ((float)currentGustClicks / (GUST_CALC_INTERVAL / 1000.0)) * WIND_KMH_PER_CLICK_PER_SEC;
    if (currentGust > maxGustKmh) {
      maxGustKmh = currentGust;
    }
    lastGustCalcTime = now;
  }
}

float readAndResetWindSpeed(unsigned long elapsedMillis) {
  noInterrupts();
  unsigned long clicks = windClicks;
  windClicks = 0;
  interrupts();

  float elapsedSeconds = elapsedMillis / 1000.0;
  float rps = (elapsedSeconds > 0) ? (float)clicks / elapsedSeconds : 0;
  return rps * WIND_KMH_PER_CLICK_PER_SEC;
}

float readAndResetMaxGust() {
  float g = maxGustKmh;
  maxGustKmh = 0.0;
  return g;
}

int getWindDirectionDegrees(int adc) {
  if (adc >= 40 && adc <= 150) return 0;    // Nord (N)
  if (adc >= 160 && adc <= 240) return 45;  // Nord-Est (NE)
  if (adc >= 250 && adc <= 350) return 90;  // Est (E)
  if (adc >= 380 && adc <= 450) return 315; // Nord-Ovest (NW)
  if (adc >= 500 && adc <= 580) return 135; // Sud-Est (SE)
  if (adc >= 590 && adc <= 645) return 270; // Ovest (W)
  if (adc >= 650 && adc <= 695) return 225; // Sud-Ovest (SW)
  if (adc >= 700 && adc <= 750) return 180; // Sud (S)
  return -1; // Lettura instabile
}
