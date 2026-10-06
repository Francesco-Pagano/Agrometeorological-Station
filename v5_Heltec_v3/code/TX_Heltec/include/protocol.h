#pragma once
#include <stdint.h>

#define PROTOCOL_MAGIC 0xAA
#define PROTOCOL_VERSION 0x02

struct __attribute__((packed)) LoRaHeader {
  uint8_t magic;      // 1 byte (0xAA)
  uint8_t version;    // 1 byte (0x02)
  uint8_t node_id;    // 1 byte
  uint16_t seq_num;   // 2 byte
  uint8_t flags;      // 1 byte (bit 0: is_historical)
};                    // Totale Header: 6 byte

struct __attribute__((packed)) SensorData {
  uint32_t timestamp; // 4 byte
  float temp;         // 4 byte
  int hum;            // 4 byte
  float wind_avg;     // 4 byte
  float wind_gust;    // 4 byte
  int wind_dir;       // 4 byte
  float rain_mm;      // 4 byte
  float lux;          // 4 byte
  int uv_index;       // 4 byte
  float leaf_temp;    // 4 byte
  float leaf_wet;     // 4 byte
  int8_t low_battery; // 1 byte (-1: invalid/unknown, 0: ok, 1: low)
};                    // Totale Data: 45 byte

struct __attribute__((packed)) LoRaPacket {
  LoRaHeader header;  // 6 byte
  SensorData data;    // 45 byte
};                    // Totale Pacchetto: 51 byte

static_assert(sizeof(SensorData) == 45, "Errore allineamento SensorData. Dimensione attesa: 45 byte");
static_assert(sizeof(LoRaPacket) == 51, "Errore allineamento LoRaPacket. Dimensione attesa: 51 byte");

struct __attribute__((packed)) AckPacket {
  uint8_t magic;      // 1 byte
  uint16_t seq_num;   // 2 byte
  uint32_t ntp_epoch; // 4 byte
};                    // Totale ACK: 7 byte

static_assert(sizeof(LoRaHeader) == 6, "Errore allineamento LoRaHeader");
static_assert(sizeof(AckPacket) == 7, "Errore allineamento AckPacket");