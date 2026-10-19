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
#include <config.h>

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
unsigned long lastOfflineReplayTime = 0;
bool wasMqttConnected = false;

float maxGustKmh = 0.0;
int lastValidWindDir = 0;

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

void connectMQTT() {
  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;
  if (now - lastMqttAttempt < MQTT_RECONNECT_INTERVAL) return;

  lastMqttAttempt = now;
  if (mqttClient.connect(mqtt_clientName)) {
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
    yield();
  }
  f.close();
  return lines;
}

void saveOfflineData(const String &payload) {
  if (!fsMounted) {
    logMsg("[FS] LittleFS non montato: impossibile salvare!");
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
    logMsg("[FS] Dato salvato offline.");
  } else {
    logMsg("[FS] ERRORE apertura file in append!");
  }
}

void sendOfflineData() {
  if (!fsMounted || !LittleFS.exists(offlineDataFile)) return;

  File f = LittleFS.open(offlineDataFile, "r");
  if (!f) return;
  
  if (f.size() == 0) {
    f.close();
    LittleFS.remove(offlineDataFile);
    return;
  }

  const int MAX_BATCH = 5; 
  unsigned long sentCount = 0;
  unsigned long failCount = 0;
  bool stopSending = false;
  File tempF;

  while (f.available()) {
    String payload = f.readStringUntil('\n');
    payload.trim();
    if (payload.length() == 0) continue;

    if (!stopSending && sentCount < MAX_BATCH && mqttClient.connected()) {
      if (mqttClient.publish(mqtt_topic, payload.c_str())) {
        sentCount++;
        delay(250); 
        yield(); 
        continue;
      } else {
        failCount++;
        stopSending = true; 
      }
    } else {
      stopSending = true; 
    }

    if (!tempF) {
      tempF = LittleFS.open(offlineTempFile, "w");
    }
    if (tempF) {
      tempF.println(payload);
    }
    yield(); 
  }
  
  f.close();

  if (tempF) {
    tempF.close();
    LittleFS.remove(offlineDataFile);
    LittleFS.rename(offlineTempFile, offlineDataFile);
  } else if (!stopSending) {
    LittleFS.remove(offlineDataFile);
  }

  if (sentCount > 0 || failCount > 0) {
    logMsg("[FS] Offline: " + String(sentCount) + " inviati, " + String(failCount) + " falliti");
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
  html += "<h2>Stazione Meteo ESP8266</h2>";
  html += "<b>Uptime:</b> " + String(now / 1000) + " s<br>";
  html += "<b>WiFi:</b> " + String(WiFi.status() == WL_CONNECTED ? "connesso" : "DISCONNESSO") + " (" + WiFi.localIP().toString() + ")<br>";
  html += "<b>MQTT:</b> " + String(mqttClient.connected() ? "connesso" : "DISCONNESSO") + "<br>";
  html += "<b>LittleFS:</b> " + String(fsMounted ? "montato" : "NON montato") + "<br>";
  html += "<b>BME680:</b> " + String(bmeFound ? "trovato" : "non trovato") + "<br>";
  html += "<b>Righe offline:</b> " + String(countOfflineLines()) + "<br>";
  html += "<hr><h2>Timestamp</h2>";
  html += "<b>Epoch:</b> " + String(epoch) + "<br>";
  html += "<b>Date:</b> " + epochToUtcString(epoch) + "<br>";
  html += "<hr><h2>Log</h2><pre>" + debugLog + "</pre>";
  html += "<hr><p><a href='/offline'>Scarica offline_data.jsonl</a></p>";
  html += "</body></html>";

  debugServer.send(200, "text/html", html);
}

void handleOfflineFile() {
  if (!fsMounted || !LittleFS.exists(offlineDataFile)) {
    debugServer.send(200, "text/plain", "(file offline non presente o vuoto)");
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
  Serial.println("\n[BOOT] Avvio stazione meteo...");

  fsMounted = LittleFS.begin();
  if (!fsMounted) {
    Serial.println("[FS] ERRORE: LittleFS.begin() fallito! Provo a formattare...");
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
  
  mqttClient.setBufferSize(512);

  debugServer.on("/", handleRoot);
  debugServer.on("/offline", handleOfflineFile);
  debugServer.begin();
  logMsg("[HTTP] Debug server avviato.");
  
  randomSeed(micros());

  lastSendTime = millis();
  lastGustCalcTime = millis();

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
        lastOfflineReplayTime = millis() + random(0, 10000); 
        logMsg("[MQTT] Connesso, attesa pre-replay offline...");
      }

      if (millis() - mqttConnectedAt >= REPLAY_GRACE_MS && millis() - lastOfflineReplayTime >= REPLAY_BATCH_DELAY) {
        sendOfflineData();
        lastOfflineReplayTime = millis();
      }
    }
    wasMqttConnected = isMqttConnected;
  } else {
    wasMqttConnected = false;
  }

  unsigned long currentMillis = millis();

  if (currentMillis - lastGustCalcTime >= GUST_CALC_INTERVAL) {
    noInterrupts();
    unsigned long currentGustClicks = gustClicks;
    gustClicks = 0;
    interrupts();

    float currentGust = ((float)currentGustClicks / (GUST_CALC_INTERVAL / 1000.0)) * WIND_SPEED_FACTOR;
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
    float wSpeed = rps * WIND_SPEED_FACTOR;
    float rainMM = currentRainClicks * BUCKET_SIZE;

    unsigned long epochTime = timeClient.getEpochTime();

    char jsonBuffer[256];
    snprintf(jsonBuffer, sizeof(jsonBuffer),
             "{\"timestamp\":%lu,\"wind_dir\":%d,\"wind_speed\":%.2f,\"wind_gust\":%.2f,\"rain_mm\":%.2f,\"temp\":%.2f,\"hum\":%.2f,\"pres\":%.2f,\"gas\":%.2f}",
             epochTime, wDir, wSpeed, maxGustKmh, rainMM, t, h, p, g);

    maxGustKmh = 0.0;

    bool sent = false;
    if (mqttReady()) {
      sent = mqttClient.publish(mqtt_topic, jsonBuffer);
      if (!sent) {
        logMsg("[MQTT] publish() fallito.");
      }
    }

    if (!sent) {
      if (epochTime > MIN_VALID_EPOCH) {
        saveOfflineData(String(jsonBuffer));
      } else {
        logMsg("[NTP] Orario errato, riga ignorata.");
      }
    } else {
      logMsg("[MQTT] Inviato: " + String(jsonBuffer));
    }

    lastSendTime = currentMillis;
  }
}