#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include <Wire.h>
#include "HWCDC.h"

// ---------- Display init ----------
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_GFX *gfx = new Arduino_CO5300(
  bus, LCD_RESET, 0, false, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0
);

// ---------- Helpers ----------
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------- Colors (safe names) ----------
const uint16_t COL_BG     = rgb565(5, 6, 10);
const uint16_t COL_WHITE  = rgb565(245, 248, 255);
const uint16_t COL_GRAY   = rgb565(120, 130, 150);
const uint16_t COL_DARK   = rgb565(18, 22, 32);
const uint16_t COL_BLACK  = rgb565(0, 0, 0);

// Neon palettes
const uint16_t COL_NEON1  = rgb565(0, 255, 220);
const uint16_t COL_RING1  = rgb565(0, 170, 160);
const uint16_t COL_GLOW1  = rgb565(0, 70, 90);

const uint16_t COL_NEON2  = rgb565(255, 60, 60);
const uint16_t COL_RING2  = rgb565(255, 140, 60);
const uint16_t COL_GLOW2  = rgb565(90, 10, 10);

// Grayscale palette for Mode 3 (realistic)
const uint16_t G0 = rgb565(0,0,0);
const uint16_t G1 = rgb565(25,25,25);
const uint16_t G2 = rgb565(55,55,55);
const uint16_t G3 = rgb565(90,90,90);
const uint16_t G4 = rgb565(140,140,140);
const uint16_t G5 = rgb565(200,200,200);
const uint16_t G6 = rgb565(245,245,245);

// Cute palette (Mode 4)
const uint16_t C_BG   = rgb565(8, 10, 14);
const uint16_t C_EYE  = rgb565(230, 235, 245);
const uint16_t C_LINE = rgb565(70, 90, 120);
const uint16_t C_ACC  = rgb565(120, 200, 255);

// ---------- Mode selection ----------
volatile int mode = 0; // 0..4

#define USE_BOOT_BUTTON 1
#if USE_BOOT_BUTTON
  #define BOOT_PIN 0
  bool lastBoot = true;
  uint32_t lastDebounce = 0;
#endif

static uint32_t eyes_t0 = 0;
static uint32_t real_t0 = 0;
static uint32_t cute_t0 = 0;

void resetTimers(int m){
  if (m == 1 || m == 2) eyes_t0 = 0;
  if (m == 3) real_t0 = 0;
  if (m == 4) cute_t0 = 0;
}

void setMode(int m) {
  mode = (m % 5 + 5) % 5;
  resetTimers(mode);
  USBSerial.print("Mode -> ");
  USBSerial.println(mode);
  gfx->fillScreen(COL_BG);
}

// ---------- Home ----------
void sceneHome() {
  gfx->fillScreen(COL_BG);
  gfx->setTextSize(2);
  gfx->setTextColor(COL_WHITE);
  gfx->setCursor(18, 22);
  gfx->println("Robot Eyes Modes");

  gfx->setTextSize(1);
  gfx->setTextColor(COL_GRAY);
  gfx->setCursor(18, 60);
  gfx->println("Send 0-4 in Serial Monitor");
#if USE_BOOT_BUTTON
  gfx->setCursor(18, 75);
  gfx->println("BOOT cycles modes");
#endif

  gfx->setTextColor(COL_NEON1);
  gfx->setCursor(18, 105); gfx->println("0: Home");
  gfx->setCursor(18, 120); gfx->println("1: Neon Friendly (cyan)");
  gfx->setCursor(18, 135); gfx->println("2: Neon Angry (red)");
  gfx->setCursor(18, 150); gfx->println("3: Realistic B/W");
  gfx->setCursor(18, 165); gfx->println("4: Cute Robot");
}

// ---------- Shared glow helper ----------
void glowCircle(int x, int y, int r, uint16_t outer, uint16_t mid, uint16_t inner) {
  for (int i = 8; i >= 1; --i) {
    uint16_t col = (i >= 6) ? outer : (i >= 3 ? mid : inner);
    gfx->drawCircle(x, y, r + i, col);
  }
}

