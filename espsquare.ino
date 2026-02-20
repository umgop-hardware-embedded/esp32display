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

// ---------- Direct pixel-write helpers (opcode 0x02 path) ----------
//
// The Arduino_GFX QSPI bus driver uses SPI opcode 0x32 (QIO mode) for all
// pixel data writes (fillScreen, drawBitmap, etc.).  On some AXS15231B panel
// revisions this opcode is ignored, leaving the random startup GRAM visible.
//
// The batchOperation init path uses opcode 0x02 via writeC8Bytes — and we
// KNOW that path reaches the display (the panel powers on correctly).  These
// helpers bypass the gfx drawing layer and write pixels the same way.
//
// pxbuf must live in internal SRAM (not flash) so SPI DMA can read it.
static uint8_t DRAM_ATTR pxbuf[8192]; // 4096 pixels × 2 bytes (RGB565) per chunk

// Set column / row address window on the display.
static void setWindowRaw(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
  uint8_t ca[4] = { (uint8_t)(x1 >> 8), (uint8_t)x1,
                    (uint8_t)(x2 >> 8), (uint8_t)x2 };
  uint8_t ra[4] = { (uint8_t)(y1 >> 8), (uint8_t)y1,
                    (uint8_t)(y2 >> 8), (uint8_t)y2 };
  bus->beginWrite();
  bus->writeC8Bytes(0x2A, ca, 4);   // CASET
  bus->writeC8Bytes(0x2B, ra, 4);   // RASET
  bus->endWrite();
}

// Write raw big-endian RGB565 bytes to the current window.
// Copies into pxbuf first so SPI DMA always has SRAM-backed data.
static void writePixelsRaw(const uint8_t *data, uint32_t byteLen) {
  const uint8_t *p = data;
  bool first = true;
  while (byteLen > 0) {
    uint32_t chunk = (byteLen > sizeof(pxbuf)) ? sizeof(pxbuf) : byteLen;
    memcpy(pxbuf, p, chunk);
    bus->beginWrite();
    // 0x2C = RAMWR (first chunk); 0x3C = Memory Write Continue (subsequent)
    bus->writeC8Bytes(first ? 0x2C : 0x3C, pxbuf, chunk);
    bus->endWrite();
    p       += chunk;
    byteLen -= chunk;
    first    = false;
  }
}

// Fill the entire display with one colour (replaces gfx->fillScreen).
static void fillDisplayRaw(uint16_t color) {
  const uint8_t hi = color >> 8, lo = color & 0xFF;
  for (uint32_t i = 0; i < sizeof(pxbuf); i += 2) {
    pxbuf[i]     = hi;
    pxbuf[i + 1] = lo;
  }
  const uint16_t W = gfx->width(), H = gfx->height();
  setWindowRaw(0, 0, W - 1, H - 1);
  uint32_t remaining = (uint32_t)W * H;
  bool first = true;
  while (remaining > 0) {
    uint32_t chunk = (remaining > sizeof(pxbuf) / 2) ? sizeof(pxbuf) / 2 : remaining;
    bus->beginWrite();
    bus->writeC8Bytes(first ? 0x2C : 0x3C, pxbuf, chunk * 2);
    bus->endWrite();
    remaining -= chunk;
    first      = false;
  }
}

// ---------- JPEG decoder ----------
JPEGDEC jpeg;

// JPEGDEC calls this for each decoded MCU block.
// Uses the opcode-0x02 write path so the pixels actually reach the display.
int jpegDrawCallback(JPEGDRAW *pDraw) {
  setWindowRaw(pDraw->x, pDraw->y,
               pDraw->x + pDraw->iWidth  - 1,
               pDraw->y + pDraw->iHeight - 1);
  writePixelsRaw((const uint8_t *)pDraw->pPixels,
                 (uint32_t)pDraw->iWidth * pDraw->iHeight * 2);
  return 1;
}

// ---------- State ----------
int  currentImage = 0;
bool lastBoot     = true;
uint32_t lastDebounce = 0;

// ---------- Helpers ----------
// Print status to Serial. Also clears the display to black so the user
// can see when a new operation starts (even if text rendering still fails).
void showStatus(const char *line1, const char *line2 = nullptr) {
  fillDisplayRaw(0x0000);   // black — uses the working opcode-0x02 path
  Serial.println(line1);
  if (line2) Serial.println(line2);
  // Note: gfx text calls (gfx->println) use the opcode-0x32 path that may
  // not work on this panel.  Status is available in Serial Monitor instead.
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
    // RGB565_BIG_ENDIAN: JPEGDEC outputs high-byte first, matching
    // our writePixelsRaw which sends bytes to the display as-is.
    jpeg.setPixelType(RGB565_BIG_ENDIAN);
    jpeg.decode(0, 0, JPEG_AUTO_ROTATE);   // honour EXIF orientation
    jpeg.close();
    Serial.println("Image displayed OK");
  } else {
    showStatus("JPEG decode failed");
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

  // Force 16-bit RGB565 colour mode.  The type2 init sequence omits COLMOD
  // so the chip may default to 18- or 24-bit mode, causing colour corruption.
  bus->beginWrite();
  bus->writeC8D8(0x3A, 0x55);  // COLMOD: 0x55 = 16 bpp RGB565
  bus->endWrite();
  delay(10);
  Serial.println("COLMOD set to 0x55 (16-bit RGB565)");

  // Apply landscape rotation BEFORE any drawing so gfx->width()/height()
  // report the correct 480x320 dimensions for fillDisplayRaw.
  gfx->setRotation(1);
  Serial.printf("After setRotation(1): %dx%d\n", gfx->width(), gfx->height());

  // Colour flash — uses the opcode-0x02 pixel path.
  // If you see RED then GREEN the display hardware and pixel writes are working.
  fillDisplayRaw(0xF800); delay(500);   // RED
  fillDisplayRaw(0x07E0); delay(500);   // GREEN
  Serial.println("Colour flash done");

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
    Serial.println("ERROR: WiFi connection failed");
    Serial.println("Fix WIFI_SSID / WIFI_PASSWORD, then reboot.");
    delay(10000);
    ESP.restart();  // restart so user can reflash with correct credentials
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
