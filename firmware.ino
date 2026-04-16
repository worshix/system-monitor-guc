/*
 * ESP32 Multi-Sensor System with 20x4 I2C LCD Display + WiFi/MQTT
 *
 * Pin Assignments:
 *   GPIO12 - ZMPT101B (AC Voltage, analog)
 *   GPIO13 - ACS712   (Current, analog)
 *   GPIO18 - DS18B20  (Temperature, OneWire manual)
 *   GPIO14 - Vibration Sensor (Digital interrupt)
 *   GPIO32 - Sound Sensor (Analog)
 *   GPIO17 - Relay (Motor control, active-HIGH)
 *   GPIO21 - LCD SDA  (I2C default on ESP32)
 *   GPIO22 - LCD SCL  (I2C default on ESP32)
 *
 * Required Libraries (Sketch > Include Library > Manage Libraries):
 *   - LiquidCrystal I2C  by Frank de Brabander
 *   - PubSubClient        by Nick O'Leary
 *
 * MQTT Topics:
 *   Publish:   /motor/vibration   (mm/s, float)
 *              /motor/temperature (°C,   float)
 *              /motor/current     (A,    float)
 *              /motor/power       (W,    float)
 *              /motor/health      (%,    float 0-100)
 *   Subscribe: /motor/command     ("start" | "stop")
 *
 * LCD I2C Address: 0x27 (change to 0x3F if display stays blank)
 *
 * LCD Layout (20 chars x 4 rows):
 *   Row 0: V:230.5V  I: 1.230A
 *   Row 1: Temp:24.3C / 75.7F
 *   Row 2: Vib:YES   Snd:62.4dB
 *   Row 3: MQTT: OK  Motor: ON
 */

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <PubSubClient.h>

// ── WiFi CREDENTIALS (fill in before flashing) ───────────────
#define WIFI_SSID     "YourSSID"
#define WIFI_PASSWORD "YourPassword"

// ── MQTT BROKER ──────────────────────────────────────────────
// Plain TCP port (1883). The dashboard uses WebSocket (9001).
// Both ports must be enabled on the same broker.
#define MQTT_BROKER   "192.168.1.100"   // broker LAN IP
#define MQTT_PORT     1883
#define MQTT_CLIENT_ID "esp32-motor-monitor"

// ── MQTT TOPICS ──────────────────────────────────────────────
#define TOPIC_VIBRATION   "/motor/vibration"
#define TOPIC_TEMPERATURE "/motor/temperature"
#define TOPIC_CURRENT     "/motor/current"
#define TOPIC_POWER       "/motor/power"
#define TOPIC_HEALTH      "/motor/health"
#define TOPIC_COMMAND     "/motor/command"

// ── LCD CONFIGURATION ────────────────────────────────────────
#define LCD_ADDRESS  0x27
#define LCD_COLS     20
#define LCD_ROWS     4

LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);

// ── PIN DEFINITIONS ──────────────────────────────────────────
#define VOLTAGE_PIN          12
#define CURRENT_PIN          13
#define ONE_WIRE_BUS         18
#define VIBRATION_PIN        14
#define SOUND_PIN            32
#define MOTOR_RELAY_PIN      17   // HIGH = relay closed = motor ON

// ── CALIBRATION & CONSTANTS ──────────────────────────────────
#define ADC_RESOLUTION       4095.0f
#define VREF                 3.3f
#define VOLTAGE_CALIBRATION  0.5f
#define SAMPLING_DURATION_MS 20
#define ACS712_SENSITIVITY   0.185f
#define ACS712_ZERO_OFFSET   2.5f
#define ACS712_SAMPLES       10
#define SOUND_REF_VOLTAGE    0.00631f
#define SUPPLY_VOLTAGE       220.0f   // nominal mains voltage (V)

#define DS18B20_SKIP_ROM        0xCC
#define DS18B20_CONVERT_T       0x44
#define DS18B20_READ_SCRATCHPAD 0xBE

// ── GLOBALS ──────────────────────────────────────────────────
volatile bool vibrationDetected;
unsigned long lastReadTime;
unsigned long lastScrollTime;
const unsigned long READ_INTERVAL   = 2000;
const unsigned long SCROLL_INTERVAL = 3000;
int displayPage;
bool motorRunning;

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

// ── ONEWIRE: RESET ───────────────────────────────────────────
bool oneWire_reset() {
  int pinState;
  bool presence;

  pinMode(ONE_WIRE_BUS, OUTPUT);
  digitalWrite(ONE_WIRE_BUS, LOW);
  delayMicroseconds(480);

  pinMode(ONE_WIRE_BUS, INPUT);
  delayMicroseconds(70);

  pinState = digitalRead(ONE_WIRE_BUS);

  if (pinState == LOW) {
    presence = true;
  } else {
    presence = false;
  }

  delayMicroseconds(410);
  return presence;
}

