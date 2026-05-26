/*
 * ESP32 Motor Monitor
 * Sensors : DS18B20 (Temperature) | ZMPT101B (AC Voltage) | ACS712 (AC Current)
 * Display : 16x4 I2C LCD
 * Comms   : WiFi + MQTT (PubSubClient)
 * Control : Motor Relay | Buzzer | Status LEDs
 *
 * ─── Pin Configuration ────────────────────────────────────────────
 *   DS18B20  DATA  ──► GPIO 18  (4.7 kΩ pull-up to 3.3 V required)
 *   ZMPT101B OUT   ──► GPIO 32
 *   ACS712   OUT   ──► GPIO 34  (input-only ADC pin)
 *   LCD      SDA   ──► GPIO 21
 *   LCD      SCL   ──► GPIO 22
 *   Relay    IN    ──► GPIO 17  (HIGH = motor ON)
 *   Buzzer         ──► GPIO 19
 *   LED RED        ──► GPIO 13
 *   LED GREEN      ──► GPIO 16
 *
 * ─── Libraries (Arduino Library Manager) ─────────────────────────
 *   ZMPT101B          by Abdurraziq Bachmid
 *   LiquidCrystal_I2C by Frank de Brabander
 *   OneWire           by Paul Stoffregen
 *   DallasTemperature by Miles Burton
 *   PubSubClient      by Nick O'Leary
 *
 * ─── MQTT Topics ──────────────────────────────────────────────────
 *   Publish:   /motor/temperature  (°C, float)
 *              /motor/current      (A,  float)
 *              /motor/power        (VA, apparent power)
 *              /motor/voltage      (V,  float)
 *              /motor/vibration    (mm/s, simulated fan model)
 *              /motor/health       (100 = OKAY | 0 = NOT OKAY)
 *              /motor/fault        ("NONE"|"UNDERVOLTAGE"|"OVERVOLTAGE"|"HIGH_TEMP")
 *              /motor/state        ("ON" | "OFF")
 *   Subscribe: /motor/command      ("start" | "stop")
 *
 * ─── Fault Thresholds ─────────────────────────────────────────────
 *   Voltage < 30 V   → noise floor; displayed as 0 V
 *   Voltage < 210 V  → UNDERVOLTAGE fault → motor forced OFF
 *   Voltage > 250 V  → OVERVOLTAGE fault  → motor forced OFF
 *   Temp    > 50 °C  → HIGH_TEMP fault    → motor forced OFF
 *   On any fault: RED LED on, GREEN off, buzzer beep (on transition)
 *   Motor start commands are rejected while a fault is active.
 *
 * ─── Vibration Model (ISO 10816 Class I — small fan motor) ────────
 *   Simulated steady-state: 1.2 mm/s with ±0.10 mm/s noise
 *   Ramp-up  : 0.25 mm/s/s  (~5 s to reach steady state)
 *   Ramp-down: 0.40 mm/s/s  (~3 s to stop after motor off)
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ZMPT101B.h>
#include <WiFi.h>
#include <PubSubClient.h>

// ─────────────────────────────────────────────
//  WiFi credentials  ← fill in before flashing
// ─────────────────────────────────────────────
#define WIFI_SSID     "YourSSID"
#define WIFI_PASSWORD "YourPassword"

// ─────────────────────────────────────────────
//  MQTT broker (plain TCP 1883 | dashboard uses WS 9001)
// ─────────────────────────────────────────────
#define MQTT_BROKER    "192.168.1.100"
#define MQTT_PORT      1883
#define MQTT_CLIENT_ID "esp32-motor-monitor"

// ─────────────────────────────────────────────
//  MQTT topics
// ─────────────────────────────────────────────
#define TOPIC_TEMPERATURE "/motor/temperature"
#define TOPIC_CURRENT     "/motor/current"
#define TOPIC_POWER       "/motor/power"
#define TOPIC_VOLTAGE     "/motor/voltage"
#define TOPIC_VIBRATION   "/motor/vibration"
#define TOPIC_HEALTH      "/motor/health"
#define TOPIC_FAULT       "/motor/fault"
#define TOPIC_STATE       "/motor/state"
#define TOPIC_COMMAND     "/motor/command"

// ─────────────────────────────────────────────
//  Pin definitions
// ─────────────────────────────────────────────
#define ONE_WIRE_BUS  18
#define VOLTAGE_PIN   32
#define CURRENT_PIN   34
#define MOTOR_RELAY   17   // HIGH = motor ON
#define BUZZER_PIN    19
#define LED_RED       13
#define LED_GREEN     16

// ─────────────────────────────────────────────
//  LCD  (try 0x3F if screen stays blank)
// ─────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 4);

// ─────────────────────────────────────────────
//  DS18B20
// ─────────────────────────────────────────────
OneWire           oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);

// ─────────────────────────────────────────────
//  ZMPT101B — voltage sensitivity calibration
//
//  HOW TO CALIBRATE:
//  1. Set VOLTAGE_SENSITIVITY to 50.0
//  2. Upload and note the Serial voltage reading.
//  3. Measure real mains with a multimeter.
//  4. New sensitivity = (serial_reading / real_voltage) * current_sensitivity
// ─────────────────────────────────────────────
#define MAINS_FREQUENCY     50.0f   // 60.0 for 60 Hz grids
#define VOLTAGE_SENSITIVITY 250.0f  // ← tune to match multimeter

ZMPT101B voltageSensor(VOLTAGE_PIN, MAINS_FREQUENCY);

// ─────────────────────────────────────────────
//  ACS712 — current calibration
//    ACS712-05B : 185 mV/A  ← default
//    ACS712-20A : 100 mV/A
//    ACS712-30A :  66 mV/A
// ─────────────────────────────────────────────
#define ACS712_MV_PER_A  185.0f
#define ADC_COUNTS       4095.0f
#define VREF_MV          3300.0f
#define CURRENT_SAMPLES  1000

int adcMidpoint = 1650;  // overwritten by auto-calibration at startup

// ─────────────────────────────────────────────
//  Fault thresholds
// ─────────────────────────────────────────────
#define VOLTAGE_NOISE_FLOOR  30.0f
#define VOLTAGE_LOW         210.0f
#define VOLTAGE_HIGH        250.0f
#define TEMP_HIGH            50.0f

// ─────────────────────────────────────────────
//  Vibration simulation (ISO 10816 Class I fan)
//  Steady-state target: 1.2 mm/s
//  A well-maintained small fan is "Satisfactory" below 2.8 mm/s
// ─────────────────────────────────────────────
#define VIB_TARGET   1.2f   // mm/s steady-state
#define VIB_RAMP_UP  0.25f  // mm/s gain per REFRESH_MS cycle
#define VIB_RAMP_DN  0.40f  // mm/s loss per REFRESH_MS cycle
#define VIB_NOISE    0.10f  // ± mm/s random noise amplitude

// ─────────────────────────────────────────────
//  Timing
// ─────────────────────────────────────────────
#define REFRESH_MS  1000
unsigned long lastRefresh = 0;

// ─────────────────────────────────────────────
//  Globals
// ─────────────────────────────────────────────
float tempC              = 0.0f;
float voltageRMS         = 0.0f;
float currentRMS         = 0.0f;
float apparentPower      = 0.0f;
float simulatedVibration = 0.0f;
bool  motorRunning       = false;

typedef enum {
  FAULT_NONE = 0,
  FAULT_UNDERVOLTAGE,
  FAULT_OVERVOLTAGE,
  FAULT_HIGH_TEMP
} FaultCode;

FaultCode currentFault  = FAULT_NONE;
FaultCode previousFault = FAULT_NONE;

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

// ─────────────────────────────────────────────
//  Auto-calibrate ACS712 zero point
//  Call ONCE at startup with NO load connected.
// ─────────────────────────────────────────────
void calibrateCurrentZero() {
  Serial.println("[ACS712] Calibrating zero — ensure NO load is connected...");
  long sum = 0;
  for (int i = 0; i < 500; i++) {
    sum += analogRead(CURRENT_PIN);
    delay(2);
  }
  adcMidpoint = sum / 500;
  Serial.printf("[ACS712] Zero point ADC = %d\n", adcMidpoint);
}

// ─────────────────────────────────────────────
//  Read DS18B20
//  Returns -99 if sensor is disconnected.
// ─────────────────────────────────────────────
float readTemperature() {
  tempSensor.requestTemperatures();
  float t = tempSensor.getTempCByIndex(0);
  if (t == DEVICE_DISCONNECTED_C) {
    Serial.println("[DS18B20] Disconnected!");
    return -99.0f;
  }
  return t;
}

// ─────────────────────────────────────────────
//  Read ZMPT101B (5-cycle average)
//  Values below the noise floor are treated as 0.
// ─────────────────────────────────────────────
float readVoltage() {
  float v = voltageSensor.getRmsVoltage(5);
  if (v < VOLTAGE_NOISE_FLOOR) v = 0.0f;
  return v;
}

// ─────────────────────────────────────────────
//  Read ACS712 — true RMS current
// ─────────────────────────────────────────────
float readCurrentRMS() {
  long sumSq = 0;
  for (int i = 0; i < CURRENT_SAMPLES; i++) {
    long raw = (long)analogRead(CURRENT_PIN) - adcMidpoint;
    sumSq   += raw * raw;
    delayMicroseconds(100);
  }
  float rmsRaw   = sqrtf((float)sumSq / (float)CURRENT_SAMPLES);
  float currentA = (rmsRaw / ADC_COUNTS * VREF_MV) / ACS712_MV_PER_A;
  if (currentA < 0.03f) currentA = 0.0f;
  return currentA;
}

// ─────────────────────────────────────────────
//  Update vibration simulation
//  Models a small fan motor (ISO 10816 Class I):
//    Running  → ramp toward VIB_TARGET + random noise
//    Stopped  → ramp toward 0
// ─────────────────────────────────────────────
void updateVibration() {
  if (motorRunning) {
    simulatedVibration += VIB_RAMP_UP;
    if (simulatedVibration > VIB_TARGET) simulatedVibration = VIB_TARGET;
    // Add realistic noise (±VIB_NOISE)
    float noise = ((float)(random(0, 2000) - 1000) / 1000.0f) * VIB_NOISE;
    simulatedVibration = constrain(simulatedVibration + noise, 0.0f, 10.0f);
  } else {
    simulatedVibration -= VIB_RAMP_DN;
    if (simulatedVibration < 0.0f) simulatedVibration = 0.0f;
  }
}

// ─────────────────────────────────────────────
//  Detect the highest-priority active fault
// ─────────────────────────────────────────────
FaultCode detectFault() {
  // Only flag undervoltage if the sensor is reading some voltage
  // (avoids false trigger when mains is simply disconnected / sensor unwired)
  if (voltageRMS > 0.0f && voltageRMS < VOLTAGE_LOW)  return FAULT_UNDERVOLTAGE;
  if (voltageRMS > VOLTAGE_HIGH)                       return FAULT_OVERVOLTAGE;
  if (tempC != -99.0f && tempC > TEMP_HIGH)            return FAULT_HIGH_TEMP;
  return FAULT_NONE;
}

// ─────────────────────────────────────────────
//  Fault string for MQTT publish
// ─────────────────────────────────────────────
const char* faultMqttStr(FaultCode f) {
  switch (f) {
    case FAULT_UNDERVOLTAGE: return "UNDERVOLTAGE";
    case FAULT_OVERVOLTAGE:  return "OVERVOLTAGE";
    case FAULT_HIGH_TEMP:    return "HIGH_TEMP";
    default:                 return "NONE";
  }
}

// ─────────────────────────────────────────────
//  Fault label for LCD row 3 — exactly 16 chars
// ─────────────────────────────────────────────
const char* faultLcdRow(FaultCode f) {
  switch (f) {
    case FAULT_UNDERVOLTAGE: return "FAULT:UNDERVOLT ";
    case FAULT_OVERVOLTAGE:  return "FAULT:OVERVOLT  ";
    case FAULT_HIGH_TEMP:    return "FAULT:HIGH TEMP ";
    default:                 return "Health: OKAY    ";
  }
}

// ─────────────────────────────────────────────
//  Apply fault: force motor off, drive LEDs & buzzer
//  Called every cycle after detectFault().
// ─────────────────────────────────────────────
void applyFaultState() {
  if (currentFault != FAULT_NONE) {
    // Force motor off on any active fault
    motorRunning = false;
    digitalWrite(MOTOR_RELAY, LOW);

    // Red LED on, green off
    digitalWrite(LED_RED,   HIGH);
    digitalWrite(LED_GREEN, LOW);

    // Beep only on the transition into (or between) faults
    if (currentFault != previousFault) {
      tone(BUZZER_PIN, 1000, 500);   // 1 kHz, 500 ms
    }
  } else {
    // System healthy
    digitalWrite(LED_RED,   LOW);
    digitalWrite(LED_GREEN, HIGH);

    // Short "all clear" beep when fault just cleared
    if (previousFault != FAULT_NONE) {
      tone(BUZZER_PIN, 2000, 150);   // 2 kHz, 150 ms
    }
  }
}

// ─────────────────────────────────────────────
//  Update 16×4 LCD (no clear — each row is
//  overwritten with exactly 16 characters)
//
//  Row 0: Mtr:ON  MQTT:OK
//  Row 1: V:230.1V I:1.23A   (note: I in mA range for this fan)
//  Row 2: Temp: 25.3°C
//  Row 3: Health: OKAY       (or fault label)
// ─────────────────────────────────────────────
void updateDisplay() {
  char buf[17];  // 16 chars + null terminator

  // Row 0 — motor + MQTT status
  // "Mtr:ON  MQTT:OK " / "Mtr:OFF MQTT:ERR" → 16 chars
  snprintf(buf, sizeof(buf), "Mtr:%-3s MQTT:%-3s",
           motorRunning ? "ON" : "OFF",
           mqttClient.connected() ? "OK" : "ERR");
  lcd.setCursor(0, 0);
  lcd.print(buf);

  // Row 1 — voltage + current
  // "V:230.1V I:1.23A" → 16 chars
  snprintf(buf, sizeof(buf), "V:%5.1fV I:%4.2fA", voltageRMS, currentRMS);
  lcd.setCursor(0, 1);
  lcd.print(buf);

  // Row 2 — temperature
  // "Temp: 25.3°C    " → 16 chars
  lcd.setCursor(0, 2);
  if (tempC == -99.0f) {
    lcd.print("Temp: NO SENSOR ");   // exactly 16 chars
  } else {
    // "Temp:" (5) + %5.1f (5) + ° (1) + "C    " (5) = 16
    snprintf(buf, sizeof(buf), "Temp:%5.1f", tempC);
    lcd.print(buf);
    lcd.print((char)223);  // ° symbol
    lcd.print("C    ");
  }

  // Row 3 — health / fault (always exactly 16 chars from faultLcdRow)
  lcd.setCursor(0, 3);
  lcd.print(faultLcdRow(currentFault));
}

// ─────────────────────────────────────────────
//  WiFi connect (with LCD splash)
// ─────────────────────────────────────────────
void connectWiFi() {
  Serial.printf("[WiFi] Connecting to %s\n", WIFI_SSID);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("WiFi connecting...");
  lcd.setCursor(0, 1);
  lcd.print(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());
    lcd.setCursor(0, 2);
    lcd.print("IP: ");
    lcd.print(WiFi.localIP());
  } else {
    Serial.println("[WiFi] FAILED — continuing offline");
    lcd.setCursor(0, 2);
    lcd.print("WiFi FAILED!    ");
    lcd.setCursor(0, 3);
    lcd.print("Running offline.");
  }
  delay(2000);
}

// ─────────────────────────────────────────────
//  MQTT command callback
//  Handles "start" and "stop" on /motor/command
// ─────────────────────────────────────────────
void mqttCallback(char *topic, byte *payload, unsigned int length) {
  char msg[16];
  if (length >= sizeof(msg)) length = sizeof(msg) - 1;
  for (unsigned int i = 0; i < length; i++) msg[i] = (char)payload[i];
  msg[length] = '\0';

  if (strcmp(topic, TOPIC_COMMAND) != 0) return;

  if (strcmp(msg, "start") == 0) {
    if (currentFault == FAULT_NONE) {
      motorRunning = true;
      digitalWrite(MOTOR_RELAY, HIGH);
      Serial.println("[Motor] START");
    } else {
      Serial.printf("[Motor] START rejected — active fault: %s\n",
                    faultMqttStr(currentFault));
    }
  } else if (strcmp(msg, "stop") == 0) {
    motorRunning = false;
    digitalWrite(MOTOR_RELAY, LOW);
    Serial.println("[Motor] STOP");
  }
}

// ─────────────────────────────────────────────
//  MQTT connect / reconnect
// ─────────────────────────────────────────────
bool mqttConnect() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (mqttClient.connected())        return true;

  Serial.print("[MQTT] Connecting...");
  if (mqttClient.connect(MQTT_CLIENT_ID)) {
    mqttClient.subscribe(TOPIC_COMMAND);
    Serial.println(" OK");
    return true;
  }
  Serial.printf(" FAILED rc=%d\n", mqttClient.state());
  return false;
}

// ─────────────────────────────────────────────
//  Publish all sensor data + state over MQTT
// ─────────────────────────────────────────────
void publishData() {
  if (!mqttClient.connected()) return;

  char buf[16];

  // Numeric sensor values
  dtostrf(tempC,              5, 2, buf);  mqttClient.publish(TOPIC_TEMPERATURE, buf);
  dtostrf(currentRMS,         5, 3, buf);  mqttClient.publish(TOPIC_CURRENT,     buf);
  dtostrf(apparentPower,      7, 2, buf);  mqttClient.publish(TOPIC_POWER,       buf);
  dtostrf(voltageRMS,         6, 2, buf);  mqttClient.publish(TOPIC_VOLTAGE,     buf);
  dtostrf(simulatedVibration, 4, 2, buf);  mqttClient.publish(TOPIC_VIBRATION,   buf);

  // Health: 100 = OKAY, 0 = NOT OKAY (compatible with dashboard gauge)
  dtostrf(currentFault == FAULT_NONE ? 100.0f : 0.0f, 5, 1, buf);
  mqttClient.publish(TOPIC_HEALTH, buf);

  // Fault string and motor state as plain strings
  mqttClient.publish(TOPIC_FAULT,  faultMqttStr(currentFault));
  mqttClient.publish(TOPIC_STATE,  motorRunning ? "ON" : "OFF");
}

// ─────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32 Motor Monitor ===");

  // Output pins — safe defaults before anything else
  pinMode(MOTOR_RELAY, OUTPUT);  digitalWrite(MOTOR_RELAY, LOW);
  pinMode(BUZZER_PIN,  OUTPUT);  digitalWrite(BUZZER_PIN,  LOW);
  pinMode(LED_RED,     OUTPUT);  digitalWrite(LED_RED,     LOW);
  pinMode(LED_GREEN,   OUTPUT);  digitalWrite(LED_GREEN,   LOW);

  // ESP32 ADC: 12-bit, full 0–3.3 V range
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // DS18B20
  tempSensor.begin();
  Serial.printf("[DS18B20] Found %d sensor(s)\n", tempSensor.getDeviceCount());

  // ZMPT101B
  voltageSensor.setSensitivity(VOLTAGE_SENSITIVITY);
  Serial.printf("[ZMPT101B] Sensitivity: %.1f\n", VOLTAGE_SENSITIVITY);

  // LCD startup splash
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("ESP32 Motor Mon.");
  lcd.setCursor(0, 1);
  lcd.print("Calibrating I...");
  lcd.setCursor(0, 2);
  lcd.print("No load on ACS! ");

  // ACS712 — auto-calibrate zero (no load during this window)
  delay(500);
  calibrateCurrentZero();
  delay(1000);

  // WiFi + MQTT
  connectWiFi();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttConnect();

  // Start with GREEN LED (healthy until first measurement)
  digitalWrite(LED_GREEN, HIGH);

  lcd.clear();
  Serial.println("[System] Ready.\n");
}

// ─────────────────────────────────────────────
//  LOOP
// ─────────────────────────────────────────────
void loop() {
  // Maintain MQTT connection
  if (!mqttClient.connected()) mqttConnect();
  mqttClient.loop();

  unsigned long now = millis();
  if (now - lastRefresh >= REFRESH_MS) {
    lastRefresh = now;

    // ── Read sensors ──────────────────────────
    tempC         = readTemperature();
    voltageRMS    = readVoltage();
    currentRMS    = readCurrentRMS();
    apparentPower = voltageRMS * currentRMS;

    // ── Fault detection + actions ─────────────
    previousFault = currentFault;
    currentFault  = detectFault();
    applyFaultState();

    // ── Vibration simulation ──────────────────
    updateVibration();

    // ── MQTT publish ──────────────────────────
    publishData();

    // ── LCD update ────────────────────────────
    updateDisplay();

    // ── Serial debug ──────────────────────────
    Serial.println("─────────────────────────");
    Serial.printf("Temperature  : %.2f C\n",   tempC);
    Serial.printf("Voltage RMS  : %.2f V\n",   voltageRMS);
    Serial.printf("ADC Midpoint : %d\n",        adcMidpoint);
    Serial.printf("Current RMS  : %.3f A\n",   currentRMS);
    Serial.printf("Apparent Pwr : %.2f VA\n",  apparentPower);
    Serial.printf("Vibration    : %.2f mm/s\n", simulatedVibration);
    Serial.printf("Fault        : %s\n",        faultMqttStr(currentFault));
    Serial.printf("Motor        : %s\n",        motorRunning ? "ON" : "OFF");
    Serial.printf("MQTT         : %s\n",        mqttClient.connected() ? "OK" : "DISCONNECTED");
  }
}