// ---------- Mode 1/2: Neon eyes ----------
void drawEyelidsNeon(int ex, int ey, int r, float blink01, bool angry, uint16_t lidLine) {
  int cut = (int)(blink01 * (r + 4));
  if (cut > 0) {
    gfx->fillRect(ex - r - 10, ey - r - 12, 2*r + 20, cut + 12, COL_BG);
    gfx->fillRect(ex - r - 10, ey + r - cut, 2*r + 20, cut + 12, COL_BG);
  }
  if (blink01 < 0.95f) {
    int yTop = ey - r + 6;
    int slope = angry ? +10 : 0;
    gfx->drawLine(ex - r, yTop + slope/2, ex + r, yTop - slope/2, lidLine);
    gfx->drawLine(ex - r, yTop + slope/2 + 1, ex + r, yTop - slope/2 + 1, lidLine);
  }
}

void drawEyeNeon(int ex, int ey, int ringR, int eyeR, int irisR, int pupilR,
                 int gazeX, int gazeY, float blink01, bool angry,
                 uint16_t neon, uint16_t ring, uint16_t glowOuter, uint16_t glowMid) {
  glowCircle(ex, ey, ringR, glowOuter, glowMid, neon);
  gfx->fillCircle(ex, ey, ringR, ring);
  gfx->fillCircle(ex, ey, ringR - 7, COL_BG);

  gfx->fillCircle(ex, ey, eyeR, COL_DARK);

  gfx->fillCircle(ex, ey, irisR, neon);
  gfx->fillCircle(ex, ey, irisR - 9, rgb565(10, 14, 22));

  int px = ex + gazeX;
  int py = ey + gazeY;
  int maxOff = irisR / 2;
  int dx = px - ex, dy = py - ey;
  long d2 = (long)dx*dx + (long)dy*dy;
  long md = maxOff;
  if (d2 > md*md) {
    float sc = (float)md / sqrtf((float)d2);
    px = ex + (int)(dx * sc);
    py = ey + (int)(dy * sc);
  }

  gfx->fillCircle(px, py, pupilR + 6, rgb565(0, 35, 40));
  gfx->fillCircle(px, py, pupilR, COL_BLACK);

  // highlights
  gfx->fillCircle(ex - eyeR/3, ey - eyeR/3, max(2, eyeR/11), COL_WHITE);

  // eyelids + brows
  drawEyelidsNeon(ex, ey, eyeR, blink01, angry, rgb565(60, 70, 90));
  if (angry && blink01 < 0.9f) {
    int by = ey - eyeR - 15;
    gfx->drawLine(ex - eyeR, by, ex + eyeR/2, by + 12, neon);
    gfx->drawLine(ex - eyeR, by + 1, ex + eyeR/2, by + 13, neon);
  }
}

void sceneNeonEyes(bool angry) {
  if (eyes_t0 == 0) eyes_t0 = millis();
  uint32_t t = millis() - eyes_t0;

  int W = gfx->width(), H = gfx->height();
  int cx = W/2, cy = H/2;

  int ringR = min(W, H) / 5;
  int eyeR  = ringR - 12;
  int irisR = eyeR - 8;
  int pupilR = irisR / 3;

  int eyeDX = ringR + ringR/2;
  int eyeY  = cy;

  float sx = angry ? 0.0024f : 0.0014f;
  float sy = angry ? 0.0019f : 0.0011f;
  int gx = (int)((irisR/2) * sinf(t * sx));
  int gy = (int)((irisR/4) * sinf(t * sy));

  float period = angry ? 1.6f : 2.2f;
  float blinkWindow = angry ? 0.18f : 0.22f;
  float phase = fmodf(t * 0.001f, period);
  float blink01 = 0.0f;
  if (phase > period - blinkWindow) {
    float x = (phase - (period - blinkWindow)) / blinkWindow;
    blink01 = (x < 0.5f) ? (x * 2.0f) : ((1.0f - x) * 2.0f);
  }

  uint16_t neon = angry ? COL_NEON2 : COL_NEON1;
  uint16_t ring = angry ? COL_RING2 : COL_RING1;
  uint16_t glowO = angry ? COL_GLOW2 : COL_GLOW1;
  uint16_t glowM = angry ? rgb565(140, 30, 30) : COL_GLOW1;

  gfx->fillScreen(COL_BG);

  int leftX = cx - eyeDX, rightX = cx + eyeDX;
  drawEyeNeon(leftX,  eyeY, ringR, eyeR, irisR, pupilR, gx, gy, blink01, angry, neon, ring, glowO, glowM);
  drawEyeNeon(rightX, eyeY, ringR, eyeR, irisR, pupilR, gx, gy, blink01, angry, neon, ring, glowO, glowM);

  gfx->setTextSize(1);
  gfx->setTextColor(neon);
  gfx->setCursor(10, 10);
  gfx->print(angry ? "MODE 2: ANGRY" : "MODE 1: FRIENDLY");
}

