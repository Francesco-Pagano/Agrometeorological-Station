#include "bme_sensor.h"
#include "debug_log.h"
#include <Adafruit_BME680.h>
#include <Adafruit_Sensor.h>

static Adafruit_BME680 bme;
static bool bmeFound = false;

bool bmeSensorBegin() {
  if (!bme.begin(0x76)) {
    if (bme.begin(0x77)) {
      bmeFound = true;
    }
  } else {
    bmeFound = true;
  }

  logMsg(bmeFound ? "[BME680] Sensore trovato." : "[BME680] Sensore NON trovato.");

  if (bmeFound) {
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150);
  }

  return bmeFound;
}

bool bmeSensorFound() {
  return bmeFound;
}

bool bmeSensorRead(float &temperature, float &humidity, float &pressure, float &gas) {
  if (!bmeFound || !bme.performReading()) return false;
  temperature = bme.temperature;
  humidity = bme.humidity;
  pressure = bme.pressure / 100.0;
  gas = bme.gas_resistance / 1000.0;
  return true;
}
