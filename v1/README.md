# Version 1: Raspberry Pi 4 Foundations

This is the first iteration of the project, focused on building a fully functional Weather Station prototype using the Raspberry Pi 4 as the core platform.

## Technical Specifications
- **SBC:** Raspberry Pi 4 Model B.
- **ADC:** MCP3008 (10-bit) connected via **SPI** bus.
- **Prototyping:** Initial prototype made on a copper-clad board etched with ferric chloride, with traces drawn manually using a PCB marker.<br>
This was later followed by the design of a professional Shield.
- **Software:** Python scripts with data transmission via **MQTT** protocol.

## Folder Structure
- `code/`: Contains the acquisition logic (`MainWS.py`) and sensor management.
- `EasyEDA_Files/`: Project files for the Shield, including Gerber files for manufacturing.
- `img/`: Photographic documentation of the prototype and the final PCB.
- `Schematic_ShieldPi4_WS.pdf`: Complete electrical schematic of the system.

[⬅️ Back to Home](../README.md)