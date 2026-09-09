#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>
#include <secrets.h>

const int WIND_DIR_PIN = A0;
const int RAIN_PIN = 13;
const int WIND_SPEED_PIN = 12;
const float BUCKET_SIZE = 0.2794;

const unsigned long SEND_INTERVAL = 10UL * 60UL * 1000UL;
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;
const unsigned long OFFLINE_MAX_LINES = 5000;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 21600000);

Adafruit_BME680 bme;
bool bmeFound = false;
bool fsMounted = false;

ESP8266WebServer debugServer(80);
String debugLog = "";
const size_t DEBUG_LOG_MAX_CHARS = 6000;
String debugTopic;

volatile unsigned long rainClicks = 0;
volatile unsigned long lastRainTime = 0;
const unsigned long RAIN_DEBOUNCE = 200;

volatile unsigned long windClicks = 0;
volatile unsigned long gustClicks = 0;
volatile unsigned long lastWindClickTime = 0;
const unsigned long WIND_DEBOUNCE = 10;

unsigned long lastSendTime = 0;
unsigned long lastGustCalcTime = 0;
unsigned long lastMqttAttempt = 0;
unsigned long mqttConnectedAt = 0;
bool wasMqttConnected = false;
const unsigned long REPLAY_GRACE_MS = 100000;
float maxGustKmh = 0.0;
int lastValidWindDir = 0;
const char* offlineDataFile = "/offline_data.jsonl";

void ICACHE_RAM_ATTR handleRainInterrupt() {
  unsigned long currentTime = millis();
  if (currentTime - lastRainTime > RAIN_DEBOUNCE) {
    rainClicks++;
    lastRainTime = currentTime;
  }
}

void ICACHE_RAM_ATTR handleWindSpeedInterrupt() {
  unsigned long currentTime = millis();
  if (currentTime - lastWindClickTime > WIND_DEBOUNCE) {
    windClicks++;
    gustClicks++;
    lastWindClickTime = currentTime;
  }
}

void logMsg(const String &msg) {
  Serial.println(msg);

  debugLog += msg + "\n";
  if (debugLog.length() > DEBUG_LOG_MAX_CHARS) {
    debugLog = debugLog.substring(debugLog.length() - DEBUG_LOG_MAX_CHARS);
  }

  if (mqttClient.connected() && debugTopic.length() > 0) {
    mqttClient.publish(debugTopic.c_str(), msg.c_str());
  }
}

String epochToUtcString(unsigned long epoch) {
  time_t rawtime = (time_t)epoch;
  struct tm *ti = gmtime(&rawtime);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", ti);
  return String(buf);
}

