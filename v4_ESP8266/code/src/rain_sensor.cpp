#include "rain_sensor.h"
#include "config.h"

static volatile unsigned long rainClicks = 0;
static volatile unsigned long lastRainTime = 0;
static const unsigned long RAIN_DEBOUNCE = 200;

static void ICACHE_RAM_ATTR handleRainInterrupt() {
  unsigned long currentTime = millis();
  if (currentTime - lastRainTime > RAIN_DEBOUNCE) {
    rainClicks++;
    lastRainTime = currentTime;
  }
}

void rainSensorBegin() {
  pinMode(RAIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(RAIN_PIN), handleRainInterrupt, FALLING);
}

float readAndResetRainMM() {
  noInterrupts();
  unsigned long clicks = rainClicks;
  rainClicks = 0;
  interrupts();
  return clicks * BUCKET_SIZE;
}