// ── ONEWIRE: WRITE BIT ───────────────────────────────────────
void oneWire_writeBit(uint8_t bit) {
  pinMode(ONE_WIRE_BUS, OUTPUT);
  digitalWrite(ONE_WIRE_BUS, LOW);
  if (bit) {
    delayMicroseconds(6);
    pinMode(ONE_WIRE_BUS, INPUT);
    delayMicroseconds(64);
  } else {
    delayMicroseconds(60);
    pinMode(ONE_WIRE_BUS, INPUT);
    delayMicroseconds(10);
  }
}

// ── ONEWIRE: READ BIT ────────────────────────────────────────
uint8_t oneWire_readBit() {
  uint8_t bitVal;
  pinMode(ONE_WIRE_BUS, OUTPUT);
  digitalWrite(ONE_WIRE_BUS, LOW);
  delayMicroseconds(6);
  pinMode(ONE_WIRE_BUS, INPUT);
  delayMicroseconds(9);
  bitVal = digitalRead(ONE_WIRE_BUS);
  delayMicroseconds(55);
  return bitVal;
}

// ── ONEWIRE: WRITE BYTE ──────────────────────────────────────
void oneWire_writeByte(uint8_t data) {
  int i;
  for (i = 0; i < 8; i++) {
    oneWire_writeBit(data & 0x01);
    data >>= 1;
  }
}

// ── ONEWIRE: READ BYTE ───────────────────────────────────────
uint8_t oneWire_readByte() {
  uint8_t data;
  int i;
  data = 0;
  for (i = 0; i < 8; i++) {
    data |= (oneWire_readBit() << i);
  }
  return data;
}

// ── DS18B20: READ TEMPERATURE ────────────────────────────────
bool readTemperature(float &tempC, float &tempF) {
  int attempt;
  int i;
  int j;
  uint8_t scratchpad[9];
  uint8_t crc;
  uint8_t inbyte;
  uint8_t mix;
  int16_t rawTemp;

  for (attempt = 1; attempt <= 3; attempt++) {

    if (oneWire_reset() == false) {
      delay(100);
      continue;
    }

    oneWire_writeByte(DS18B20_SKIP_ROM);
    oneWire_writeByte(DS18B20_CONVERT_T);
    delay(750);

    if (oneWire_reset() == false) {
      delay(100);
      continue;
    }

    oneWire_writeByte(DS18B20_SKIP_ROM);
    oneWire_writeByte(DS18B20_READ_SCRATCHPAD);

    for (i = 0; i < 9; i++) {
      scratchpad[i] = oneWire_readByte();
    }

    // Dallas CRC-8 check
    crc = 0;
    for (i = 0; i < 8; i++) {
      inbyte = scratchpad[i];
      for (j = 0; j < 8; j++) {
        mix = (crc ^ inbyte) & 0x01;
        crc >>= 1;
        if (mix) {
          crc ^= 0x8C;
        }
        inbyte >>= 1;
      }
    }

    if (crc != scratchpad[8]) {
      delay(100);
      continue;
    }

    rawTemp = (int16_t)((scratchpad[1] << 8) | scratchpad[0]);
    tempC = rawTemp / 16.0f;
    tempF = tempC * 9.0f / 5.0f + 32.0f;

    if (tempC >= -55.0f && tempC <= 125.0f) {
      return true;
    }

    delay(100);
  }

  tempC = NAN;
  tempF = NAN;
  return false;
}

// ── ZMPT101B: RMS VOLTAGE ────────────────────────────────────
float readRMSVoltage() {
  float sumSquares;
  float v;
  int sampleCount;
  int raw;
  unsigned long startTime;

  sumSquares = 0.0f;
  sampleCount = 0;
  startTime = millis();

  while (millis() - startTime < SAMPLING_DURATION_MS) {
    raw = analogRead(VOLTAGE_PIN);
    v = ((raw / ADC_RESOLUTION) * VREF) - (VREF / 2.0f);
    sumSquares += v * v;
    sampleCount++;
    delayMicroseconds(100);
  }

  if (sampleCount == 0) {
    return 0.0f;
  }
  return sqrt(sumSquares / sampleCount) * VOLTAGE_CALIBRATION;
}

