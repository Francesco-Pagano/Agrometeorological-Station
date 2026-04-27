#include <Arduino.h>
#include "ArduinoJson.h"
#include <string>
#include <RadioLib.h>
#include "heltec.h"
#include <HardwareSerial.h>
#include <Wire.h>

//LEAF - RS485
#define RXD2 16
#define TXD2 17
byte values[11];
HardwareSerial mod(1);
const byte leaf[] = {0x01, 0x04, 0x00, 0x00, 0x00, 0x02, 0x71, 0xCB};

//I2C
#include <Adafruit_Sensor.h>
#include "Adafruit_BME680.h"
#include "Adafruit_PM25AQI.h"

Adafruit_BME680 bme;
#define SEALEVELPRESSURE_HPA (1013.25)

Adafruit_PM25AQI aqi = Adafruit_PM25AQI();

//VOLTAGE
#define ANALOG_PIN 39
 
float adc_voltage = 0.0;
float voltage = 0.0;

float R1 = 51000.0;
float R2 = 7500.0; 

float ref_voltage = 3.2;
int adc_value = 0;

//WEATHER STATION
#define BAND 868E6

typedef struct {
  bool temp_ok = false;
  bool humidity_ok = false;
  bool light_ok = false;
  bool uv_ok = false;
  bool wind_ok = false;
  bool rain_ok = false;      
  bool battery_ok = false;
  float temp_c;
  float light_lux;
  float uv;
  float rain_mm;
  float wind_direction_deg;
  float wind_gust_meter_sec;
  float wind_avg_meter_sec;
  uint8_t humidity;
  float rssi;
} Sensor;

#define PIN_RECEIVER_CS 2
#define PIN_RECEIVER_IRQ 37 //D0
#define PIN_RECEIVER_GPIO 38 //D2

#define PIN_VSPI_MOSI 23
#define PIN_VSPI_SCK  12
#define PIN_VSPI_MISO 13
#define PIN_VSPI_SS   15

SPIClass spi(HSPI);
SPISettings spiSettings(2000000, MSBFIRST, SPI_MODE0);
static CC1101 radio = new Module(PIN_RECEIVER_CS, PIN_RECEIVER_IRQ, RADIOLIB_NC, PIN_RECEIVER_GPIO, spi, spiSettings);
static Sensor sensor;

//BME680
bool isFirstReading = false;

uint8_t crc8(uint8_t const message[], unsigned nBytes, uint8_t polynomial, uint8_t init) {
  uint8_t remainder = init;
  unsigned byte, bit;

  for (byte = 0; byte < nBytes; ++byte) {
    remainder ^= message[byte];
    for (bit = 0; bit < 8; ++bit) {
      if (remainder & 0x80) {
        remainder = (remainder << 1) ^ polynomial;
      } else {
        remainder = (remainder << 1);
      }
    }
  }
  return remainder;
}

