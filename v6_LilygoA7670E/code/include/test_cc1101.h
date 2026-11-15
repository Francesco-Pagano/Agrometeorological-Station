#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include "config.h"

unsigned long lastMeteoDataMillis = 0;
unsigned long lastCcResetMillis = 0;
uint16_t last_rain_raw = 0;
bool first_rain_reading = true;

uint8_t crc8(uint8_t const message[], unsigned nBytes, uint8_t polynomial, uint8_t init) {
  uint8_t remainder = init;
  for (unsigned byte = 0; byte < nBytes; ++byte) {
    remainder ^= message[byte];
    for (unsigned bit = 0; bit < 8; ++bit) {
      remainder = (remainder & 0x80) ? (remainder << 1) ^ polynomial : (remainder << 1);
    }
  }
  return remainder;
}

void decodeFineOffset(uint8_t* b) {
  uint8_t crc = crc8(&b[1], 15, 0x31, 0x00);
  uint8_t checksum = 0;
  for (int n = 1; n < 17; ++n) checksum += b[n];
  
  if (crc != b[16] || checksum != b[17]) {
    Serial.printf("[WS3500] Scartato per CRC/Sum errati (CRC calc:%02X ric:%02X | Sum calc:%02X ric:%02X)\n", crc, b[16], checksum, b[17]);
    return;
  }

  float current_temp = (((b[4] & 0x07) << 8 | b[5]) - 400) * 0.1f;
  int current_hum = b[6];
  if (current_hum < 0 || current_hum > 100 || current_temp < -50 || current_temp > 80) return;

  int wind_dir = b[3] | (b[4] & 0x80) << 1;
  float current_wind = (b[7] | (b[4] & 0x10) << 4) * 0.125f * 0.51f;
  float current_gust = b[8] * 0.51f;

  int uv_raw = b[11] << 8 | b[12];
  int light_raw = b[13] << 16 | b[14] << 8 | b[15];
  float current_lux = light_raw * 0.1f;

  int uvi_upper[] = { 432, 851, 1210, 1570, 2017, 2450, 2761, 3100, 3512, 3918, 4277, 4650, 5029 };
  int current_uv_index = 0;
  while (current_uv_index < 13 && uvi_upper[current_uv_index] < uv_raw) ++current_uv_index;

  int low_bat = (b[4] & 0x08) >> 3;

  uint16_t current_rain_raw = (b[9] << 8 | b[10]);
  float rain_total_mm = current_rain_raw * 0.254f;
  float rain_delta = 0.0f;
  if (!first_rain_reading && current_rain_raw != last_rain_raw) {
    rain_delta = (current_rain_raw < last_rain_raw) ?
                 (65536 - last_rain_raw + current_rain_raw) * 0.254f :
                 (current_rain_raw - last_rain_raw) * 0.254f;
  }
  last_rain_raw = current_rain_raw;
  first_rain_reading = false;
  lastMeteoDataMillis = millis();

  Serial.println("\n================ SAINLOGIC WS3500 OK ================");
  Serial.printf("Temperatura : %.1f °C | Umidità: %d %%\n", current_temp, current_hum);
  Serial.printf("Vento Medio : %.2f m/s (%.1f km/h) | Dir: %d°\n", current_wind, current_wind * 3.6f, wind_dir);
  Serial.printf("Raffiche    : %.2f m/s (%.1f km/h)\n", current_gust, current_gust * 3.6f);
  Serial.printf("Pioggia     : Tot %.1f mm (Delta: +%.2f mm)\n", rain_total_mm, rain_delta);
  Serial.printf("Luce / UV   : %.1f Lux | UV Index: %d (raw: %d)\n", current_lux, current_uv_index, uv_raw);
  Serial.printf("Batteria    : %s | RSSI: %d dBm\n", low_bat ? "SCARICA" : "OK", ELECHOUSE_cc1101.getRssi());
  Serial.println("=====================================================");
}

void configCC1101() {
  ELECHOUSE_cc1101.setSpiPin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, CC1101_CS_PIN);
  ELECHOUSE_cc1101.setGDO(CC1101_GDO0_PIN, CC1101_GDO2_PIN);
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setModulation(0);      // 2-FSK
  ELECHOUSE_cc1101.setMHZ(868.3);
  ELECHOUSE_cc1101.setDRate(17.24);
  ELECHOUSE_cc1101.setRxBW(250.0);
  ELECHOUSE_cc1101.setDeviation(40.0);
  ELECHOUSE_cc1101.setPacketLength(27);
  ELECHOUSE_cc1101.setSyncMode(2);        // 16/16 sync word bits detected
  ELECHOUSE_cc1101.setSyncWord(0xAA, 0x2D);
  ELECHOUSE_cc1101.setManchester(0);
  ELECHOUSE_cc1101.setPktFormat(0);
  ELECHOUSE_cc1101.setLengthConfig(0);    // Fixed packet length
  ELECHOUSE_cc1101.setCrc(0);
  ELECHOUSE_cc1101.setAdrChk(0);
  ELECHOUSE_cc1101.SetRx();
}

void setup_test_cc1101() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n--- TEST CC1101 (ELECHOUSE DRV) PER WS3500 ---");

  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);

  configCC1101();

  if (ELECHOUSE_cc1101.getCC1101()) {
    Serial.println("[CC1101] Connessione SPI OK! In ascolto su 868.3 MHz...");
  } else {
    Serial.println("[CC1101] ERRORE DI CONNESSIONE SPI! Verifica i cablaggi.");
    while (true) { delay(1000); }
  }

  unsigned long now = millis();
  lastMeteoDataMillis = now;
  lastCcResetMillis = now;
}

void loop_test_cc1101() {
  unsigned long now = millis();

  // Reset periodico FIFO ogni 5 minuti
  if ((int32_t)(now - lastCcResetMillis) > 300000) {
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SFRX);
    ELECHOUSE_cc1101.SetRx();
    lastCcResetMillis = now;
  }

  // Re-init completo se non riceve dati per 20 minuti
  if ((int32_t)(now - lastMeteoDataMillis) > 1200000) {
    configCC1101();
    lastMeteoDataMillis = now;
  }

  uint8_t rxb = ELECHOUSE_cc1101.SpiReadStatus(CC1101_RXBYTES);
  
  // Se c'è overflow nella FIFO (bit 7 alto), svuota e riparti
  if (rxb & 0x80) {
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SFRX);
    ELECHOUSE_cc1101.SetRx();
  } 
  else if ((rxb & 0x7F) >= 27) {
    uint8_t buf[27];
    ELECHOUSE_cc1101.SpiReadBurstReg(CC1101_RXFIFO, buf, 27);

    Serial.print("[RX FIFO] Raw HEX: ");
    for (int i = 0; i < 20; i++) Serial.printf("%02X ", buf[i]);
    Serial.println();

    if (buf[0] == 0xD4 && buf[1] == 0x24) {
      decodeFineOffset(buf);
    } else if (buf[0] == 0x24) {
      uint8_t shiftedBuf[28];
      shiftedBuf[0] = 0x00;
      memcpy(&shiftedBuf[1], buf, 27);
      decodeFineOffset(shiftedBuf);
    }

    ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
    ELECHOUSE_cc1101.SpiStrobe(CC1101_SFRX);
    ELECHOUSE_cc1101.SetRx();
  }

  delay(10);
}
