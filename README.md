# Páramo Kiln Monitor
### by Victor Moreno

A WiFi-connected kiln monitoring system built around the ESP32-C6, a Type S thermocouple, and a round TFT display. Monitor your kiln in real time from anywhere in the world via a web dashboard, and receive Telegram alerts when temperatures are reached.

---

## What you need

**Hardware**
- ESP32-C6 Super Mini
- Adafruit MAX31856 thermocouple breakout (Type S or K)
- GC9A01A 1.28" round TFT display
- Passive buzzer
- 100Ω potentiometer
- Perforated PCB (90×70mm recommended)

**Accounts (all free)**
- [HiveMQ Cloud](https://www.hivemq.com/mqtt-cloud-broker/) — MQTT broker
- [Telegram](https://telegram.org/) — for alerts

---

## 1. Setting up HiveMQ (your MQTT broker)

HiveMQ is the service that relays data between your kiln and the web dashboard. The free tier is more than enough.

**Step by step:**

1. Go to [hivemq.com/mqtt-cloud-broker](https://www.hivemq.com/mqtt-cloud-broker/) and create a free account
2. Click **Create Serverless Cluster** — no credit card required
3. Once created, copy your **cluster hostname** — it looks like:
   ```
   abc12345.s1.eu.hivemq.cloud
   ```
4. Go to **Access Management → Credentials → Add new credentials**
5. Create a username and password (example: user `paramo`, password of your choice)
6. Set permission to **Publish and Subscribe**
7. Save — you now have everything you need:
   ```
   Host:     abc12345.s1.eu.hivemq.cloud
   Port:     8883  (for the device)
   Port WSS: 8884  (for the web dashboard)
   Username: paramo
   Password: your_password
   ```

---

## 2. Setting up Telegram alerts

Telegram alerts are sent directly from the ESP32 — they work even if the web dashboard is closed.

**Step by step:**

1. Open Telegram and search for **@BotFather**
2. Send `/newbot` and follow the prompts — choose a name and username for your bot
3. BotFather will give you a **token** that looks like:
   ```
   123456789:ABCdefGHIjklMNOpqrSTUvwxYZ
   ```
   Copy it — this is your `TG_BOT_TOKEN`

4. Search for **@userinfobot** in Telegram, send `/start`
5. It will reply with your numeric ID:
   ```
   Id: 987654321
   ```
   This is your `TG_CHAT_ID`

6. **Important:** before the bot can message you, open your new bot in Telegram and press **Start** or send it any message. Without this step, the bot cannot reach you.

---

## 3. Configuring the device (KILN-SETUP)

The device uses a captive portal for configuration — no need to edit code or recompile.

**First time setup:**

1. Flash the firmware to your ESP32-C6 (Arduino IDE, Partition Scheme: Huge APP 3MB)
2. Power on the device — it will create a WiFi network called **KILN-SETUP**
3. Connect to **KILN-SETUP** from your phone or computer
4. A setup page will open automatically (if it doesn't, open your browser and go to `192.168.4.1`)
5. Fill in the fields:
   - **WiFi network** — select your network and enter the password
   - **HiveMQ Host** — your cluster hostname
   - **HiveMQ Username** — the username you created
   - **HiveMQ Password** — the password you created
   - **Telegram Token** — your bot token from BotFather
   - **Telegram Chat ID** — your numeric user ID
6. Click Save — the device will restart and connect automatically

**All credentials are saved on the device.** You only need to do this once. If you move the device to a new location or want to change credentials, use the **Reset Device Config** button in the web dashboard.

---

## 4. Web dashboard

Open `index.html` in any browser, or access it via GitHub Pages if you have it hosted there. No installation required.

### Connecting

1. Click **+ Add** in the Kilns panel
2. Enter a name for your kiln setup and your HiveMQ credentials (Host, Port 8884, Username, Password)
3. Optionally add your Telegram token and chat ID here too
4. Click Save — your kiln profile is stored in the browser for future sessions
5. Select the kiln from the list and click **Connect**

You can save multiple kiln profiles and switch between them.

### Temperature display

- **Kiln Temperature** — live reading from the thermocouple
- **Cold Junction** — ambient temperature at the sensor board
- **Rate of Change** — °C per hour, calculated over the last 60 seconds
- **Session High / Low** — peak and minimum temperatures since the session started

### Session history

The dashboard records every data point received while it is open and stores them in the browser (localStorage). When you reopen the dashboard, the previous curve is restored automatically.

- Click **Nueva Horneada** to clear the history and start a fresh session
- The session stops recording automatically once the kiln drops below 100°C after reaching 800°C (end of firing)
- Click **⬇ PNG** to export the temperature curve as an image

> **Note:** if you close the dashboard for a period, there will be a gap in the curve for that time. Future firmware versions will store data on the device itself to eliminate this.

### Threshold alerts

Set a temperature threshold using the **Threshold Alert** panel or the physical potentiometer on the device.

- Enter a temperature in °C and click **Establecer** to arm the alert
- When the kiln reaches that temperature, the ESP32 beeps and sends a Telegram message:
  ```
  Threshold Crossed!
  🎯 1050.2°C >= 1050°C
  📅 14:32 / 16/04/2026
  ```
- The threshold can be changed from the web at any time — the last value set (web or physical dial) takes priority
- Click **OFF** to disarm

### Ramp Analyzer

The ramp analyzer monitors the rate of temperature change and alerts you to potential problems. Select your firing type from the dropdown:

| Mode | Use case |
|------|----------|
| **Bisque** | First firing, clay still has moisture |
| **Glaze** | Standard glaze firing |
| **Crystalline** | Very slow ramps, long holds |
| **Raku** | Fast ramp, rapid cool-down expected |

The analyzer will alert you (in the dashboard and via Telegram) if:
- The ramp rate is too fast for the selected mode
- The temperature stalls unexpectedly
- A sudden drop is detected

**Tuning the parameters:** click **edit ▾** next to Firing Mode Params in the sidebar to adjust the thresholds for each mode. Changes are saved in the browser.

### Device reset

If you need to change WiFi network or update credentials, click **Reset Device Config** in the dashboard. The device will erase its saved credentials and restart into setup mode (KILN-SETUP network).

---

## Libraries required (Arduino IDE)

Install via Manage Libraries:

- Adafruit GC9A01A
- Adafruit GFX
- Adafruit MAX31856
- Adafruit BusIO
- WiFiManager (by Tzapu)
- PubSubClient (by Nick O'Leary)

---

## Arduino IDE settings

- **Board:** ESP32C6 Dev Module
- **Partition Scheme:** Huge APP (3MB No OTA / 1MB SPIFFS)

---

## Project structure

```
/
├── index.html          ← web dashboard (rename from kiln-dashboard.html)
├── paramo-logo.png     ← logo (must be in the same folder as index.html)
└── firmware/
    └── paramo.ino      ← ESP32 firmware
```

---

## License

Open source — feel free to adapt, build on, and share.
If you make something with it, we'd love to hear about it.

---

*Páramo by Victor Moreno · Built for ceramic artists who want to understand their kilns.*