int decodeFineOffsetWH65BPayload(float rssi, uint8_t* b, uint8_t msgSize) {
  if (b[0] != 0x24) {
  }

  uint8_t crc = crc8(b, 15, 0x31, 0x00);
  uint8_t checksum = 0;
  for (unsigned n = 0; n < 16; ++n) {
    checksum += b[n];
  }
  if (crc != b[15] || checksum != b[16]) {
    return -1;
  }

  //DECODE DATA
  int id = b[1];
  int wind_dir = b[2] | (b[3] & 0x80) << 1;
  int low_battery = (b[3] & 0x08) >> 3;
  int temp_raw = (b[3] & 0x07) << 8 | b[4];
  float temperature = (temp_raw - 400) * 0.1f;
  int humidity = b[5];
  int wind_speed_raw = b[6] | (b[3] & 0x10) << 4;
  float wind_speed_factor, rain_cup_count;

  wind_speed_factor = 0.51f;
  rain_cup_count = 0.254f;

  float wind_speed_ms = wind_speed_raw * 0.125f * wind_speed_factor;
  int gust_speed_raw = b[7];

  float gust_speed_ms = gust_speed_raw * wind_speed_factor;
  int rainfall_raw = b[8] << 8 | b[9];
  float rainfall_mm = rainfall_raw * rain_cup_count;
  int uv_raw = b[10] << 8 | b[11];
  int light_raw = b[12] << 16 | b[13] << 8 | b[14];
  double light_lux = light_raw * 0.1;

  int uvi_upper[] = { 432, 851, 1210, 1570, 2017, 2450, 2761, 3100, 3512, 3918, 4277, 4650, 5029 };
  int uv_index = 0;
  while (uv_index < 13 && uvi_upper[uv_index] < uv_raw) ++uv_index;

  sensor.temp_ok = true;
  sensor.humidity_ok = true;
  sensor.wind_ok = true;
  sensor.rain_ok = true;
  sensor.light_ok = true;
  sensor.uv_ok = true;

  sensor.temp_c = temperature;
  sensor.humidity = humidity;
  sensor.wind_direction_deg = wind_dir;
  sensor.wind_gust_meter_sec = gust_speed_ms;
  sensor.wind_avg_meter_sec = wind_speed_ms;

  sensor.rain_mm = rainfall_mm;
  sensor.light_lux = light_lux;
  sensor.uv = uv_index;
  sensor.battery_ok = !low_battery;
  sensor.rssi = rssi;
    return 0;
}

int receiveData() {
  uint8_t recvData[27];
  
  int state = radio.receive(recvData, 27);
  float rssi = radio.getRSSI();
  if (state == RADIOLIB_ERR_NONE) {
    if (recvData[0] == 0xD4) {
      state = decodeFineOffsetWH65BPayload(rssi, &recvData[1], sizeof(recvData) - 1);
      if (state != 0) {
        return -1;
      }      
    }
    else if (state == RADIOLIB_ERR_RX_TIMEOUT) {
      return -2;
    }
    else {
      return -3;
    }
  } else {
    return -4;
  }
  return 0;
}


void setup() {
  delay(1000);
  
  Serial.begin(115200);
  LoRa.begin(866E6, true);
  spi.begin(PIN_VSPI_SCK, PIN_VSPI_MISO, PIN_VSPI_MOSI, PIN_VSPI_SS);
  mod.begin(9600, SERIAL_8N1, RXD2, TXD2);

  int state = radio.begin(868.3, 17.24, 40, 270, 10, 32);
 
  if (state == RADIOLIB_ERR_NONE) {
    state = radio.setCrcFiltering(false);
    if (state != RADIOLIB_ERR_NONE) {
      while (true)
      ;
    }
    state = radio.fixedPacketLengthMode(27);
    if (state != RADIOLIB_ERR_NONE) {
      while (true)
        ;
    }
    state = radio.setSyncWord(0xAA, 0x2D, 0, false);
    if (state != RADIOLIB_ERR_NONE) {
      while (true)
        ;
    }
  } else {
    while (true)
      ;
  }
  
  bme.begin();
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);

  aqi.begin_I2C();

  delay(10);
}

