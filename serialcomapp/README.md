# ESP32 Serial Controller App

A simple Android app that sends serial messages (1, 2, 3, 4) to an ESP32 via USB.

## Features
- Connect to ESP32 via USB OTG
- Four buttons to send serial values 1, 2, 3, 4
- Real-time connection status
- Automatic USB device detection
- Baud rate: 115200

## Usage

1. **Connect ESP32 to Android device** using a USB OTG cable
2. **Launch the app** - it will automatically detect the ESP32
3. **Grant USB permission** when prompted
4. **Press buttons** to send serial values (1, 2, 3, or 4)

## ESP32 Setup

Upload this simple code to your ESP32 to test:

```cpp
void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
  if (Serial.available() > 0) {
    char received = Serial.read();
    Serial.print("Received: ");
    Serial.println(received);
    
    // Example: blink LED based on received value
    switch(received) {
      case '1':
        digitalWrite(LED_BUILTIN, HIGH);
        delay(100);
        digitalWrite(LED_BUILTIN, LOW);
        break;
      case '2':
        for(int i=0; i<2; i++) {
          digitalWrite(LED_BUILTIN, HIGH);
          delay(100);
          digitalWrite(LED_BUILTIN, LOW);
          delay(100);
        }
        break;
      case '3':
        for(int i=0; i<3; i++) {
          digitalWrite(LED_BUILTIN, HIGH);
          delay(100);
          digitalWrite(LED_BUILTIN, LOW);
          delay(100);
        }
        break;
      case '4':
        for(int i=0; i<4; i++) {
          digitalWrite(LED_BUILTIN, HIGH);
          delay(100);
          digitalWrite(LED_BUILTIN, LOW);
          delay(100);
        }
        break;
    }
  }
}
```

## Permissions

The app requires the following permissions:
- `android.hardware.usb.host` - For USB host mode
- `android.permission.USB_PERMISSION` - For USB device access

## Dependencies

- `com.github.mik3y:usb-serial-for-android:3.7.0` - USB serial communication library

## Technical Details

- **Min SDK**: 24 (Android 7.0)
- **Target SDK**: 35 (Android 15)
- **Baud Rate**: 115200
- **Data Bits**: 8
- **Stop Bits**: 1
- **Parity**: None

## Building

```bash
./gradlew assembleDebug
```

The APK will be generated at: `app/build/outputs/apk/debug/app-debug.apk`

## Installation

```bash
./gradlew installDebug
```

Or install manually:
```bash
adb install app/build/outputs/apk/debug/app-debug.apk
```
