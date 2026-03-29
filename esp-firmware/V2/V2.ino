#include <NimBLEDevice.h>
#include <Wire.h>
#include <Adafruit_LTR390.h>
#include <Adafruit_AS7341.h>
#include <SparkFun_TMP117.h>

// ================= BLE UUIDs =================
#define SERVICE_UUID      "12345678-1234-1234-1234-1234567890ab"
#define DATA_CHAR_UUID    "abcdefab-1234-1234-1234-abcdefabcdef" // Notify + Read
#define CTRL_CHAR_UUID    "abcdefac-1234-1234-1234-abcdefabcdef" // Write

// ================= SYSTEM STATE =================
enum SystemState {
  STATE_IDLE,
  STATE_CONNECTED,
  STATE_STREAMING
};

volatile SystemState currentState = STATE_IDLE;

NimBLECharacteristic* dataChar;
NimBLECharacteristic* ctrlChar;

// ================= TIMING & TRACKING =================
uint32_t sampleInterval = 1000; 
unsigned long lastSampleTime = 0;
uint16_t seq_id = 0;

// ================= SENSOR OBJECTS =================
Adafruit_LTR390 ltr = Adafruit_LTR390();
Adafruit_AS7341 as7341;
TMP117 tmp117;

// ================= CALIBRATION BASELINES =================
uint16_t baseline_R680 = 0;
uint16_t baseline_R910 = 0;
float temp_offset = 0.0;

// ================= SENSOR ABSTRACTION =================

float read_IUV() {
  // LTR390 UVS reading. (Divide by sensitivity factor for actual UVI based on your window/gain)
  // Assuming default gain (3) and resolution (18-bit), sensitivity is ~2300 per 1 UVI
  uint32_t raw_uvs = ltr.readUVS();
  return (float)raw_uvs / 2300.0; 
}

uint16_t read_R660() {
  // AS7341 doesn't have an exact 660nm channel. F7 (680nm) is the closest standard bucket.
  return as7341.getChannel(AS7341_CHANNEL_680nm_F8); 
}

uint16_t read_R940() {
  // Closest on standard AS7341 is the NIR channel (~910nm)
  return as7341.getChannel(AS7341_CHANNEL_NIR); 
}

float read_T_skin() {
  return tmp117.readTempC(); // Returns °C
}

void calibrate_sensors() {
  Serial.println("ACTION: Calibrating sensors for dual-sensor mapping...");

  // 1. Take a fresh spectral reading for the baseline
  as7341.readAllChannels(); 
  baseline_R680 = as7341.getChannel(AS7341_CHANNEL_680nm_F8);
  baseline_R910 = as7341.getChannel(AS7341_CHANNEL_NIR);

  // 2. Read current skin temp to use as a starting offset if needed
  float current_temp = tmp117.readTempC();
  
  // (Optional) LTR390 Auto-Gain logic could go here if UV is saturated

  Serial.printf("Baselines Locked! R680: %d | R910: %d | Temp: %.2f C\n", 
                baseline_R680, baseline_R910, current_temp);
}

// ================= SERVER CALLBACKS =================
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    currentState = STATE_CONNECTED;
    Serial.println("BLE: Client Connected");
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    currentState = STATE_IDLE;
    Serial.println("BLE: Client Disconnected");
    NimBLEDevice::startAdvertising(); 
  }
};

// ================= CONTROL WRITE CALLBACK =================
class CtrlWriteCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& connInfo) override {
    std::string val = c->getValue();
    if (val.length() < 1) return;

    uint8_t opcode = val[0];

