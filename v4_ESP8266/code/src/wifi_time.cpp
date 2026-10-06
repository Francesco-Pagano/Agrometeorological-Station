#include "wifi_time.h"
#include "config.h"
#include "debug_log.h"
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

static WiFiUDP ntpUDP;
static NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 21600000);

void wifiTimeBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connessione");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  logMsg("[WiFi] OK, IP: " + WiFi.localIP().toString());

  timeClient.begin();
}

void timeUpdate() {
  timeClient.update();
}

unsigned long getEpochTime() {
  return timeClient.getEpochTime();
}
