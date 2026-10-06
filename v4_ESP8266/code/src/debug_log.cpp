#include "debug_log.h"
#include "config.h"
#include "mqtt_manager.h"
#include <time.h>

static String debugLog = "";

void debugLogBegin() {
  Serial.begin(115200);
  delay(200);
}

void logMsg(const String &msg) {
  Serial.println(msg);

  debugLog += msg + "\n";
  if (debugLog.length() > DEBUG_LOG_MAX_CHARS) {
    debugLog = debugLog.substring(debugLog.length() - DEBUG_LOG_MAX_CHARS);
  }

  if (mqttClient.connected()) {
    mqttClient.publish(DEBUG_TOPIC, msg.c_str());
  }
}

const String &getDebugLog() {
  return debugLog;
}

String epochToUtcString(unsigned long epoch) {
  time_t rawtime = (time_t)epoch;
  struct tm *ti = gmtime(&rawtime);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", ti);
  return String(buf);
}