void loop() {
  static unsigned long previousMillis = 0;
  const unsigned long interval = 86400000;

  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    ESP.restart();
  }
  
  //LEAF
  byte val;
  delay(10);
  if (mod.write(leaf, sizeof(leaf)) == 8){
    for (byte i = 0; i < 11; i++){
      values[i] = mod.read();
    }
  }
  float temp = (float(values[3]) * 256 + float(values[4])) / 100;
  float wetness = (float(values[5]) * 256 + float(values[6])) / 100;

  //I2C
  PM25_AQI_Data data;
  aqi.read(&data);

  //VOLTAGE
   adc_value = analogRead(ANALOG_PIN);
   adc_voltage  = (adc_value * ref_voltage) / 4096.0; 
   voltage = adc_voltage / (R2/(R1+R2)) ; 

  int state = receiveData();

  if (state == 0) {
      unsigned char buffer[255];
      delay(5000);

      //Weather Station
      int index = 0;
      buffer[index++] = 0;
      insertIntoBuffer(&index, buffer, sensor.temp_c);
      insertIntoBuffer(&index, buffer, sensor.humidity);
      insertIntoBuffer(&index, buffer, sensor.wind_gust_meter_sec);
      insertIntoBuffer(&index, buffer, sensor.wind_avg_meter_sec);
      insertIntoBuffer(&index, buffer, sensor.wind_direction_deg);
      insertIntoBuffer(&index, buffer, sensor.rain_mm);
      insertIntoBuffer(&index, buffer, sensor.light_lux);
      insertIntoBuffer(&index, buffer, sensor.uv);
      sendBuffer(buffer, index);
      delay(5000);

      //LEAF
      index = 0;
      buffer[index++] = 1;
      insertIntoBuffer(&index, buffer, temp);
      insertIntoBuffer(&index, buffer, wetness);
      sendBuffer(buffer, index);
      delay(5000);

      //BME680
      index = 0;
      buffer[index++] = 2;
      insertIntoBuffer(&index, buffer, bme.temperature);
      insertIntoBuffer(&index, buffer, bme.pressure / 100.0);
      insertIntoBuffer(&index, buffer, bme.humidity);
      insertIntoBuffer(&index, buffer, bme.gas_resistance / 1000.0);
      insertIntoBuffer(&index, buffer, bme.readAltitude(SEALEVELPRESSURE_HPA));
      sendBuffer(buffer, index);
      delay(5000);

      //PMA
      index = 0;
      buffer[index++] = 3;
      insertIntoBuffer(&index, buffer, data.pm10_standard);
      insertIntoBuffer(&index, buffer, data.pm25_standard);
      insertIntoBuffer(&index, buffer, data.pm100_standard);
      insertIntoBuffer(&index, buffer, data.pm10_env);
      insertIntoBuffer(&index, buffer, data.pm25_env);
      insertIntoBuffer(&index, buffer, data.pm100_env);
      insertIntoBuffer(&index, buffer, data.particles_03um);
      insertIntoBuffer(&index, buffer, data.particles_05um);
      insertIntoBuffer(&index, buffer, data.particles_10um);
      insertIntoBuffer(&index, buffer, data.particles_25um);
      insertIntoBuffer(&index, buffer, data.particles_50um);
      insertIntoBuffer(&index, buffer, data.particles_100um);
      sendBuffer(buffer, index);
      delay(5000);

      //STATUS
      index = 0;
      buffer[index++] = 4;
      insertIntoBuffer(&index, buffer, voltage);
      insertIntoBuffer(&index, buffer, sensor.battery_ok);
      sendBuffer(buffer, index);
      delay(2000);
  }

}

void sendBuffer(unsigned char* buffer, int size) {

      /*Serial.println(size);
      if (size > 255) Serial.println("Buffer overlow!!!!!!!");
      for (int i = 0; i < size; i++) {
        byte b = buffer[i];
        Serial.print(b);
        Serial.print(" ");
      }*/

      //SEND LORA
      LoRa.beginPacket();
      LoRa.setTxPower(20,RF_PACONFIG_PASELECT_PABOOST);
      LoRa.write(buffer, size);
      int ack = LoRa.endPacket();
}

void insertIntoBuffer(int* index, unsigned char* buffer, float value) {
  byte tmp_float[4];
  float2Bytes(tmp_float, value);
  buffer[(*index)++] = (char) (tmp_float[0]);
  buffer[(*index)++] = (char) (tmp_float[1]);
  buffer[(*index)++] = (char) (tmp_float[2]);
  buffer[(*index)++] = (char) (tmp_float[3]);
}

void float2Bytes(byte bytes_temp[4],float float_variable){ 
  memcpy(bytes_temp, (unsigned char*) (&float_variable), 4);
}