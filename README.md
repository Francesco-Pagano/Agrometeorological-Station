# Agrometeorological Station

![IoT](https://img.shields.io/badge/IoT-Project-blue) ![Raspberry Pi](https://img.shields.io/badge/-Raspberry_Pi-C51A4A?logo=Raspberry-Pi&logoColor=white) ![ESP32](https://img.shields.io/badge/-ESP32-E7352C?logo=Espressif&logoColor=white) ![ESP8266](https://img.shields.io/badge/-ESP8266-000000?logo=Espressif&logoColor=white) ![LoRaWAN](https://img.shields.io/badge/LoRa-Communication-007AFF) ![ThingsBoard](https://img.shields.io/badge/ThingsBoard-Dashboard-0B2F83) ![EasyEDA](https://img.shields.io/badge/EasyEDA-PCB_Design-brightgreen)

Welcome to the repository of my **Agrometeorological Station**.<br>
This project documents the evolution of the system, showing the transition from handmade DIY prototypes to the design of a custom stand-alone Printed Circuit Board (PCB).

<p align="center">
  <img src="images/Main.jpg" width="350" alt="Main Project Overview">
</p>

---

## Data Visualization
All versions of this station send telemetry data to a server.
I developed an interactive dashboard on **ThingsBoard** for real-time monitoring of environmental and agricultural parameters.

<p align="center">
  <img src="images/Dashboard_Thingsboard.jpg" width="800" alt="ThingsBoard Dashboard">
</p>

---

## The Hardware & Software Journey

The project is structured into three iterations, each focused on a specific improvement: computing power, off-grid autonomy, and total hardware integration.

### [Version 1: The Foundations (Raspberry Pi 4)](v1/)
The initial goal was to build a first, fully functional working prototype of a Weather Station using the Raspberry Pi 4 as the core platform.

* **Source Code:** [Python scripts](v1/code/)
* **Hardware Design:** [EasyEDA/Gerber Files](v1/EasyEDA_Files/) and [PDF Schematic](v1/Schematic_ShieldPi4_WS.pdf)

<p align="center">
  <img src="v1/img/Schematic_ShieldPi4_WS.png" height="220" alt="V1 Schematic">
  <img src="v1/img/3D_ShieldPi4_WS.png" height="220" alt="V1 3D Render">
  <img src="v1/img/PCB.png" height="220" alt="V1 Assembled PCB">
  <br><i>From left: Schematic, 3D Render, and assembled PCB on RPi4.</i>
</p>

---

### [Version 2: Miniaturization & Field Deployment (RPi Zero)](v2/)
In the second version, I focused on energy optimization and physical robustness for field deployment.<br>
I switched to the **Raspberry Pi Zero W** and introduced standard **RJ11** connectors for commercial weather sensors (plug-and-play).

* **Source Code:** [Python scripts](v2/code/)
* **Hardware Design:** [EasyEDA/Gerber Files](v2/EasyEDA_Files/) and [PDF Schematic](v2/Schematic_ShieldPi0_WS.pdf)

<p align="center">
  <img src="v2/img/Schematic_ShieldPi0_WS.png" height="220" alt="V2 Schematic">
  <img src="v2/img/3D_ShieldPi0_WS.png" height="220" alt="V2 3D Render">
  <img src="v2/img/PCB.jpg" height="220" alt="V2 Deployed">
  <br><i>From left: Schematic, 3D Render, and assembled PCB.</i>
</p>

---

### [Version 3: Industrial IoT, LoRa & Solar Autonomy](v3/)
The third iteration introduces long-range communication and off-grid energy independence.<br>
The system is powered by a solar panel with an **MPPT** charge controller. I used two **Heltec LoRa 32 v2** (ESP32) modules for radio transmission and integrated a leaf wetness sensor via **RS485**.

* **Firmware Code:** [C++ sketches](v3/code/)
* **Hardware Design:** [EasyEDA/Gerber Files](v3/EasyEDA_Files/) and [PDF Schematic](v3/Schematic_ShieldHeltec_WS.pdf)

<p align="center">
  <img src="v3/img/Schematic_ShieldHeltec_WS.png" height="220" alt="V3 Schematic">
  <img src="v3/img/3D_ShieldHeltec_WS.png" height="220" alt="V3 3D Render">
  <img src="v3/img/PCB.png" height="220" alt="V3 Final Assembly">
  <br><i>From left: Schematic, 3D Render, and final Enclosure.</i>
</p>

---

### [Version 4: The Stand-Alone Custom PCB (WIP)](v4/)
The state of the art and the natural evolution of the project.<br>
In this version, currently under development, I am abandoning the concept of a "Shield" connected to a third-party dev-board. <br>
I designed a single printed circuit board that directly integrates the **ESP8266** SoC (ESP-12F module).<br>
This "all-in-one" approach drastically reduces production costs, power consumption, and physical footprint, representing a true hardware product ready for industrialization.

* **Hardware Design:** [Work In Progress - EasyEDA Files](v4/EasyEDA_Files/)
* **Schematics:** [PDF of the WIP schematic](v4/Schematic_ESP8266_WS.pdf)

<p align="center">
  <img src="v4/img/Schematic_ESP8266_WS.png" height="400" alt="V4 Schematic">
  &nbsp;&nbsp;&nbsp;&nbsp;
  <img src="v4/img/3D_ESP8266_WS.png" height="400" alt="V4 ESP8266 Standalone PCB">
  <br><i>Left: Schematic. Right: 3D Render of the PCB.</i>
</p>

---

## Author
**Francesco Pagano**
* [LinkedIn](https://www.linkedin.com/in/francescopagano-/)
* [Website](http://www.fpagano.com)