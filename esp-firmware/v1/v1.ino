/*
 * MELANEX - Final Phase 4: Real-Time Wearable & Haptic Motor
 * Hardware: ESP32-S3, LTR390, AS7341, TMP117, SSD1306, Coin Motor Circuit (Pin 5)
 */

#include <Arduino.h>
#include <Wire.h>
#include <math.h>

#include <Adafruit_LTR390.h>
#include <Adafruit_AS7341.h>
#include <SparkFun_TMP117.h> 
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── Pin Definitions ──
#define SDA_PIN 8
#define SCL_PIN 9
#define PIN_HAPTIC 5 

// ── OLED Setup ──
#define SCREEN_W 128
#define SCREEN_H 64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);

// ── Sensor Objects ──
Adafruit_LTR390 ltr390;
Adafruit_AS7341 as7341;
TMP117 tmp117;

// ── Custom "Emojis" (16x16 Bitmaps) ──
const unsigned char bmp_sun [] PROGMEM = {
  0x01, 0x80, 0x01, 0x80, 0x08, 0x10, 0x00, 0x00, 0x33, 0xcc, 0x47, 0xe2, 0x4f, 0xf2, 0x4f, 0xf2, 
  0x4f, 0xf2, 0x47, 0xe2, 0x33, 0xcc, 0x00, 0x00, 0x08, 0x10, 0x01, 0x80, 0x01, 0x80, 0x00, 0x00
};
const unsigned char bmp_alert [] PROGMEM = {
  0x01, 0x80, 0x03, 0xc0, 0x03, 0xc0, 0x06, 0x60, 0x04, 0x20, 0x0c, 0x30, 0x0c, 0x30, 0x19, 0x98, 
  0x19, 0x98, 0x31, 0x8c, 0x31, 0x8c, 0x60, 0x06, 0x61, 0x86, 0x7f, 0xfe, 0x7f, 0xfe, 0x00, 0x00
};

// ── Constants & Tracking ──
const float K1 = 0.70f;  
const float BURN_B0 = 200.0f; 
const float BURN_B1 = 50.0f;  

float cumulativeDose = 0.0f;
uint32_t lastSampleMs = 0;

// ── 3-Tier Alert Flags ──
bool alert50Triggered = false;
bool alert80Triggered = false;
bool alert100Triggered = false; 

// Math Helpers
float erythemaWeight(float lambda) {
  if (lambda < 280.f || lambda > 400.f) return 0.0f;
  if (lambda <= 298.f) return 1.0f;
  if (lambda <= 328.f) return powf(10.f, 0.094f * (298.f - lambda));
  return powf(10.f, 0.015f * (139.f - lambda)); 
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin(SDA_PIN, SCL_PIN);

  // Setup Outputs
  pinMode(PIN_HAPTIC, OUTPUT);
  digitalWrite(PIN_HAPTIC, LOW);

  oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE);
  
  ltr390.begin(); ltr390.setMode(LTR390_MODE_UVS); ltr390.setGain(LTR390_GAIN_3); ltr390.setResolution(LTR390_RESOLUTION_16BIT);
  as7341.begin(); as7341.setATIME(100); as7341.setASTEP(999); as7341.setGain(AS7341_GAIN_256X); as7341.setLEDCurrent(4); as7341.enableLED(true);
  tmp117.begin(0x48, Wire);

  lastSampleMs = millis();
}

