#pragma once

// Avvia il web server di debug su porta 80. Da chiamare in setup(),
// dopo che il WiFi e' connesso.
void debugWebServerBegin();

// Da chiamare ad ogni loop(), sempre (anche senza WiFi/MQTT), per
// servire le richieste HTTP in arrivo.
void debugWebServerHandle();
