# Arduino Distillation Column Controller (v48.0)

> A hobby project from my university days — recently dusted off and modernized with the help of AI.

Distillation process controller built around an Arduino MCU with a WiFiNINA module. Provides precise control of fraction collection (foreshots / hearts / tails), automatic boiling-point compensation versus atmospheric pressure, online monitoring via MQTT (ThingSpeak), and a responsive web UI accessible from any phone browser on the local network.

## 🚀 Key features

* **Web application (SPA/AJAX)** — embedded HTML/JS UI served straight from Arduino. Works on any phone or computer in the same WiFi, no app install required. Mode buttons reflect real-time state in color: green = stable collection, yellow = waiting for stabilization, purple = paused, blinking yellow = column ready / awaiting choice.
* **Physical button control** — short press for Foreshots, long press (≥1 s) for Hearts. Tails available only via web UI.
* **Last-rate memory** — when the column briefly destabilizes (temperature spike), the UI shows a grey "previously X ml/min" hint per fraction.
* **Pressure compensation** — the boiling point is computed live from the BMP280 reading (linear interpolation from a BP-vs-pressure catalog). The operator can fine-tune via the "BP correction" buttons.
* **Two-sided hysteresis** — critical stop when `columnT ≥ BP + 0.4°C`, recovery when `columnT ≤ BP − 0.2°C`. Checked every 150 s, so brief noise does not cycle the pump.
* **Soft restart after fault** — every critical stop reduces the target rate by 2 ml/min. The system naturally finds an equilibrium rate for the current ethanol concentration in the keg.
* **Foreshots auto-cutoff** — automatic stop after 200 ml. UI displays a sticky "Foreshots Done" message until the operator picks the next mode.
* **Hard FINISHED stop** — when `kegT ≥ 96°C`. Pump off, but UI/HTTP remain responsive (restart available via web).
* **ThingSpeak monitoring (8 fields)** — two temperatures, pressure, current rate, state, and per-fraction collected volumes. 20 s interval, compatible with the free-tier 15 s rate limit.
* **Offline mode** — if WiFi is unavailable at boot, the controller still runs. Physical button works, sensors work, the process runs. When WiFi appears later, the UI comes up on its own.
* **Async sensor read** — DS18B20 in non-blocking mode; the main loop never stalls on the 750 ms conversion every second.

## 🧰 Hardware components

| Component | Description |
| :--- | :--- |
| **Arduino with WiFiNINA module** | MCU (e.g. MKR WiFi 1010 / Nano 33 IoT / Uno WiFi Rev2) |
| **12 V peristaltic pump** | Fraction collection pump (DC motor) |
| **L298N** | Dual-channel DC motor driver — drives the pump via PWM + DIR |
| **2× DS18B20 (waterproof probe)** | Temperature sensors: keg and column head |
| **BMP280** | Atmospheric pressure + ambient temperature sensor (I2C) |
| **Momentary push button** | Manual control (Foreshots / Hearts) |
| **2× LED (green, red)** | Process state indicators |

## 🛠 Wiring (Pinout)

| Component | Arduino pin | Description |
| :--- | :--- | :--- |
| **DS18B20 (bus)** | D2 | OneWire bus for both temperature sensors |
| **Motor PWM** | D3 | PWM signal driving the pump (through L298N IN/EN) |
| **Motor DIR** | D4 | Pump direction (through L298N) |
| **Button** | D7 | Multi-function button (shorts to GND, INPUT_PULLUP) |
| **LED Green** | D9 | Foreshots / stabilization indicator |
| **LED Red** | D10 | Hearts / stabilization indicator |
| **BMP280** | I2C (SDA/SCL) | Pressure and ambient temperature |

## ⚙️ Software setup

### 1. Secrets

Copy `secrets_template.h` to `secrets.h` (it is in `.gitignore`, so it never gets committed) and fill in:

* **WiFi:** SSID + password
* **MQTT (ThingSpeak):** Client ID, username, password (from the *MQTT Devices* tab in ThingSpeak)
* **`THINGSPEAK_CHANNEL_ID`** — channel ID (digits only, no quotes)

### 2. DS18B20 sensor addresses

Edit [`dist_controller_final.ino`](dist_controller_final.ino) and put your probes' unique 8-byte HEX addresses into `kegSensorAddress` and `columnSensorAddress`. You can read them with the OneWire/DallasTemperature *Tester* example sketch.

### 3. ThingSpeak field mapping

In your ThingSpeak channel settings, label the eight fields in this exact order (the controller publishes in this order):

| Field   | Suggested name | Value |
|---------|----------------|-------|
| field1  | Temp_Keg       | keg temperature [°C] |
| field2  | Temp_Column    | column-head temperature [°C] |
| field3  | Pressure       | atmospheric pressure [hPa] |
| field4  | Rate           | current collection rate [ml/min] |
| field5  | State          | 0=Idle, 1=Foreshots, 2=Hearts, 3=Tails |
| field6  | V_foreshots    | foreshots volume [ml] |
| field7  | V_hearts       | hearts volume [ml] |
| field8  | V_tails        | tails volume [ml] |

## 📚 Required libraries

All available in the Arduino IDE Library Manager:

* `WiFiNINA` (Arduino)
* `OneWire`
* `DallasTemperature`
* `Adafruit_BMP280` + `Adafruit_Unified_Sensor` (dependency)
* `PubSubClient` **version ≥ 2.8** — earlier versions lack `setSocketTimeout()`

## 🌐 Web app

### First start

