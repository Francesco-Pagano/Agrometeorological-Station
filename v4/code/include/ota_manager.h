#pragma once

// Configura ArduinoOTA (hostname, password). Da chiamare in setup().
void otaBegin();

// Da chiamare ad ogni loop() quando il WiFi e' connesso.
void otaHandle();