// ---------- Mode 3: Realistic black & white ----------
void drawEyeRealisticBW(int ex, int ey, int eyeR, int pupilR, int gazeX, int gazeY, float blink01) {
  // Eyeball gradient (fake with concentric circles)
  for (int r = eyeR; r > 0; r -= 3) {
    // map r -> shade
    int k = (eyeR - r) * 255 / max(1, eyeR);
    // brighter center
    uint16_t col = (r > eyeR*0.7) ? G5 : (r > eyeR*0.45 ? G6 : G5);
    // subtle edge darkening
    if (r > eyeR*0.85) col = G4;
    gfx->fillCircle(ex, ey, r, col);
  }

  // iris subtle ring (gray)
  gfx->drawCircle(ex, ey, eyeR - 10, G3);
  gfx->drawCircle(ex, ey, eyeR - 11, G3);

  // pupil clamped
  int px = ex + gazeX;
  int py = ey + gazeY;
  int maxOff = eyeR / 3;
  int dx = px - ex, dy = py - ey;
  long d2 = (long)dx*dx + (long)dy*dy;
  long md = maxOff;
  if (d2 > md*md) {
    float sc = (float)md / sqrtf((float)d2);
    px = ex + (int)(dx * sc);
    py = ey + (int)(dy * sc);
  }

  // pupil + soft shadow halo
  gfx->fillCircle(px, py, pupilR + 6, G2);
  gfx->fillCircle(px, py, pupilR, G0);

  // glossy highlight (two dots)
  gfx->fillCircle(ex - eyeR/3, ey - eyeR/3, max(3, eyeR/12), G6);
  gfx->fillCircle(ex - eyeR/4, ey - eyeR/4, max(2, eyeR/18), G5);

  // blink mask
  int cut = (int)(blink01 * (eyeR + 6));
  if (cut > 0) {
    gfx->fillRect(ex - eyeR - 12, ey - eyeR - 14, 2*eyeR + 24, cut + 14, COL_BG);
    gfx->fillRect(ex - eyeR - 12, ey + eyeR - cut, 2*eyeR + 24, cut + 14, COL_BG);
  }

  // crisp outline
  gfx->drawCircle(ex, ey, eyeR, G3);
  gfx->drawCircle(ex, ey, eyeR + 1, G2);
}

void sceneRealisticBW() {
  if (real_t0 == 0) real_t0 = millis();
  uint32_t t = millis() - real_t0;

  int W = gfx->width(), H = gfx->height();
  int cx = W/2, cy = H/2;

  int eyeR = min(W, H) / 5;
  int pupilR = eyeR / 4;
  int eyeDX = eyeR + eyeR/2;
  int eyeY = cy;

  // slow realistic gaze drift
  int gx = (int)((eyeR/3) * sinf(t * 0.0010f));
  int gy = (int)((eyeR/5) * sinf(t * 0.0008f));

  // blink occasionally
  float period = 3.0f;
  float win = 0.22f;
  float phase = fmodf(t * 0.001f, period);
  float blink01 = 0.0f;
  if (phase > period - win) {
    float x = (phase - (period - win)) / win;
    blink01 = (x < 0.5f) ? (x * 2.0f) : ((1.0f - x) * 2.0f);
  }

  gfx->fillScreen(COL_BG);

  int leftX = cx - eyeDX, rightX = cx + eyeDX;
  drawEyeRealisticBW(leftX,  eyeY, eyeR, pupilR, gx, gy, blink01);
  drawEyeRealisticBW(rightX, eyeY, eyeR, pupilR, gx, gy, blink01);

  gfx->setTextSize(1);
  gfx->setTextColor(G5);
  gfx->setCursor(10, 10);
  gfx->print("MODE 3: REALISTIC B/W");
}

