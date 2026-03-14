/*
 Coordinate probe — draw large rectangles at absolute coordinates
 - Put roboteye_rgb565.h in the same folder (not needed by this sketch, but keep it there).
 - Upload, open Serial Monitor at 115200.
 - The sketch prints reported W/H/rotation and draws 5 boxes:
     RED  @ (0,0)
     GRN  @ (240,0)
     BLU  @ (0,400)
     YEL  @ (240,400)
     MAG  @ (0,150)   <- your reported image top-left earlier
 - Take a photo and paste the Serial output back here.
*/

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <pgmspace.h>

// Display pins - same as your hardware
#define LCD_CS   12
#define LCD_CLK  5
#define LCD_D0   1
#define LCD_D1   2
#define LCD_D2   3
#define LCD_D3   4
#define LCD_BL   6

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_CLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3
);

Arduino_GFX *gfx = new Arduino_AXS15231B(
  bus, -1, 0, false, 320, 480, 0, 0, 0, 0
);



void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Coordinate probe ===");
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  if (!gfx->begin()) {
    Serial.println("gfx->begin() FAILED!");
    while (true) delay(100);
  }

  int W = gfx->width();
  int H = gfx->height();
  int rot = gfx->getRotation();
  Serial.printf("Library reports: W=%d H=%d rotation=%d\n", W, H, rot);

  // white background for best visibility
  gfx->fillScreen(rgb565(255,255,255));
  delay(50);

  // draw 4 large 80x80 squares at corners for a 320x480 portrait layout
  const int S = 80;
  // top-left
  gfx->fillRect(0, 0, S, S, rgb565(255,0,0));       // RED @ (0,0)
  // top-right (320-80 = 240)
  gfx->fillRect(240, 0, S, S, rgb565(0,255,0));     // GREEN @ (240,0)
  // bottom-left (480-80 = 400)
  gfx->fillRect(0, 400, S, S, rgb565(0,0,255));     // BLUE @ (0,400)
  // bottom-right
  gfx->fillRect(240, 400, S, S, rgb565(255,255,0)); // YELLOW @ (240,400)

  // magenta box at the image top-left you reported earlier
  gfx->fillRect(0, 150, S, S, rgb565(255,0,255));   // MAGENTA @ (0,150)

  // draw small text to help orientation
  gfx->setTextSize(2);
  gfx->setTextColor(rgb565(0,0,0));
  gfx->setCursor(6, 6);
  gfx->println("Probe: red TL, grn TR");
  gfx->setCursor(6, 30);
  gfx->println("blu BL, yel BR, mag @ (0,150)");

  Serial.println("Drawn probe boxes. Take a photo and report which boxes you see and where.");
}

void loop() {
  // nothing else to do
  delay(1000);
}
