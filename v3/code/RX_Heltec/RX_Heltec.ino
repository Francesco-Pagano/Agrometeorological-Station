#include "heltec.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#define WS_TOKEN "YOUR_THINGSBOARD_TOKEN1"
#define LEAF_TOKEN "YOUR_THINGSBOARD_TOKEN2"
#define BME_TOKEN "YOUR_THINGSBOARD_TOKEN3"
#define PMSA_TOKEN "YOUR_THINGSBOARD_TOKEN4"
#define STATUS_TOKEN "YOUR_THINGSBOARD_TOKEN5"

//WI-FI
const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";

//IF YOU USE MULTIPLE SERVERS
const char* mqtt_server1 = "YOUR_IP1";
const int mqtt_port1 = 1884;
const char* mqtt_server2 = "YOUR_IP2";
const int mqtt_port2 = 1883;
const char* telemetryTopic = "v1/devices/me/telemetry";

WiFiClient espClient;
PubSubClient client1(espClient);
PubSubClient client2(espClient);

#define BAND 866E6

byte buffer[256];
int packetSize = 0;
int rssi = 0;

void onReceive(int size) {
  packetSize = size;

  int i = 0;
  while (LoRa.available()) {
    byte b = LoRa.read();
    buffer[i] = b;
    i++;
  }
  rssi = LoRa.packetRssi();
}

void setup() {
  LoRa.begin(BAND, true);
  Serial.begin(115200);

  LoRa.onReceive(onReceive);
  delay(100);
  Serial.println("LoRa Init");
  LoRa.receive();

  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  delay(1000);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  setup_wifi();

  client1.setServer(mqtt_server1, mqtt_port1);
  client2.setServer(mqtt_server2, mqtt_port2);
}

void setup_wifi() {
  if (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    WiFi.reconnect();
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void sendTelemetry(const char* jsonData, char* access_token) {
  if (client1.connect("ESP32 Client", access_token, NULL)) {
    client1.publish(telemetryTopic, jsonData);
    client1.disconnect();
  } else {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
    }

    delay(5000);
    setup_wifi();
  }

  if (client2.connect("ESP32 Client", access_token, NULL)) {
    client2.publish(telemetryTopic, jsonData);
    client2.disconnect();
  } else {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
    }

    delay(5000);
    setup_wifi();
  }
}