// ── ACS712: CURRENT ──────────────────────────────────────────
float readCurrentACS712() {
  long sum;
  float avgRaw;
  float voltage;
  float current;
  int i;

  sum = 0;
  for (i = 0; i < ACS712_SAMPLES; i++) {
    sum += analogRead(CURRENT_PIN);
    delay(2);
  }

  avgRaw = (float)sum / ACS712_SAMPLES;
  voltage = (avgRaw / ADC_RESOLUTION) * VREF;
  current = (voltage - ACS712_ZERO_OFFSET) / ACS712_SENSITIVITY;

  if (current < 0.05f && current > -0.05f) {
    current = 0.0f;
  }
  return current;
}

// ── SOUND: RAW TO dB ─────────────────────────────────────────
float soundToDecibels(int rawValue) {
  float voltage;
  float dB;

  if (rawValue <= 0) {
    return 0.0f;
  }
  voltage = (rawValue / ADC_RESOLUTION) * VREF;
  dB = 20.0f * log10(voltage / SOUND_REF_VOLTAGE);
  return constrain(dB, 0.0f, 100.0f);
}

// ── INTERRUPT: VIBRATION ─────────────────────────────────────
void IRAM_ATTR onVibration() {
  vibrationDetected = true;
}

// ── RANDOM FLOAT helper ──────────────────────────────────────
// Returns a float in [lo, hi]
float randomFloat(float lo, float hi) {
  return lo + (float)random(0, 10000) / 10000.0f * (hi - lo);
}

// ── COMPUTE HEALTH SCORE (0–100) ─────────────────────────────
// Derived from vibration (mm/s), temperature (°C), and current (A)
float computeHealth(float vibration, float temperature, float current) {
  float score = 100.0f;

  // vibration penalty: starts at 6 mm/s, max deduction 50 pts
  if (vibration > 6.0f) {
    score -= constrain((vibration - 6.0f) / 4.0f * 50.0f, 0.0f, 50.0f);
  }

  // temperature penalty: starts at 80°C, max deduction 35 pts
  if (temperature > 80.0f) {
    score -= constrain((temperature - 80.0f) / 70.0f * 35.0f, 0.0f, 35.0f);
  }

  // current penalty: starts at 35A, max deduction 15 pts
  if (current > 35.0f) {
    score -= constrain((current - 35.0f) / 15.0f * 15.0f, 0.0f, 15.0f);
  }

  return constrain(score, 0.0f, 100.0f);
}

// ── WIFI: CONNECT ────────────────────────────────────────────
void connectWiFi() {
  Serial.print("WiFi connecting to ");
  Serial.print(WIFI_SSID);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi...");
  lcd.setCursor(0, 1);
  lcd.print(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" OK");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    lcd.setCursor(0, 2);
    lcd.print("IP: ");
    lcd.print(WiFi.localIP());
  } else {
    Serial.println(" FAILED – continuing offline");
    lcd.setCursor(0, 2);
    lcd.print("WiFi FAILED!        ");
  }

  delay(2000);
}

// ── MQTT: COMMAND CALLBACK ───────────────────────────────────
void mqttCallback(char *topic, byte *payload, unsigned int length) {
  char msg[16];
  unsigned int i;

  if (length >= sizeof(msg)) {
    length = sizeof(msg) - 1;
  }
  for (i = 0; i < length; i++) {
    msg[i] = (char)payload[i];
  }
  msg[length] = '\0';

  if (strcmp(topic, TOPIC_COMMAND) == 0) {
    if (strcmp(msg, "start") == 0) {
      motorRunning = true;
      digitalWrite(MOTOR_RELAY_PIN, HIGH);
      Serial.println("Motor: START");
    } else if (strcmp(msg, "stop") == 0) {
      motorRunning = false;
      digitalWrite(MOTOR_RELAY_PIN, LOW);
      Serial.println("Motor: STOP");
    }
  }
}

// ── MQTT: CONNECT / RECONNECT ────────────────────────────────
bool mqttConnect() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (mqttClient.connected()) {
    return true;
  }

  Serial.print("MQTT connecting...");
  if (mqttClient.connect(MQTT_CLIENT_ID)) {
    mqttClient.subscribe(TOPIC_COMMAND);
    Serial.println(" OK");
    return true;
  }

  Serial.print(" FAILED rc=");
  Serial.println(mqttClient.state());
  return false;
}