// ---------- Mode 4: Cute robot eyes ----------
void drawCuteEye(int ex, int ey, int w, int h, float blink01, int pupilX) {
  // Simple rounded-rect eye with thick outline + big highlight
  int r = min(w, h) / 3;

  // Outer outline
  gfx->fillRoundRect(ex - w/2 - 4, ey - h/2 - 4, w + 8, h + 8, r + 4, C_LINE);
  // Inner fill
  gfx->fillRoundRect(ex - w/2, ey - h/2, w, h, r, C_EYE);

  // Blink: cover from top/bottom
  int cut = (int)(blink01 * (h/2 + 4));
  if (cut > 0) {
    gfx->fillRect(ex - w/2 - 2, ey - h/2 - 2, w + 4, cut + 2, C_BG);
    gfx->fillRect(ex - w/2 - 2, ey + h/2 - cut, w + 4, cut + 2, C_BG);
  }

  // pupil (simple vertical pill)
  int px = ex + pupilX;
  px = clampi(px, ex - w/4, ex + w/4);
  int pw = w / 6;
  int ph = h / 3;
  gfx->fillRoundRect(px - pw/2, ey - ph/2, pw, ph, pw/2, rgb565(25, 35, 55));

  // cute sparkle
  gfx->fillCircle(ex - w/5, ey - h/5, max(3, w/16), COL_WHITE);
  gfx->fillCircle(ex - w/6, ey - h/6, max(2, w/24), C_ACC);
}

void sceneCuteRobot() {
  if (cute_t0 == 0) cute_t0 = millis();
  uint32_t t = millis() - cute_t0;

  int W = gfx->width(), H = gfx->height();
  int cx = W/2, cy = H/2;

  gfx->fillScreen(C_BG);

  int eyeW = min(W, H) / 3;
  int eyeH = eyeW / 2;
  int eyeDX = eyeW / 2 + 35;
  int eyeY = cy;

  // bouncy pupil + blink
  int px = (int)((eyeW/8) * sinf(t * 0.0022f));
  float blink01 = 0.0f;
  float phase = fmodf(t * 0.001f, 2.6f);
  if (phase > 2.35f) {
    float x = (phase - 2.35f) / 0.25f;
    blink01 = (x < 0.5f) ? (x * 2.0f) : ((1.0f - x) * 2.0f);
  }

  // subtle “happy bounce”
  int bob = (int)(3 * sinf(t * 0.003f));

  drawCuteEye(cx - eyeDX, eyeY + bob, eyeW, eyeH, blink01, px);
  drawCuteEye(cx + eyeDX, eyeY + bob, eyeW, eyeH, blink01, px);

  gfx->setTextSize(1);
  gfx->setTextColor(C_ACC);
  gfx->setCursor(10, 10);
  gfx->print("MODE 4: CUTE ROBOT");
}

// ---------- Input handling ----------
void pollSerialCommands() {
  while (USBSerial.available()) {
    char c = (char)USBSerial.read();
    if (c >= '0' && c <= '4') {
      setMode(c - '0');
      if (mode == 0) sceneHome();
    }
  }
}

#if USE_BOOT_BUTTON
void pollBootButton() {
  bool b = digitalRead(BOOT_PIN); // HIGH idle, LOW pressed
  if (b != lastBoot) lastDebounce = millis();
  if (millis() - lastDebounce > 40) {
    if (lastBoot == true && b == false) { // press
      setMode(mode + 1);
      if (mode == 0) sceneHome();
    }
  }
  lastBoot = b;
}
#endif

void setup() {
  USBSerial.begin(115200);
  USBSerial.println("Robot Eyes: 5 modes");

#ifdef GFX_EXTRA_PRE_INIT
  GFX_EXTRA_PRE_INIT();
#endif

  if (!gfx->begin()) {
    USBSerial.println("gfx->begin() failed!");
  }

#if USE_BOOT_BUTTON
  pinMode(BOOT_PIN, INPUT_PULLUP);
#endif

  setMode(0);
  sceneHome();
}

void loop() {
  pollSerialCommands();
#if USE_BOOT_BUTTON
  pollBootButton();
#endif

  switch (mode) {
    case 0:
      delay(30);
      break;
    case 1:
      sceneNeonEyes(false);
      delay(33);
      break;
    case 2:
      sceneNeonEyes(true);
      delay(33);
      break;
    case 3:
      sceneRealisticBW();
      delay(33);
      break;
    case 4:
      sceneCuteRobot();
      delay(33);
      break;
  }
}