1. Power the Arduino. In the Serial Monitor (115200 baud) you will see the assigned IP, e.g. `IP address: 192.168.1.20`.
2. Open that IP in any browser on the **same WiFi network**.
3. UI loads instantly, data refreshes every 1.5 s.

### Static IP (recommended)

To prevent the IP from changing after a router reboot, configure a **DHCP reservation** in your router:

1. Log into the router admin (typically `192.168.0.1` or `192.168.1.1`).
2. Find the LAN/DHCP/Address Reservation section.
3. Bind your chosen IP to the controller's MAC address (Arduino usually shows up as *Espressif* or *Arduino*).
4. Save and reboot the router.

### Mode button states (Foreshots / Hearts / Tails)

| Appearance | Meaning |
| :--- | :--- |
| 🔘 Gray | Mode available but not selected. Clickable. |
| 🟢 Green (locked) | Selected mode, column stable, collection in progress |
| 🟡 Yellow (locked) | Selected mode, column not yet stable — pump is off |
| 🔘 Gray dim (opacity 0.4) | Another mode is active and waiting for stabilization — switch is locked |
| 🟡 Blinking yellow | Column stable, no mode selected — choose Foreshots/Hearts/Tails |
| 🟣 Purple (locked) | System paused (**Pause** clicked) — everything locked until **Resume** |

### Pause / Resume

* **Pause** stops the pump immediately, but **does not change mode or target rate**. All `lock-on-pause` buttons go purple. Resume and Restart remain active.
* **Resume** clears the pause flag. Column stability is determined naturally by the sensor logic — `?start` no longer forces it.

### Restart

The blue **Restart** button at the very bottom requires a `confirm()` dialog to avoid accidental clicks. The current process is interrupted; the MCU performs a full reset.

## 🕹 Manual control (physical button)

* **Short press** — Foreshots
* **Long press (≥ 1 s)** — Hearts

> ℹ️ **Tails** is available **only via the web UI**.

## 💡 LED indicators

| State | Green | Red |
| :--- | :---: | :---: |
| Column stable, awaiting selection | fast alternating blink (100 ms) | |
| Foreshots, waiting for stabilization | 500 ms blink | off |
| Foreshots, stable collection | solid on | off |
| Hearts, waiting for stabilization | off | 500 ms blink |
| Hearts, stable collection | off | solid on |

## 📡 ThingSpeak monitoring

The controller publishes data every 20 seconds (compatible with the free-tier 15 s rate limit). Connection to `mqtt3.thingspeak.com:1883`. After a failed connect — 30 s backoff before retry. KeepAlive 60 s, socket timeout 2 s — the main loop never blocks waiting on the network.

## 📴 Offline mode (no WiFi)

Arduino starts up **even without WiFi**:

* Connection attempts: 4 × 5 s = max 20 s during boot.
* If unsuccessful, continues offline. Physical button, sensors, pump, LEDs — all work.
* In the background tries to log into WiFi every 30 s. When the router returns, the UI starts itself (server.begin after reconnect).
* Missing WiFi module (`WL_NO_MODULE`) — the system does *not* hang, continues offline.

## 🛠 Developer tools

### Simulator: `simulation.py`

Pure Python (stdlib only). Logic is a 1:1 port of `dist_controller_final.ino` — lets you test parameter tweaks (`TEMP_HYSTERESIS_C`, `STABILIZATION_CHECK_INTERVAL_S`, `COLLECTION_RATE_HYSTERESIS`, …) **without touching the hardware**.

```bash
python simulation.py                  # auto: replay example_run.csv if present
python simulation.py my_log.csv       # replay your own ThingSpeak export
python simulation.py --synthetic      # synthetic scenario
```

CSV must have columns: `Time, Seq, Temp_Keg, Temp_Column, Pressure, Rate, State`. The simulator also detects manual operator interventions (e.g. manual `?plus` mid-process).

### Offline UI preview: `regen_preview.py`

Extracts the HTML from `index.h` and bolts on a JS mock layer — a fully interactive UI preview **without Arduino, without a server**:

```bash
python regen_preview.py
start web_ui_preview.html         # Windows
# or: open web_ui_preview.html (macOS), xdg-open web_ui_preview.html (Linux)
```

In the browser you will see an orange "OFFLINE PREVIEW" banner and a test panel with 6 scenarios (READY, HEATING, FINISHED, DESTABILIZE, RESTABILIZE, FORESHOTS DONE) for quickly checking every UI state.

After each change to `index.h`, run `regen_preview.py` again to refresh the preview.

## 🔍 Serial monitor (debug)

Compact format, one line per second:

```
[t=12345s] keg=85.20 col=78.30 P=970.20 BP=77.20 st=Hearts stab=Y pause=N rate=40
```

Plus prefixed events:

* `[STATE] ...` — state changes (Foreshots waiting, Hearts waiting, column stable)
* `!!! CRITICAL STOP ...` / `STABILIZATION: ...` — critical stops and recoveries
* `[WiFi] ...` — connection lost / regained, IP
* `[MQTT] ...` — connect, publish OK/FAIL

**Baud:** 115200.

## 🛡 Safety

This project is for **educational and hobby** use. Distillation requires constant supervision by someone aware of the hazards (high temperatures, flammable vapors, pressure).

Built-in safeguards:

* **Critical Safety Check** — cuts the pump when column temperature leaves the `BP ± 0.4°C` window or drops below 75°C
* **Hard FINISHED stop** — at `kegT ≥ 96°C` the process ends permanently (until restart)
* **Recovery hysteresis** — the system does not start/stop oscillate on frequent spikes

This **does not absolve** the operator of responsibility. Never leave a running process unattended.

---
**Version:** 48.0 (Modern Web UI)
