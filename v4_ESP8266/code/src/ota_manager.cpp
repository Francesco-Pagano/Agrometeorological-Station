#include "ota_manager.h"
#include "config.h"
#include <ArduinoOTA.h>

void otaBegin() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();
}

void otaHandle() {
  ArduinoOTA.handle();
}
