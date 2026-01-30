Overview

This project contains two scripts for serial communication between an Android app and an ESP32 to control on-device graphics.

1. ESP32 Graphics Script

Runs on the ESP32 and listens for incoming serial commands. Based on the received signal, it updates the display using the ArduinoGFX library. The graphics logic can be customized to render different visuals or animations.

Uses ArduinoGFX for display rendering

Triggers graphics based on serial input

Easily customizable for different display use cases

2. Android Serial Control App (serialcomapp)

A simple Android app that sends serial commands to the ESP32 when UI buttons are pressed. This can be extended to support additional commands or more complex UI interactions.

Sends serial signals on button press

Simple UI for testing and control

Easily extendable for more commands or controls
