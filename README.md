## Overview

This project contains two scripts for serial communication between an Android app and an ESP32 to control on-device graphics.

### ESP32 Graphics Script

- Runs on the ESP32 and listens for incoming serial commands
- Updates the display based on received serial signals
- Uses the ArduinoGFX library for rendering
- Graphics and animations can be customized

### Android Serial Control App (serialcomapp)

- Sends serial commands to the ESP32 on button press
- Simple UI for testing and control
- Easily extendable for additional commands and controls
