/*
 * WiFi JPEG Image Viewer — Waveshare ESP32-S3-Touch-LCD-3.5B
 * AXS15231B 320×480 LCD, landscape (480×320) via setRotation(1).
 *
 * Downloads JPEG images from the internet and displays them.
 * Send '0'–'4' over Serial (115200) to switch images.
 * BOOT button (GPIO 0) cycles to the next image.
 *
 * REQUIRED SETUP:
 *  1. Fill in WIFI_SSID and WIFI_PASSWORD below.
 *  2. Install the "JPEGDEC" library by bitbank2:
 *     Arduino IDE → Library Manager → search "JPEGDEC" → Install.
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <JPEGDEC.h>

// ---------- WiFi credentials — EDIT THESE ----------
#define WIFI_SSID     "YOUR_SSID"
#define WIFI_PASSWORD "YOUR_PASSWORD"

// ---------- Image URLs (480×320 landscape JPEGs) ----------
// These are stable images from picsum.photos (Unsplash CDN).
// To swap in your own images, replace any URL with a direct JPEG link.
#define NUM_IMAGES 5
const char *imageUrls[NUM_IMAGES] = {
  "https://picsum.photos/id/1/480/320",    // 0: woman looking into sky
  "https://picsum.photos/id/10/480/320",   // 1: mountain fog
  "https://picsum.photos/id/20/480/320",   // 2: boats in harbour
  "https://picsum.photos/id/30/480/320",   // 3: city buildings
  "https://picsum.photos/id/40/480/320",   // 4: tree canopy
};

// ---------- Pin definitions (Waveshare ESP32-S3-Touch-LCD-3.5B) ----------
#define LCD_CS   39
#define LCD_CLK  40
#define LCD_D0   41
#define LCD_D1   42
#define LCD_D2   45
#define LCD_D3   46
// Backlight: drive BOTH candidates; only the correct pin has effect.
#define LCD_BL_A  6    // original probe BL pin
#define LCD_BL_B  48   // Waveshare wiki BL pin
// RST is driven manually in setup(); GFX_NOT_DEFINED stops the library repeating it.
#define LCD_RST  38

#define BOOT_PIN  0

// ---------- Display ----------
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_CLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);

Arduino_AXS15231B *gfx = new Arduino_AXS15231B(
  bus,
  GFX_NOT_DEFINED,   // RST handled manually below
  0,
  false,
  320,
  480,
  0, 0, 0, 0,
  axs15231b_320480_type2_init_operations,
  sizeof(axs15231b_320480_type2_init_operations)
);

// ---------- JPEG decoder ----------
JPEGDEC jpeg;

// Callback called by JPEGDEC for each decoded MCU block.
// RGB565_BIG_ENDIAN pixel type + draw16bitBeRGBBitmap = correct colours.
int jpegDrawCallback(JPEGDRAW *pDraw) {
  gfx->draw16bitBeRGBBitmap(
    pDraw->x, pDraw->y,
    pDraw->pPixels,
    pDraw->iWidth, pDraw->iHeight);
  return 1; // continue decoding
}

// ---------- State ----------
int  currentImage = 0;
bool lastBoot     = true;
uint32_t lastDebounce = 0;

// ---------- Helpers ----------
// Show up to two lines of white text on a black background.
void showStatus(const char *line1, const char *line2 = nullptr) {
  gfx->fillScreen(0x0000);
  gfx->setTextSize(2);
  gfx->setTextColor(0xFFFF);
  gfx->setCursor(10, 10);
  gfx->println(line1);
  if (line2) {
    gfx->setTextSize(1);
    gfx->setCursor(10, 44);
    gfx->println(line2);
  }
}

// ---------- Core: fetch a JPEG from a URL and display it ----------
void showImage(int idx) {
  if (idx < 0 || idx >= NUM_IMAGES) return;
  currentImage = idx;

  const char *url = imageUrls[idx];
  Serial.printf("[%d] %s\n", idx, url);
  // Show brief loading screen (skip "https://" prefix to save space)
  showStatus("Fetching...", url + 8);

  // --- Download ---
  WiFiClientSecure client;
  client.setInsecure();  // skip certificate validation for simplicity

  HTTPClient http;
  http.begin(client, url);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  int code = http.GET();
  Serial.printf("HTTP %d\n", code);
  if (code != HTTP_CODE_OK) {
    char msg[32];
    snprintf(msg, sizeof(msg), "HTTP error: %d", code);
    showStatus("Fetch failed", msg);
    http.end();
    return;
  }

  int contentLen = http.getSize();  // -1 if chunked / unknown
  Serial.printf("Content-Length: %d\n", contentLen);

  // Allocate receive buffer — prefer PSRAM (8 MB on ESP32-S3), fall back to heap
  size_t bufSize = (contentLen > 0) ? (size_t)contentLen : 300000u;
  uint8_t *buf = (uint8_t *)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t *)malloc(bufSize);
  if (!buf) {
    showStatus("Out of memory!", "Enable PSRAM in Arduino IDE");
    Serial.println("ERROR: could not allocate image buffer");
    http.end();
    return;
  }

  // Read response body into buffer
  WiFiClient *stream = http.getStreamPtr();
  size_t total = 0;
  uint32_t deadline = millis() + 15000;
  while (millis() < deadline && total < bufSize) {
    size_t avail = stream->available();
    if (avail > 0) {
      total += stream->readBytes(buf + total,
                                 min(avail, bufSize - total));
    } else if (!stream->connected()) {
      break;
    } else {
      delay(1);
    }
    if (contentLen > 0 && total >= (size_t)contentLen) break;
  }
  http.end();
  Serial.printf("Downloaded %u bytes\n", (unsigned)total);

  if (total == 0) {
    free(buf);
    showStatus("No data received");
    return;
  }

  // --- Decode and display ---
  if (jpeg.openRAM(buf, (int)total, jpegDrawCallback)) {
    jpeg.setPixelType(RGB565_BIG_ENDIAN);  // matches draw16bitBeRGBBitmap
    gfx->startWrite();
    jpeg.decode(0, 0, JPEG_AUTO_ROTATE);   // honour EXIF orientation
    gfx->endWrite();
    jpeg.close();
    Serial.println("Image displayed OK");
  } else {
    showStatus("JPEG decode failed",
               "Is the download a valid JPEG?");
    Serial.println("ERROR: jpeg.openRAM() failed");
  }

  free(buf);
}

// ---------- Input polling ----------
void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c >= '0' && c < '0' + NUM_IMAGES) {
      showImage(c - '0');
    }
  }
}

void pollBootButton() {
  bool b = digitalRead(BOOT_PIN); // HIGH idle, LOW pressed
  if (b != lastBoot) lastDebounce = millis();
  if (millis() - lastDebounce > 40) {
    if (lastBoot == true && b == false) {
      showImage((currentImage + 1) % NUM_IMAGES);
    }
  }
  lastBoot = b;
}

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("=== WiFi Image Viewer booting ===");

  // Backlight — enable both candidates; only the correct pin has effect
  pinMode(LCD_BL_A, OUTPUT); digitalWrite(LCD_BL_A, HIGH);
  pinMode(LCD_BL_B, OUTPUT); digitalWrite(LCD_BL_B, HIGH);
  Serial.println("Backlight HIGH on GPIO 6 + GPIO 48");

  // Hardware reset — 100 ms LOW then 200 ms settle
  pinMode(LCD_RST, OUTPUT);
  digitalWrite(LCD_RST, HIGH); delay(10);
  digitalWrite(LCD_RST, LOW);  delay(100);
  digitalWrite(LCD_RST, HIGH); delay(200);
  Serial.println("HW reset done");

  // Display driver init
  if (!gfx->begin()) {
    Serial.println("ERROR: gfx->begin() failed! Check QSPI wiring.");
    delay(5000);
    ESP.restart();
  }
  Serial.printf("Display OK  native: %dx%d\n", gfx->width(), gfx->height());

  // Quick colour flash — if you see red then green the display hardware works
  gfx->fillScreen(0xF800); delay(500);   // RED
  gfx->fillScreen(0x07E0); delay(500);   // GREEN

  gfx->setRotation(1);  // landscape: 480×320
  Serial.printf("After setRotation(1): %dx%d\n", gfx->width(), gfx->height());

  // WiFi
  showStatus("Connecting WiFi...", WIFI_SSID);
  Serial.printf("Connecting to %s ...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    showStatus("WiFi failed!", "Check SSID / password in sketch");
    Serial.println("ERROR: WiFi connection failed — halting");
    while (true) delay(1000);  // let user read the on-screen message
  }
  Serial.printf("WiFi OK  IP: %s\n", WiFi.localIP().toString().c_str());

  pinMode(BOOT_PIN, INPUT_PULLUP);

  Serial.println("Send 0-4 to switch images | BOOT button cycles");
  showImage(0);
  Serial.println("=== Boot complete ===");
}

// ---------- loop ----------
void loop() {
  pollSerial();
  pollBootButton();
  delay(20);
}
