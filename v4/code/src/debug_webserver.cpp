#include "debug_webserver.h"
#include "config.h"
#include "debug_log.h"
#include "offline_storage.h"
#include "mqtt_manager.h"
#include "bme_sensor.h"
#include "wifi_time.h"
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>

static ESP8266WebServer debugServer(80);

static void handleRoot() {
  unsigned long now = millis();
  unsigned long epoch = getEpochTime();

  String html = "<html><head><meta charset='utf-8'>";
  html += "<meta http-equiv='refresh' content='15'>";
  html += "<style>body{font-family:monospace;background:#111;color:#0f0;padding:10px}";
  html += "h2{color:#6cf}pre{white-space:pre-wrap;background:#000;padding:8px;border:1px solid #333}</style>";
  html += "</head><body>";
  html += "<h2>Debug</h2>";
  html += "<b>Uptime:</b> " + String(now / 1000) + " s<br>";
  html += "<b>WiFi:</b> " + String(WiFi.status() == WL_CONNECTED ? "connesso" : "DISCONNESSO") + " (" + WiFi.localIP().toString() + ")<br>";
  html += "<b>MQTT:</b> " + String(mqttClient.connected() ? "connesso" : "DISCONNESSO") + "<br>";
  html += "<b>LittleFS:</b> " + String(storageIsMounted() ? "montato" : "NON montato") + "<br>";
  html += "<b>BME680:</b> " + String(bmeSensorFound() ? "trovato" : "non trovato") + "<br>";
  html += "<b>Righe offline in coda:</b> " + String(countOfflineLines()) + "<br>";
  html += "<hr><h2>Timestamp</h2>";
  html += "<b>Epoch (raw):</b> " + String(epoch) + "<br>";
  html += "<b>Date:</b> " + epochToUtcString(epoch) + "<br>";
  html += "<hr><h2>Log</h2><pre>" + getDebugLog() + "</pre>";
  html += "<hr><p><a href='/offline'>File offline_data.jsonl</a></p>";
  html += "</body></html>";

  debugServer.send(200, "text/html", html);
}

static void handleOfflineFile() {
  if (!storageIsMounted() || !LittleFS.exists(OFFLINE_DATA_FILE)) {
    debugServer.send(200, "text/plain", "(file offline non presente)");
    return;
  }
  File f = LittleFS.open(OFFLINE_DATA_FILE, "r");
  if (!f) {
    debugServer.send(500, "text/plain", "Errore apertura file");
    return;
  }
  debugServer.streamFile(f, "text/plain");
  f.close();
}

void debugWebServerBegin() {
  debugServer.on("/", handleRoot);
  debugServer.on("/offline", handleOfflineFile);
  debugServer.begin();
  logMsg("[HTTP] Debug server avviato su http://" + WiFi.localIP().toString() + "/");
}

void debugWebServerHandle() {
  debugServer.handleClient();
}
