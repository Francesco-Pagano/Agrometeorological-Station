#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <secrets.h>

const int WIND_DIR_PIN = A0;
const int RAIN_PIN = 13;
const int WIND_SPEED_PIN = 12;
const float BUCKET_SIZE = 0.2794;

const unsigned long SEND_INTERVAL = 10 * 60 * 1000;

WiFiClient espClient;
PubSubClient mqttClient(espClient);

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 21600000);

Adafruit_BME680 bme;
bool bmeFound = false;

volatile unsigned long rainClicks = 0;
volatile unsigned long lastRainTime = 0;
const unsigned long RAIN_DEBOUNCE = 200;

volatile unsigned long windClicks = 0;
volatile unsigned long gustClicks = 0;
volatile unsigned long lastWindClickTime = 0;
const unsigned long WIND_DEBOUNCE = 10;

unsigned long lastSendTime = 0;
unsigned long lastGustCalcTime = 0;
float maxGustKmh = 0.0;
int lastValidWindDir = 0;
String offlineDataFile = "/offline_data.jsonl";

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
  if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    mqttClient.connect("StazioneMeteoESP8266");
  }
}

void saveOfflineData(String payload) {
  File f = LittleFS.open(offlineDataFile, "a");
  if (f) {
    f.println(payload);
    f.close();
  }
}

void sendOfflineData() {
  if (LittleFS.exists(offlineDataFile)) {
    File f = LittleFS.open(offlineDataFile, "r");
    if (f) {
      bool allSent = true;
      while (f.available()) {
        String payload = f.readStringUntil('\n');
        payload.trim();
        if (payload.length() > 0) {
          if (!mqttClient.publish(mqtt_topic, payload.c_str())) {
            allSent = false;
            break;
          }
          delay(50);
        }
      }
      f.close();
      if (allSent) {
        LittleFS.remove(offlineDataFile);
      }
    }
  }
}

void setup() {
  LittleFS.begin();

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

  if (bmeFound) {
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150);
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); }

  timeClient.begin();

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.begin();

  mqttClient.setServer(mqtt_server, mqtt_port);
  
  lastSendTime = millis();
  lastGustCalcTime = millis();
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
    timeClient.update();
    
    if (!mqttClient.connected()) {
      connectMQTT();
    } else {
      mqttClient.loop();
      sendOfflineData();
    }
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
    float rps = (float)currentWindClicks / elapsedSeconds;
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

    if (mqttClient.connected()) {
      mqttClient.publish(mqtt_topic, json.c_str());
    } else if (epochTime > 1600000000) {
      saveOfflineData(json);
    }

    lastSendTime = currentMillis;
  }
}