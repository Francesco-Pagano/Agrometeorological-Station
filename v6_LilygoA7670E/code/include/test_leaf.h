#pragma once
#include <Arduino.h>
#include <ModbusMaster.h>
#include "config.h"

ModbusMaster nodeLeaf;

void setup_test_leaf() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  nodeLeaf.begin(LEAF_SENSOR_ID, Serial2);
  Serial.println("\n--- TEST SENSORE FOGLIA AVVIATO ---");
}

void loop_test_leaf() {
  uint8_t result = nodeLeaf.readHoldingRegisters(0x0000, 2);
  
  if (result == nodeLeaf.ku8MBSuccess) {
    float temp = (int16_t)nodeLeaf.getResponseBuffer(0) / 100.0;
    float wetness = nodeLeaf.getResponseBuffer(1) / 100.0;
    
    Serial.printf("[FOGLIA] Temp: %.2f °C | Bagnatura: %.2f %%\n", temp, wetness);
  } else {
    Serial.printf("[FOGLIA] Errore lettura Modbus: 0x%02X\n", result);
  }
  delay(2000);
}