    switch (opcode) {
      case 0x01: // START_STREAM
        if (currentState == STATE_CONNECTED) {
          currentState = STATE_STREAMING;
          Serial.println("CMD: START_STREAM");
        }
        break;
      case 0x02: // STOP_STREAM
        if (currentState == STATE_STREAMING) {
          currentState = STATE_CONNECTED;
          Serial.println("CMD: STOP_STREAM");
        }
        break;
      case 0x03: // SET_INTERVAL
        if (val.length() >= 3) {
          sampleInterval = val[1] | (val[2] << 8);
          Serial.printf("CMD: SET_INTERVAL -> %d ms\n", sampleInterval);
        }
        break;
      case 0x04: // CALIBRATE
        calibrate_sensors();
        break;
      case 0x05: // PING
        Serial.println("CMD: PING");
        break;
    }
  }
};

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("System Booting...");

  Wire.begin(); // Initialize I2C bus

  // --- Init LTR390 (UV) ---
  if (!ltr.begin()) {
    Serial.println("ERROR: LTR390 not found!");
  } else {
    ltr.setMode(LTR390_MODE_UVS);
    ltr.setGain(LTR390_GAIN_3);
    ltr.setResolution(LTR390_RESOLUTION_18BIT);
  }

  // --- Init AS7341 (Reflectance) ---
  if (!as7341.begin()) {
    Serial.println("ERROR: AS7341 not found!");
  } else {
    as7341.setATIME(100);
    as7341.setASTEP(999);
    as7341.setGain(AS7341_GAIN_256X);
    as7341.setLEDCurrent(50); // Set to 50mA to prevent brownouts
    as7341.enableLED(true);   // Turn on the reflectance LED
  }

  // --- Init TMP117 (Skin Temp) ---
  if (!tmp117.begin()) {
    Serial.println("ERROR: TMP117 not found!");
  }

  // --- Init BLE ---
  NimBLEDevice::init("MELANEX_HUB");
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCB());

  NimBLEService* service = server->createService(SERVICE_UUID);

  dataChar = service->createCharacteristic(
    DATA_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );

  ctrlChar = service->createCharacteristic(
    CTRL_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE
  );
  ctrlChar->setCallbacks(new CtrlWriteCB());

  service->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName("MELANEX_HUB");
  adv->addServiceUUID(service->getUUID());
  adv->enableScanResponse(true);
  adv->start();

  Serial.println("Sensors Init Complete. BLE Advertising.");
}

// ================= MAIN LOOP =================
void loop() {
  if (currentState == STATE_STREAMING) {
    unsigned long currentMillis = millis();

    if (currentMillis - lastSampleTime >= sampleInterval) {
      lastSampleTime = currentMillis;

      // 1. Trigger spectral reading (requires slight delay, handled by library)
      as7341.readAllChannels(); 

      // 2. Read and Scale Data
      uint32_t timestamp = currentMillis;
      uint16_t iuv_scaled = (uint16_t)(read_IUV() * 100.0);
      uint16_t r660_val = read_R660();
      uint16_t r940_val = read_R940();
      int16_t tskin_scaled = (int16_t)(read_T_skin() * 100.0);

      // 3. Pack Array (20 Bytes, Little Endian)
      uint8_t packet[20] = {0};
      
      packet[0] = 0x01; // Type

      packet[1] = timestamp & 0xFF;
      packet[2] = (timestamp >> 8) & 0xFF;
      packet[3] = (timestamp >> 16) & 0xFF;
      packet[4] = (timestamp >> 24) & 0xFF;

      packet[5] = iuv_scaled & 0xFF;
      packet[6] = (iuv_scaled >> 8) & 0xFF;

      packet[7] = r660_val & 0xFF;
      packet[8] = (r660_val >> 8) & 0xFF;

      packet[9] = r940_val & 0xFF;
      packet[10] = (r940_val >> 8) & 0xFF;

      packet[11] = (uint16_t)tskin_scaled & 0xFF;
      packet[12] = ((uint16_t)tskin_scaled >> 8) & 0xFF;

      packet[13] = seq_id & 0xFF;
      packet[14] = (seq_id >> 8) & 0xFF;

      // 4. Calculate Checksum
      uint8_t checksum = 0;
      for (int i = 0; i < 19; i++) {
        checksum ^= packet[i];
      }
      packet[19] = checksum;

      // 5. Notify
      dataChar->setValue(packet, 20);
      dataChar->notify();

      Serial.printf("NOTIFY Seq: %d | UV: %.2f | R680: %d | R910: %d | T: %.2f C\n", 
                    seq_id, read_IUV(), r660_val, r940_val, read_T_skin());

      seq_id++;
    }
  }
}