void loop() {
  static unsigned long previousMillis = 0;
  const unsigned long interval = 86400000;

  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    ESP.restart();
  }

  if (rssi != 0) {
    byte sensorType = buffer[0];
    
    float temp, hum, gust, avg, dir, rain, light, uv, wetness, pressure, gas, altitude;

    float pm10, pm25, pm100, pm10e, pm25e, pm100e, p3, p5, p10, p25, p50, p100;
    float voltage, battery;

    StaticJsonDocument<1024> jsonDoc;
    String jsonDoc_string;

    switch (sensorType){
      case 0:
        jsonDoc.clear();
        readFloatFromBuffer(buffer, 1, &temp);
        readFloatFromBuffer(buffer, 5, &hum);
        readFloatFromBuffer(buffer, 9, &gust);
        readFloatFromBuffer(buffer, 13, &avg);
        readFloatFromBuffer(buffer, 17, &dir);
        readFloatFromBuffer(buffer, 21, &rain);
        readFloatFromBuffer(buffer, 25, &light);
        readFloatFromBuffer(buffer, 29, &uv);
        jsonDoc["Temperature"] = temp;
        jsonDoc["Humidity"] = hum;
        jsonDoc["Gust"] = gust;
        jsonDoc["WindAvg"] = avg;
        jsonDoc["WindDir"] = dir;
        jsonDoc["Rain"] = rain;
        jsonDoc["Light"] = light;
        jsonDoc["Uv"] = uv;
        serializeJson(jsonDoc, jsonDoc_string);
        sendTelemetry(jsonDoc_string.c_str(), WS_TOKEN);
        break;
      case 1:
        jsonDoc.clear();
        readFloatFromBuffer(buffer, 1, &temp);
        readFloatFromBuffer(buffer, 5, &wetness);
        jsonDoc["Temperature"] = temp;
        jsonDoc["Wetness"] = wetness;
        serializeJson(jsonDoc, jsonDoc_string);
        sendTelemetry(jsonDoc_string.c_str(), LEAF_TOKEN);
        break;
      case 2:
        jsonDoc.clear();
        readFloatFromBuffer(buffer, 1, &temp);
        readFloatFromBuffer(buffer, 5, &pressure);
        readFloatFromBuffer(buffer, 9, &hum);
        readFloatFromBuffer(buffer, 13, &gas);
        readFloatFromBuffer(buffer, 17, &altitude);
        if(temp == 0 && pressure == 0 && hum == 0 && gas == 0){
          Serial.println("Non mandare BME680");
          break;
        }else{
          jsonDoc["Temperature"] = temp;
          jsonDoc["Pressure"] = pressure;
          jsonDoc["Humidity"] = hum;
          jsonDoc["Gas"] = gas;
          jsonDoc["Altitude"] = altitude;
          serializeJson(jsonDoc, jsonDoc_string);
          sendTelemetry(jsonDoc_string.c_str(), BME_TOKEN);
          break;
        }
      case 3:
        jsonDoc.clear();
        readFloatFromBuffer(buffer, 1, &pm10);
        readFloatFromBuffer(buffer, 5, &pm25);
        readFloatFromBuffer(buffer, 9, &pm100);
        readFloatFromBuffer(buffer, 13, &pm10e);
        readFloatFromBuffer(buffer, 17, &pm25e);
        readFloatFromBuffer(buffer, 21, &pm100e);
        readFloatFromBuffer(buffer, 25, &p3);
        readFloatFromBuffer(buffer, 29, &p5);
        readFloatFromBuffer(buffer, 33, &p10);
        readFloatFromBuffer(buffer, 37, &p25);
        readFloatFromBuffer(buffer, 41, &p50);
        readFloatFromBuffer(buffer, 45, &p100);
        jsonDoc["Pm1_0"] = pm10;
        jsonDoc["Pm2_5"] = pm25;
        jsonDoc["Pm10_0"] = pm100;
        jsonDoc["Pm1_0env"] = pm10e;
        jsonDoc["Pm2_5env"] = pm25e;
        jsonDoc["Pm10_0env"] = pm100e;
        jsonDoc["P3"] = p3;
        jsonDoc["P5"] = p5;
        jsonDoc["P10"] = p10;
        jsonDoc["P25"] = p25;
        jsonDoc["P50"] = p50;
        jsonDoc["P100"] = p100;
        serializeJson(jsonDoc, jsonDoc_string);
        sendTelemetry(jsonDoc_string.c_str(), PMSA_TOKEN);
        break;
      case 4:
        jsonDoc.clear();
        readFloatFromBuffer(buffer, 1, &voltage);
        readFloatFromBuffer(buffer, 5, &battery);
        jsonDoc["Voltage"] = voltage;
        jsonDoc["Battery"] = battery;
        jsonDoc["RSSI"] = rssi;
        serializeJson(jsonDoc, jsonDoc_string);
        sendTelemetry(jsonDoc_string.c_str(), STATUS_TOKEN);
        break;
    }

    /*Serial.println(packetSize);
    for (int i = 0; i < packetSize; i++) {
      byte b = buffer[i];
      Serial.print(b);
      Serial.print(" ");
    }*/
    
    rssi = 0;
  }
}

void readFloatFromBuffer(byte* buffer, int offset, float* value) {
  byte tmp_float_buffer[4];
  for (int i = offset, j = 0; j < 4; i++, j++) {
    tmp_float_buffer[j] = buffer[i];
  }
  memcpy(value, &tmp_float_buffer, 4);
}