## Overview

This project controls graphics on a **Waveshare ESP32-S3-Touch-LCD-3.5B** (AXS15231B 320×480 LCD over QSPI) from a companion Android app via serial commands.

---

### espsquare.ino — WiFi JPEG Image Viewer

Fetches full-screen JPEG photos from the internet and displays them.  
Send `'0'`–`'4'` over the Serial Monitor (115200 baud) to switch images.  
Press the **BOOT** button (GPIO 0) to cycle to the next image.

#### Required setup

1. **Install library** — In Arduino IDE open *Library Manager* and install **JPEGDEC** by bitbank2.  
2. **Edit WiFi credentials** near the top of `espsquare.ino`:
   ```cpp
   #define WIFI_SSID     "YOUR_SSID"
   #define WIFI_PASSWORD "YOUR_PASSWORD"
   ```
3. **Board settings** (Arduino IDE → Tools):
   | Setting | Value |
   |---------|-------|
   | Board | ESP32S3 Dev Module |
   | Flash Mode | QIO 80MHz |
   | PSRAM | OPI PSRAM |
   | Partition Scheme | Huge APP (3MB No OTA) |
   | USB CDC On Boot | Enabled |
4. Upload and open Serial Monitor at **115200 baud**.

#### What you'll see

| Boot phase | Display | Serial |
|------------|---------|--------|
| Hardware init | RED flash → GREEN flash | Pin / dimension report |
| WiFi connecting | Black screen | Dot progress |
| Fetching image | Black + cyan progress bar | HTTP status, byte count |
| Image displayed | Full-screen photo | "Image displayed OK" |

#### Swapping images

Replace any URL in `imageUrls[]` with any direct JPEG link sized to **480×320 px** (landscape).  
The default set uses [picsum.photos](https://picsum.photos) (Unsplash CDN):

| Key | Image |
|-----|-------|
| `0` | Woman looking into sky |
| `1` | Mountain fog |
| `2` | Boats in harbour |
| `3` | City buildings |
| `4` | Tree canopy |

#### Hardware — Waveshare ESP32-S3-Touch-LCD-3.5B pin assignments

| Signal | GPIO |
|--------|------|
| LCD_CS | 39 |
| LCD_CLK | 40 |
| LCD_D0 | 41 |
| LCD_D1 | 42 |
| LCD_D2 | 45 |
| LCD_D3 | 46 |
| LCD_BL | 48 |
| LCD_RST | 38 |
| BOOT button | 0 |

---

### serialcomapp — Android Serial Control App

- Sends serial commands to the ESP32 on button press
- Simple UI for testing and control
- Easily extendable for additional commands and controls

---

### Background — why direct pixel writes?

The Arduino_GFX QSPI bus driver sends pixel data using SPI opcode **0x32** (QIO mode).  
Some AXS15231B panel revisions ignore this opcode, leaving the random startup GRAM visible (random colorful static).  
`espsquare.ino` bypasses the GFX drawing layer entirely and writes pixels using opcode **0x02** — the same path used by the working init sequence — via `setWindowRaw()` / `writePixelsRaw()` / `fillDisplayRaw()`.
