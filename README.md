## Overview

This project contains two scripts for serial communication between an Android app and an ESP32 to control on-device graphics.

### ESP32 Graphics Script

- Runs on the ESP32 and listens for incoming serial commands
- Updates the display based on received serial signals
- Uses the ArduinoGFX library for rendering
- Graphics and animations can be customized
- Extremely useful site: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75?srsltid=AfmBOoo0XXboYLa7aAQUxWXshpvcOcL-91DwrRC2b-E4TUDviT9IMh3N#Resources

### Android Serial Control App (serialcomapp)

- Sends serial commands to the ESP32 on button press
- Simple UI for testing and control
- Easily extendable for additional commands and controls
