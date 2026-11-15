#pragma once
#include <Arduino.h>
#include <ModbusMaster.h>
#include "config.h"

ModbusMaster nodeSoil;

void setup_test_soil() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  nodeSoil.begin(SOIL_SENSOR_ID, Serial2);
  Serial.println("\n--- TEST SENSORE TERRENO AVVIATO ---");
}

void loop_test_soil() {
  // Leggi solo 2 registri: Temp (0) e Moisture (1)
  uint8_t result = nodeSoil.readHoldingRegisters(0x0000, 2); 
  
  if (result == nodeSoil.ku8MBSuccess) {
    float temp = (int16_t)nodeSoil.getResponseBuffer(0) / 10.0;
    float moisture = nodeSoil.getResponseBuffer(1) / 10.0;
    
    Serial.printf("[TERRENO] Temp: %.1f °C | Umidità: %.1f %%\n", temp, moisture);
  } else {
    Serial.printf("[TERRENO] Errore lettura Modbus: 0x%02X\n", result);
  }
  delay(2000);
}
