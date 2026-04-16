# GUC Motor Monitor

ESP32 multi-sensor firmware + Next.js dashboard connected over MQTT.

---

## Architecture

```text
ESP32 ──(TCP 1883)──► Mosquitto broker ◄──(WebSocket 9001)── Browser dashboard
                             │
                     publishes sensor data
                     subscribes to commands
```

The ESP32 publishes sensor readings over plain-TCP MQTT (port 1883).  
The Next.js dashboard connects to the same broker over MQTT-over-WebSocket (port 9001).  
Both listeners must be enabled on the same broker instance.

---

## MQTT Topics

| Direction       | Topic                | Payload          | Unit     |
| --------------- | -------------------- | ---------------- | -------- |
| ESP32 → broker  | `/motor/vibration`   | float            | mm/s     |
| ESP32 → broker  | `/motor/temperature` | float            | °C       |
| ESP32 → broker  | `/motor/current`     | float            | A        |
| ESP32 → broker  | `/motor/power`       | float            | W        |
| ESP32 → broker  | `/motor/health`      | float            | % (0–100)|
| broker → ESP32  | `/motor/command`     | `start` or `stop`| —        |

The dashboard publishes `start`/`stop` to `/motor/command`; the ESP32 receives that command and switches the relay on **GPIO17** (HIGH = motor on, LOW = motor off).

---

## Broker Setup (Mosquitto)

### Install

```bash
sudo apt install mosquitto mosquitto-clients   # Debian/Ubuntu
```

### Enable both TCP and WebSocket listeners

Edit `/etc/mosquitto/mosquitto.conf` (or create `/etc/mosquitto/conf.d/listeners.conf`):

```conf
# Plain TCP – used by ESP32 (PubSubClient)
listener 1883
allow_anonymous true

# WebSocket – used by the browser dashboard (mqtt.js)
listener 9001
protocol websockets
allow_anonymous true
```

Restart the broker:

```bash
sudo systemctl restart mosquitto
sudo systemctl enable mosquitto
```

Verify both ports are open:

```bash
sudo ss -tlnp | grep mosquitto
```

You should see entries for `*:1883` and `*:9001`.

---

## Firmware

### Hardware

| GPIO      | Sensor / Device                                               |
| --------- | ------------------------------------------------------------- |
| 12        | ZMPT101B (AC voltage, analog)                                 |
| 13        | ACS712 (current, analog)                                      |
| 18        | DS18B20 (temperature, OneWire)                                |
| 14        | Vibration sensor (digital, interrupt)                         |
| 32        | Sound sensor (analog)                                         |
| **17**    | **Relay module (motor control, active-HIGH)**                 |
| 21        | LCD SDA (I²C)                                                 |
| 22        | LCD SCL (I²C)                                                 |

> If your relay module is **active-LOW**, swap `HIGH`/`LOW` in the `mqttCallback` function inside `firmware.ino`.

### Required Arduino libraries

Install via **Sketch → Include Library → Manage Libraries**:

| Library            | Author               |
| ------------------ | -------------------- |
| LiquidCrystal I2C  | Frank de Brabander   |
| PubSubClient       | Nick O'Leary         |

The ESP32 board package (which provides `WiFi.h`) must be installed via **Boards Manager**:  
URL to add: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`  
Then install: **esp32 by Espressif Systems**.

### Configuration

Open `firmware.ino` and set these defines at the top before flashing:

```cpp
#define WIFI_SSID     "YourSSID"
#define WIFI_PASSWORD "YourPassword"
#define MQTT_BROKER   "192.168.1.100"   // LAN IP of the machine running Mosquitto
```

`MQTT_PORT` is `1883` by default — change only if you moved the TCP listener.

### Flash

1. Select board: **ESP32 Dev Module** (or your specific variant)
2. Select the correct COM/ttyUSB port
3. Upload

The LCD will show WiFi connection progress then switch to live sensor data. MQTT connection status and motor state appear on row 3 of page 1.

### Simulated / fallback values

When a sensor is absent or returns an error the firmware generates realistic random values so the dashboard always receives data:

| Metric            | Real sensor                  | Fallback range                     |
| ----------------- | ---------------------------- | ---------------------------------- |
| Temperature       | DS18B20                      | 20–85 °C (random)                  |
| Vibration (mm/s)  | Digital sensor → converted   | 0–2.5 idle / 3–9 vibrating         |
| Current           | ACS712                       | 0–0.4 A stopped / 5–35 A running   |
| Power             | Computed (`220 V × I`)       | derived from current above         |
| Health            | Computed from vib + temp + I | derived                            |

---

## Dashboard (Next.js)

### Run

```bash
npm install
npm run dev
```

Open [http://localhost:3000](http://localhost:3000).

### Connect to broker

1. Enter the broker WebSocket URL in the **Broker WebSocket URL** field:
   - Same machine: `ws://localhost:9001`
   - Remote broker: `ws://192.168.1.100:9001`
2. Click **Connect**.

The status dot turns green when the broker is reachable. The **Motor Online** indicator activates within the first MQTT message from the ESP32 (heartbeat timeout is 10 s).

### Motor control

Click **Start** / **Stop** in the Motor Control panel. The dashboard publishes `start` or `stop` to `/motor/command`. The ESP32 receives the command and switches the relay on GPIO17 accordingly. The dashboard updates its motor-state display optimistically (no round-trip confirmation).

---

## Troubleshooting

| Symptom                                          | Likely cause                    | Fix                                              |
| ------------------------------------------------ | ------------------------------- | ------------------------------------------------ |
| ESP32 Serial shows `MQTT FAILED rc=-2`           | Broker unreachable              | Check IP, firewall, port 1883                    |
| Dashboard shows "Broker Offline"                 | WebSocket port closed           | Verify port 9001 in mosquitto config             |
| Motor Online indicator never lights              | ESP32 not publishing            | Open Serial Monitor, check MQTT output           |
| Relay clicks but motor doesn't start             | Wiring / relay polarity         | Swap HIGH/LOW in `mqttCallback`                  |
| DS18B20 shows `SENSOR ERR` on LCD                | Missing pull-up resistor        | Add 4.7 kohm pull-up from GPIO18 to 3.3 V       |
| IntelliSense errors for WiFi.h / PubSubClient.h  | Arduino include path not set up | Compile via Arduino IDE; ignore in VS Code       |
