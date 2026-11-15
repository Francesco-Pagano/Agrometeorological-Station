#include <Arduino.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>

#define CC_SCK  12
#define CC_MISO 13
#define CC_MOSI 23
#define CC_CSN  2
#define CC_GDO0 37
#define CC_GDO2 38

uint8_t pkt1[27] = {
  0xD4, 0x24, 0x25, 0x22, 0x82, 0x42, 0x48, 0x0F, 0x03, 0x03, 
  0xF3, 0x00, 0x39, 0x01, 0x2B, 0x38, 0xF8, 0x14, 0x20, 0x0F,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

uint8_t pkt2[27] = {
  0xD4, 0x24, 0x25, 0x06, 0x82, 0x41, 0x47, 0x0B, 0x02, 0x03, 
  0xF3, 0x00, 0x2F, 0x00, 0xF1, 0x36, 0x6E, 0x20, 0x20, 0x16,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

bool toggle = false;

void setup() {
  Serial.begin(115200);
  ELECHOUSE_cc1101.setSpiPin(CC_SCK, CC_MISO, CC_MOSI, CC_CSN);
  ELECHOUSE_cc1101.setGDO(CC_GDO0, CC_GDO2);
  ELECHOUSE_cc1101.Init();
  ELECHOUSE_cc1101.setModulation(0);      // 2-FSK
  ELECHOUSE_cc1101.setMHZ(868.3);
  ELECHOUSE_cc1101.setDRate(17.24);
  ELECHOUSE_cc1101.setDeviation(40.0);
  ELECHOUSE_cc1101.setPacketLength(27);
  ELECHOUSE_cc1101.setSyncMode(2);
  ELECHOUSE_cc1101.setSyncWord(0xAA, 0x2D);
  ELECHOUSE_cc1101.setManchester(0);
  ELECHOUSE_cc1101.setPktFormat(0);
  ELECHOUSE_cc1101.setLengthConfig(0);    // Lunghezza fissa
  ELECHOUSE_cc1101.setCrc(0);
  ELECHOUSE_cc1101.setPA(10);             // Potenza TX 10 dBm
  Serial.println("Emulatore WS3500 Avviato!");
}

void loop() {
  uint8_t* pktToSend = toggle ? pkt1 : pkt2;

  ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);
  ELECHOUSE_cc1101.SpiStrobe(CC1101_SFTX);
  ELECHOUSE_cc1101.SpiWriteBurstReg(CC1101_TXFIFO, pktToSend, 27);
  ELECHOUSE_cc1101.SpiStrobe(CC1101_STX);
  
  delay(50);
  ELECHOUSE_cc1101.SpiStrobe(CC1101_SIDLE);

  Serial.printf("Pacchetto %d trasmesso su 868.3 MHz!\n", toggle ? 1 : 2);
  toggle = !toggle;
  
  delay(16000); // Trasmette ogni 16 secondi
}