void loop() {

  uint32_t now = millis();
  if (now - lastSampleMs < 1000) return; 
  lastSampleMs = now;

  // 1. Gather Sensor Data
  float iUV = 0.0f, iBio = 0.0f, mi = 0.3f, alphaSkin = 0.2f;
  
  if (ltr390.newDataAvailable()) {
    iUV = ltr390.readUVS() * 0.0036f;
    iBio = iUV * (0.20f * erythemaWeight(300.f) + 0.80f * erythemaWeight(365.f));
  }
  
  if (as7341.readAllChannels()) {
    uint16_t c630 = as7341.getChannel(AS7341_CHANNEL_630nm_F7);
    uint16_t cNIR = as7341.getChannel(AS7341_CHANNEL_NIR);
    uint16_t clr = as7341.getChannel(AS7341_CHANNEL_CLEAR);
    
    float r630 = clr == 0 ? 0.5f : constrain((float)c630 / clr, 0.001f, 1.0f);
    float rNIR = clr == 0 ? 0.5f : constrain((float)cNIR / clr, 0.001f, 1.0f);
    
    mi = fabs(log10f(1.f / r630) - log10f(1.f / rNIR));
    
    // Invert the melanin factor. High Melanin = Low Vulnerability
    alphaSkin = 1.0f - constrain(K1 * mi, 0.0f, 1.0f);
  }

  // 2. Calculate Real-Time Damage (1 second of exposure = 1 second of dose)
  cumulativeDose += (iBio * alphaSkin * 500.0f);
  
  float tBurn = BURN_B0 + (BURN_B1 * mi); 
  float burnPercentage = (cumulativeDose / tBurn) * 100.0f;
  if (burnPercentage > 100.0f) burnPercentage = 100.0f;

// 3. ── Three-Tier AUDIO Warning System ──
  // WARNING 1: 50% (Single Loud Beep)
  if (burnPercentage >= 50.0f && burnPercentage < 80.0f && !alert50Triggered) {
    tone(PIN_HAPTIC, 2500); delay(200); noTone(PIN_HAPTIC);
    alert50Triggered = true; 
  }
  // WARNING 2: 80% (Double Loud Beep)
  else if (burnPercentage >= 80.0f && burnPercentage < 100.0f && !alert80Triggered) {
    for(int i=0; i<2; i++) {
      tone(PIN_HAPTIC, 2500); delay(150); noTone(PIN_HAPTIC); delay(100);
    }
    alert80Triggered = true; 
  }
  // WARNING 3: 100% (Continuous Beeping!)
  else if (burnPercentage >= 100.0f) {
    // Lock removed! This will double-beep EVERY second until you press Reset.
    tone(PIN_HAPTIC, 2500); delay(150); noTone(PIN_HAPTIC); delay(100);
    tone(PIN_HAPTIC, 2500); delay(150); noTone(PIN_HAPTIC); 
  }
  
  // 4. Draw Interactive OLED UI
  oled.clearDisplay();
  
  if (burnPercentage < 100.0f) {
    // --- NORMAL & HIGH STATE ---
    oled.drawBitmap(0, 0, bmp_sun, 16, 16, SSD1306_WHITE);
    
    oled.setTextSize(1);
    oled.setCursor(22, 4);
    oled.printf("Exp: %.1f J/m2", cumulativeDose);

    oled.setTextSize(2);
    oled.setCursor(0, 25);
    // Change text if it reaches the 80% high-risk threshold
    if (burnPercentage >= 80.0f) {
      oled.printf("HIGH:%.0f%%", burnPercentage);
    } else {
      oled.printf("Limit:%.0f%%", burnPercentage);
    }
    
    // --- CLEAN PROGRESS BAR ---
    oled.drawRect(4, 50, 120, 10, SSD1306_WHITE); 
    int barWidth = (int)((burnPercentage / 100.0f) * 116.0f);
    oled.fillRect(6, 52, barWidth, 6, SSD1306_WHITE); 

  } else {
    // --- BURN ALERT STATE ---
    if ((now / 1000) % 2 == 0) {
      oled.drawBitmap(16, 0, bmp_alert, 16, 16, SSD1306_WHITE);
      oled.drawBitmap(96, 0, bmp_alert, 16, 16, SSD1306_WHITE);
    }
    
    oled.setTextSize(2);
    oled.setCursor(4, 25);
    oled.print("BURN ALERT");

    // --- BLINKING FULL PROGRESS BAR ---
    oled.drawRect(4, 50, 120, 10, SSD1306_WHITE); 
    if ((now / 250) % 2 == 0) {
      oled.fillRect(6, 52, 116, 6, SSD1306_WHITE); 
    }
  }

  oled.display();
}