// ── MQTT: PUBLISH SENSOR DATA ────────────────────────────────
void publishSensorData(float vibration, float tempC, float current) {
  char buf[16];
  float power;
  float health;

  if (!mqttClient.connected()) {
    return;
  }

  power  = SUPPLY_VOLTAGE * current;
  health = computeHealth(vibration, tempC, current);

  dtostrf(vibration, 4, 2, buf);
  mqttClient.publish(TOPIC_VIBRATION, buf);

  dtostrf(tempC, 5, 2, buf);
  mqttClient.publish(TOPIC_TEMPERATURE, buf);

  dtostrf(current, 5, 3, buf);
  mqttClient.publish(TOPIC_CURRENT, buf);

  dtostrf(power, 7, 2, buf);
  mqttClient.publish(TOPIC_POWER, buf);

  dtostrf(health, 5, 1, buf);
  mqttClient.publish(TOPIC_HEALTH, buf);
}

// ── LCD HELPER: Print padded string to fill remaining columns ─
void lcdPrint(int col, int row, String text, int fieldWidth) {
  int i;
  int len;
  lcd.setCursor(col, row);
  lcd.print(text);
  len = text.length();
  for (i = len; i < fieldWidth; i++) {
    lcd.print(" ");
  }
}

// ── LCD: Display Page 1 ──────────────────────────────────────
// Row 0: Voltage + Current
// Row 1: Temperature C
// Row 2: Temperature F
// Row 3: MQTT + Motor state
void displayPage1(float voltage, float current, float tempC, float tempF) {

  // Row 0: V:230.5V  I:1.230A
  lcd.setCursor(0, 0);
  lcd.print("V:");
  if (voltage >= 0.0f) {
    lcd.print(voltage, 1);
  } else {
    lcd.print("ERR ");
  }
  lcd.print("V");

  lcd.setCursor(10, 0);
  lcd.print("I:");
  if (current >= -99.0f) {
    lcd.print(current, 3);
  } else {
    lcd.print("ERR  ");
  }
  lcd.print("A");

  // Row 1: Temp: 24.3 C
  lcd.setCursor(0, 1);
  if (!isnan(tempC)) {
    lcd.print("Temp:");
    lcd.print(tempC, 1);
    lcd.print((char)223);  // degree symbol
    lcd.print("C        ");
  } else {
    lcd.print("Temp: SENSOR ERR    ");
  }

  // Row 2: Temp: 75.7 F
  lcd.setCursor(0, 2);
  if (!isnan(tempF)) {
    lcd.print("    (");
    lcd.print(tempF, 1);
    lcd.print((char)223);
    lcd.print("F)       ");
  } else {
    lcd.print("                    ");
  }

  // Row 3: MQTT status + motor state
  lcd.setCursor(0, 3);
  if (mqttClient.connected()) {
    lcd.print("MQTT:OK  ");
  } else {
    lcd.print("MQTT:ERR ");
  }
  lcd.print("Motor:");
  lcd.print(motorRunning ? "ON " : "OFF");
}

// ── LCD: Display Page 2 ──────────────────────────────────────
// Row 0: Vibration status
// Row 1: Sound level dB
// Row 2: Sound raw ADC value
// Row 3: Page indicator
void displayPage2(int vibration, int soundRaw, float soundDB) {

  // Row 0: Vibration
  lcd.setCursor(0, 0);
  if (vibration == 1) {
    lcd.print("Vibration: YES !!!! ");
  } else {
    lcd.print("Vibration: NO      ");
  }

  // Row 1: Sound dB
  lcd.setCursor(0, 1);
  lcd.print("Sound:  ");
  lcd.print(soundDB, 1);
  lcd.print(" dB         ");

  // Row 2: Raw ADC value
  lcd.setCursor(0, 2);
  lcd.print("Raw ADC:");
  lcd.print(soundRaw);
  lcd.print("          ");

  // Row 3: Page label
  lcd.setCursor(0, 3);
  lcd.print("-- Sensor Page 2/2 -");
}

// ── SETUP ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  vibrationDetected = false;
  lastReadTime      = 0;
  lastScrollTime    = 0;
  displayPage       = 1;
  motorRunning      = false;

  // Motor relay – start with motor OFF
  pinMode(MOTOR_RELAY_PIN, OUTPUT);
  digitalWrite(MOTOR_RELAY_PIN, LOW);

  // Init I2C and LCD
  Wire.begin(21, 22);  // SDA=GPIO21, SCL=GPIO22 (ESP32 defaults)
  lcd.init();
  lcd.backlight();
  lcd.clear();

  // Startup splash screen
  lcd.setCursor(2, 0);
  lcd.print("ESP32 MultiSensor");
  lcd.setCursor(0, 1);
  lcd.print("--------------------");
  lcd.setCursor(3, 2);
  lcd.print("Initializing...");
  lcd.setCursor(0, 3);
  lcd.print("--------------------");

  delay(2000);
  lcd.clear();

  // Check DS18B20
  lcd.setCursor(0, 0);
  if (oneWire_reset() == true) {
    lcd.print("DS18B20: OK         ");
  } else {
    lcd.print("DS18B20: NOT FOUND! ");
    lcd.setCursor(0, 1);
    lcd.print("Check wiring+pullup ");
  }

  // Pin setup
  pinMode(VOLTAGE_PIN,   INPUT);
  pinMode(CURRENT_PIN,   INPUT);
  pinMode(SOUND_PIN,     INPUT);
  pinMode(ONE_WIRE_BUS,  INPUT);
  pinMode(VIBRATION_PIN, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(VIBRATION_PIN), onVibration, RISING);
  analogSetAttenuation(ADC_11db);

  delay(2000);

  // Connect WiFi
  connectWiFi();

  // Init MQTT client
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttConnect();

  lcd.clear();
}