bool mqttReady() {
  return mqttClient.connected() && (millis() - mqttConnectedAt >= REPLAY_GRACE_MS);
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

void connectMQTT() {
  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;
  if (now - lastMqttAttempt < MQTT_RECONNECT_INTERVAL) return;

  lastMqttAttempt = now;
  if (mqttClient.connect("StazioneMeteoESP8266")) {
    logMsg("[MQTT] Connessione OK");
  } else {
    logMsg("[MQTT] Connessione FALLITA, rc=" + String(mqttClient.state()));
  }
}

unsigned long countOfflineLines() {
  if (!fsMounted || !LittleFS.exists(offlineDataFile)) return 0;
  File f = LittleFS.open(offlineDataFile, "r");
  if (!f) return 0;
  unsigned long lines = 0;
  while (f.available()) {
    f.readStringUntil('\n');
    lines++;
  }
  f.close();
  return lines;
}

void saveOfflineData(const String &payload) {
  if (!fsMounted) {
    logMsg("[FS] LittleFS non montato: impossibile salvare il dato offline!");
    return;
  }

  if (countOfflineLines() >= OFFLINE_MAX_LINES) {
    logMsg("[FS] Limite righe offline raggiunto, dato scartato.");
    return;
  }

  File f = LittleFS.open(offlineDataFile, "a");
  if (f) {
    f.println(payload);
    f.close();
    logMsg("[FS] Dato salvato offline: " + payload);
  } else {
    logMsg("[FS] ERRORE apertura file in append!");
  }
}

void sendOfflineData() {
  if (!fsMounted || !LittleFS.exists(offlineDataFile)) return;

  File f = LittleFS.open(offlineDataFile, "r");
  if (!f) return;

  String pending = "";
  unsigned long sentCount = 0;
  unsigned long failCount = 0;

  while (f.available()) {
    String payload = f.readStringUntil('\n');
    payload.trim();
    if (payload.length() == 0) continue;

    if (mqttClient.connected() && mqttClient.publish(mqtt_topic, payload.c_str())) {
      sentCount++;
      delay(50);
    } else {
      failCount++;
      pending += payload + "\n";
      if (!mqttClient.connected()) {
        while (f.available()) {
          pending += f.readStringUntil('\n') + "\n";
        }
        break;
      }
    }
  }
  f.close();

  LittleFS.remove(offlineDataFile);
  if (pending.length() > 0) {
    File wf = LittleFS.open(offlineDataFile, "w");
    if (wf) {
      wf.print(pending);
      wf.close();
    }
  }

  if (sentCount > 0 || failCount > 0) {
    logMsg("[FS] Replay offline: inviati=" + String(sentCount) + " falliti=" + String(failCount));
  }
}

void handleRoot() {
  unsigned long now = millis();
  unsigned long epoch = timeClient.getEpochTime();

  String html = "<html><head><meta charset='utf-8'>";
  html += "<meta http-equiv='refresh' content='15'>";
  html += "<style>body{font-family:monospace;background:#111;color:#0f0;padding:10px}";
  html += "h2{color:#6cf}pre{white-space:pre-wrap;background:#000;padding:8px;border:1px solid #333}</style>";
  html += "</head><body>";
  html += "<h2>Debug</h2>";
  html += "<b>Uptime:</b> " + String(now / 1000) + " s<br>";
  html += "<b>WiFi:</b> " + String(WiFi.status() == WL_CONNECTED ? "connesso" : "DISCONNESSO") + " (" + WiFi.localIP().toString() + ")<br>";
  html += "<b>MQTT:</b> " + String(mqttClient.connected() ? "connesso" : "DISCONNESSO") + "<br>";
  html += "<b>LittleFS:</b> " + String(fsMounted ? "montato" : "NON montato") + "<br>";
  html += "<b>BME680:</b> " + String(bmeFound ? "trovato" : "non trovato") + "<br>";
  html += "<b>Righe offline in coda:</b> " + String(countOfflineLines()) + "<br>";
  html += "<hr><h2>Timestamp</h2>";
  html += "<b>Epoch (raw):</b> " + String(epoch) + "<br>";
  html += "<b>Date:</b> " + epochToUtcString(epoch) + "<br>";
  html += "<hr><h2>Log</h2><pre>" + debugLog + "</pre>";
  html += "<hr><p><a href='/offline'>File offline_data.jsonl</a></p>";
  html += "</body></html>";

  debugServer.send(200, "text/html", html);
}

void handleOfflineFile() {
  if (!fsMounted || !LittleFS.exists(offlineDataFile)) {
    debugServer.send(200, "text/plain", "(file offline non presente)");
    return;
  }
  File f = LittleFS.open(offlineDataFile, "r");
  if (!f) {
    debugServer.send(500, "text/plain", "Errore apertura file");
    return;
  }
  debugServer.streamFile(f, "text/plain");
  f.close();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  logMsg("\n[BOOT] Avvio stazione meteo...");

  fsMounted = LittleFS.begin();
  if (!fsMounted) {
    logMsg("[FS] ERRORE: LittleFS.begin() fallito! Provo a formattare...");
    if (LittleFS.format()) {
      fsMounted = LittleFS.begin();
    }
  }
  logMsg(fsMounted ? "[FS] LittleFS montato correttamente." : "[FS] LittleFS NON disponibile.");

  pinMode(RAIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(RAIN_PIN), handleRainInterrupt, FALLING);
  pinMode(WIND_SPEED_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(WIND_SPEED_PIN), handleWindSpeedInterrupt, FALLING);

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

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connessione");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  logMsg("[WiFi] OK, IP: " + WiFi.localIP().toString());

  timeClient.begin();

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();

  mqttClient.setServer(mqtt_server, mqtt_port);

  debugTopic = "debug/stazione_meteo";

  debugServer.on("/", handleRoot);
  debugServer.on("/offline", handleOfflineFile);
  debugServer.begin();
  logMsg("[HTTP] Debug server avviato su http://" + WiFi.localIP().toString() + "/");

  lastSendTime = millis();
  lastGustCalcTime = millis();
  lastMqttAttempt = 0;

  unsigned long pending = countOfflineLines();
  if (pending > 0) {
    logMsg("[FS] " + String(pending) + " righe in coda dal riavvio precedente.");
  }
}

void loop() {
  debugServer.handleClient();

  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
    timeClient.update();

    bool isMqttConnected = mqttClient.connected();

    if (!isMqttConnected) {
      connectMQTT();
    } else {
      mqttClient.loop();

      if (!wasMqttConnected) {
        mqttConnectedAt = millis();
        logMsg("[MQTT] Connesso, attendo " + String(REPLAY_GRACE_MS / 1000) + "s prima del replay offline...");
      }

      if (millis() - mqttConnectedAt >= REPLAY_GRACE_MS) {
        sendOfflineData();
      }
    }
    wasMqttConnected = isMqttConnected;
  } else {
    wasMqttConnected = false;
  }

  unsigned long currentMillis = millis();

  if (currentMillis - lastGustCalcTime >= 3000) {
    noInterrupts();
    unsigned long currentGustClicks = gustClicks;
    gustClicks = 0;
    interrupts();

    float currentGust = ((float)currentGustClicks / 3.0) * 2.4;
    if (currentGust > maxGustKmh) {
      maxGustKmh = currentGust;
    }
    lastGustCalcTime = currentMillis;
  }

  if (currentMillis - lastSendTime >= SEND_INTERVAL) {
    float t = 0, h = 0, p = 0, g = 0;
    if (bmeFound && bme.performReading()) {
      t = bme.temperature;
      h = bme.humidity;
      p = bme.pressure / 100.0;
      g = bme.gas_resistance / 1000.0;
    }

    int adcVal = analogRead(WIND_DIR_PIN);
    int wDir = getWindDirectionDegrees(adcVal);

    if (wDir != -1) {
      lastValidWindDir = wDir;
    } else {
      wDir = lastValidWindDir;
    }

    noInterrupts();
    unsigned long currentWindClicks = windClicks;
    unsigned long currentRainClicks = rainClicks;
    windClicks = 0;
    rainClicks = 0;
    interrupts();

    float elapsedSeconds = (currentMillis - lastSendTime) / 1000.0;
    float rps = (elapsedSeconds > 0) ? (float)currentWindClicks / elapsedSeconds : 0;
    float wSpeed = rps * 2.4;
    float rainMM = currentRainClicks * BUCKET_SIZE;

    unsigned long epochTime = timeClient.getEpochTime();

    String json = "{";
    json += "\"timestamp\":" + String(epochTime) + ",";
    json += "\"wind_dir\":" + String(wDir) + ",";
    json += "\"wind_speed\":" + String(wSpeed, 2) + ",";
    json += "\"wind_gust\":" + String(maxGustKmh, 2) + ",";
    json += "\"rain_mm\":" + String(rainMM, 2) + ",";
    json += "\"temp\":" + String(t, 2) + ",";
    json += "\"hum\":" + String(h, 2) + ",";
    json += "\"pres\":" + String(p, 2) + ",";
    json += "\"gas\":" + String(g, 2);
    json += "}";

    maxGustKmh = 0.0;

    bool sent = false;
    if (mqttReady()) {
      sent = mqttClient.publish(mqtt_topic, json.c_str());
      if (!sent) {
        logMsg("[MQTT] publish() fallito nonostante connessione attiva.");
      }
    }

    if (!sent) {
      if (epochTime > 1600000000) {
        saveOfflineData(json);
      } else {
        logMsg("[NTP] Orario non valido (" + String(epochTime) + "), dato scartato (non salvato offline).");
      }
    } else {
      logMsg("[MQTT] Dato inviato (" + epochToUtcString(epochTime) + "): " + json);
    }

    lastSendTime = currentMillis;
  }
}