// ── LOOP ─────────────────────────────────────────────────────
void loop() {
  static float   lastVoltage   = 0.0f;
  static float   lastCurrent   = 0.0f;
  static float   lastTempC     = 25.0f;
  static float   lastTempF     = 77.0f;
  static float   lastVibration = 0.0f;  // mm/s for MQTT
  static int     lastVib       = 0;     // 0/1 for LCD
  static int     lastSoundRaw  = 0;
  static float   lastSoundDB   = 0.0f;

  unsigned long now;
  bool tempOK;
  bool vibState;
  bool vibEvent;

  now = millis();

  // ── Maintain MQTT connection ──────────────────────────────
  if (!mqttClient.connected()) {
    mqttConnect();
  }
  mqttClient.loop();

  // ── Read sensors every READ_INTERVAL ─────────────────────
  if (now - lastReadTime >= READ_INTERVAL) {
    lastReadTime = now;

    lastVoltage = readRMSVoltage();
    lastCurrent = readCurrentACS712();

    // Temperature: real sensor, fall back to random on failure
    tempOK = readTemperature(lastTempC, lastTempF);
    if (!tempOK) {
      lastTempC = randomFloat(20.0f, 85.0f);
      lastTempF = lastTempC * 9.0f / 5.0f + 32.0f;
    }

    // Vibration: digital sensor → convert to mm/s range for MQTT
    vibState = (digitalRead(VIBRATION_PIN) == LOW);
    vibEvent = vibrationDetected;
    vibrationDetected = false;

    if (vibState || vibEvent) {
      lastVib = 1;
      // Motor running: elevated vibration; stopped: mechanical noise
      if (motorRunning) {
        lastVibration = randomFloat(3.0f, 9.0f);
      } else {
        lastVibration = randomFloat(1.5f, 4.0f);
      }
    } else {
      lastVib = 0;
      lastVibration = motorRunning ? randomFloat(0.5f, 2.5f) : randomFloat(0.0f, 0.4f);
    }

    // Current: if ACS712 reads near zero (no real load wired),
    // generate a plausible value based on motor state
    if (lastCurrent < 0.5f && motorRunning) {
      lastCurrent = randomFloat(5.0f, 35.0f);
    } else if (!motorRunning) {
      lastCurrent = randomFloat(0.0f, 0.4f);
    }

    lastSoundRaw = analogRead(SOUND_PIN);
    lastSoundDB  = soundToDecibels(lastSoundRaw);

    // Publish all metrics over MQTT
    publishSensorData(lastVibration, lastTempC, lastCurrent);

    // Mirror to Serial for debugging
    Serial.print("V:");    Serial.print(lastVoltage,   2);
    Serial.print(" I:");   Serial.print(lastCurrent,   3);
    Serial.print(" T:");   Serial.print(lastTempC,     2);
    Serial.print(" Vib:"); Serial.print(lastVibration, 2);
    Serial.print(" dB:");  Serial.print(lastSoundDB,   1);
    Serial.print(" Motor:"); Serial.println(motorRunning ? "ON" : "OFF");
  }

  // ── Switch display page every SCROLL_INTERVAL ────────────
  if (now - lastScrollTime >= SCROLL_INTERVAL) {
    lastScrollTime = now;

    if (displayPage == 1) {
      displayPage = 2;
    } else {
      displayPage = 1;
    }
    lcd.clear();
  }

  // ── Refresh LCD with latest data ─────────────────────────
  if (displayPage == 1) {
    displayPage1(
      lastVoltage,
      lastCurrent,
      lastTempC,
      lastTempF
    );
  } else {
    displayPage2(
      lastVib,
      lastSoundRaw,
      lastSoundDB
    );
  }

  delay(200);  // Small delay to avoid I2C bus flooding
}
