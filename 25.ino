// ════════════════════════════════════════════════════════════════
//  KILN CONTROLLER · ESP32-C6 Super Mini
//  Fixes: sin fillRect glitch, status simplificado,
//         layout corregido, pot saltos 25°, buzzer fiable
// ════════════════════════════════════════════════════════════════

#include "SPI.h"
#include "Adafruit_GFX.h"
#include "Adafruit_GC9A01A.h"
#include "Adafruit_MAX31856.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <time.h>
#include <Preferences.h>   // guardar config en flash

// ════════════════════════════════════════════════════════════════
//  PINES
// ════════════════════════════════════════════════════════════════
#define PIN_SCK      6
#define PIN_MOSI     7
#define PIN_MISO     2
#define PIN_TFT_CS  18
#define PIN_TFT_DC  19
#define PIN_TFT_RST 20
#define PIN_TC_CS   14
#define PIN_POT      4
#define PIN_BUZZER   1

// ════════════════════════════════════════════════════════════════
//  CONSTANTES
// ════════════════════════════════════════════════════════════════
#define T_MIN      0
#define T_MAX   1300
#define ADC_MAX 3250
#define CX       120
#define CY       120
#define R_OUTER  104
#define R_INNER   90

#define MS_SENSOR  5000  //Segundos actualizacion sensor y otros
#define MS_POT       20
#define MS_DRAW     150
#define MS_PULSE    500

// ════════════════════════════════════════════════════════════════
//  PALETA RGB565
// ════════════════════════════════════════════════════════════════
#define C_BG        0x3921
#define C_MID       0xFFFF
#define C_DIM       0xbdc9
#define C_PARCHMENT 0xE6F7
#define C_ACCENT    0xde00
#define C_ACCENT2   0x8A00
#define C_FAINT     0x3921
#define C_WHITE     0xFFFF
#define C_NOTIF     0xce59

// ════════════════════════════════════════════════════════════════
//  OBJETOS
// ════════════════════════════════════════════════════════════════
Adafruit_GC9A01A tft(PIN_TFT_CS, PIN_TFT_DC,
                     PIN_MOSI, PIN_SCK,
                     PIN_TFT_RST, PIN_MISO);

Adafruit_MAX31856 maxthermo = Adafruit_MAX31856(
  PIN_TC_CS, PIN_MOSI, PIN_MISO, PIN_SCK
);

WiFiManager wm;
Preferences prefs;

// ════════════════════════════════════════════════════════════════
//  CREDENCIALES — se leen de flash (configuradas por portal)
// ════════════════════════════════════════════════════════════════
String mqttHost;
String mqttUser;
String mqttPass;
String tgToken;
String tgChatId;

#define MQTT_PORT   8883
#define MQTT_CLIENT "paramo-kiln-01"

// Topics
#define TOPIC_TC         "kiln/thermocouple"
#define TOPIC_CJ         "kiln/coldjunction"
#define TOPIC_FAULT      "kiln/fault"
#define TOPIC_STATUS     "kiln/status"
#define TOPIC_UPTIME     "kiln/uptime"
#define TOPIC_RSSI       "kiln/rssi"
#define TOPIC_ALERT      "kiln/alert"
#define TOPIC_THRESHOLD  "kiln/threshold"
#define TOPIC_THRESH_SET "kiln/threshold/set"
#define TOPIC_RESET      "kiln/reset"           // reset config desde web

WiFiClientSecure tlsClient;
PubSubClient     mqttClient(tlsClient);

// Timers MQTT
static unsigned long tMqtt   = 0;
static unsigned long tStatus = 0;
static bool prevFaultMqtt    = false;

// ════════════════════════════════════════════════════════════════
//  ESTADO
// ════════════════════════════════════════════════════════════════
static float currentTemp       = 0.0;
static float prevTemp          = 0.0;
static int   alertTarget       = 0;
static bool  alertFired        = false;
static bool  pulseOn           = false;
static bool  isFaultState      = false;
static int   lastAlertTarget   = -1;
static bool  webThreshold      = false;  // true = threshold viene de la web
static int   lastPotRaw        = -1;     // para detectar movimiento físico del pot

// Valores anteriores para detectar cambios
static float prevDisplayTemp   = -999.0;
static int   prevDisplayTarget = -999;
static bool  prevDisplayFired  = false;
static char  prevStatusStr[10] = "";
static float prevOuterProgress = -1.0;
static float prevInnerProgress = -1.0;

// Timers
static unsigned long tSensor = 0;
static unsigned long tPot    = 0;
static unsigned long tDraw   = 0;
static unsigned long tPulse  = 0;

// ── Callback MQTT — recibe mensajes entrantes ─────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[16];
  length = min(length, (unsigned int)15);
  memcpy(msg, payload, length);
  msg[length] = '\0';

  if (strcmp(topic, TOPIC_THRESH_SET) == 0) {
    int newVal = atoi(msg);
    newVal = constrain(newVal, T_MIN, T_MAX);
    alertTarget       = newVal;
    lastAlertTarget   = newVal;
    alertFired        = false;
    prevDisplayTarget = -999;
    webThreshold      = true;
    Serial.printf("[MQTT] Threshold desde web: %d°C\n", newVal);
  }

  // Reset de configuración desde la web — requiere "confirm" como payload
  if (strcmp(topic, TOPIC_RESET) == 0 && strcmp(msg, "confirm") == 0) {
    Serial.println("[RESET] Borrando configuración...");
    prefs.begin("kiln", false);
    prefs.clear();
    prefs.end();
    wm.resetSettings();
    tft.fillScreen(C_BG);
    textCenter("RESET", CX, CY - 10, 3, C_ACCENT);
    textCenter("Restarting...", CX, CY + 16, 1, C_DIM);
    delay(2000);
    ESP.restart();
  }
}

// ════════════════════════════════════════════════════════════════
//  BUZZER — simple y fiable
// ════════════════════════════════════════════════════════════════
void beep(int freq, int ms) {
  ledcAttach(PIN_BUZZER, freq, 8);
  ledcWrite(PIN_BUZZER, 128);
  delay(ms);
  ledcWrite(PIN_BUZZER, 0);
  ledcDetach(PIN_BUZZER);
}

void beepAlert() {
 beep(795, 120); delay(30);   // bip
  beep(795, 120); delay(150); // bip

  beep(500, 250); delay(250); //boop
}

void beepFault() {
  for (int i = 0; i < 5; i++) {
    beep(400, 80);
    delay(80);
  }
}

void beepBoot() {
 
  //Melodia retro On Boot
  
  beep(523, 180); delay(60);   // C5
  beep(659, 200); delay(80);   // E5
  beep(784, 260); delay(140);  // G5

  beep(659, 180); delay(60);   // E5
  beep(523, 220); delay(100);  // C5

  delay(180); // pequeña respiración

  beep(587, 200); delay(60);   // D5
  beep(698, 220); delay(80);   // F5
  beep(784, 300); delay(120);  // G5

  beep(659, 220); delay(80);   // E5
  beep(523, 400);              // C5 (cierre estable)
}

// ════════════════════════════════════════════════════════════════
//  POTENCIÓMETRO — saltos de 25° para filtrar ruido
// ════════════════════════════════════════════════════════════════
int readAlertTarget() {
  // Promedio de 4 lecturas para suavizar ruido ADC
  int raw = 0;
  for (int i = 0; i < 4; i++) { raw += analogRead(PIN_POT); delay(2); }
  raw /= 4;

  if (raw < 100) return 0;  // zona muerta CCW = OFF
  int t = map(raw, 0, ADC_MAX, T_MIN, T_MAX);
  return constrain((t / 50) * 50, T_MIN, T_MAX);  // snap a 50°
}

// ════════════════════════════════════════════════════════════════
//  HELPERS DE DIBUJO
// ════════════════════════════════════════════════════════════════

// Texto centrado — sobreescribe directamente sin fillRect
void textCenter(const char* str, int x, int y,
                uint8_t size, uint16_t col) {
  tft.setTextSize(size);
  tft.setTextColor(col, C_BG);  // fondo C_BG borra el texto anterior
  tft.setTextWrap(false);
  int16_t w = strlen(str) * 6 * size;
  tft.setCursor(x - w / 2, y - (4 * size));
  tft.print(str);
}

// Arco completo
void drawArcFull(float progress, int radius, int dotR, uint16_t col) {
  const int N = 72;
  int active  = (int)(progress * N);
  for (int i = 0; i < N; i++) {
    float angle = ((float)i / N) * TWO_PI - HALF_PI;
    int x = CX + (int)(radius * cos(angle));
    int y = CY + (int)(radius * sin(angle));
    tft.fillCircle(x, y, dotR, i < active ? col : C_BG);
  }
}

// Update parcial del arco
void updateArcPartial(float oldProg, float newProg,
                      int radius, int dotR, uint16_t col) {
  if (oldProg < 0) {
    drawArcFull(newProg, radius, dotR, col);
    return;
  }
  const int N   = 72;
  int oldActive = (int)(oldProg * N);
  int newActive = (int)(newProg * N);
  if (oldActive == newActive) return;

  int from = min(oldActive, newActive);
  int to   = max(oldActive, newActive);
  for (int i = from; i < to; i++) {
    float angle = ((float)i / N) * TWO_PI - HALF_PI;
    int x = CX + (int)(radius * cos(angle));
    int y = CY + (int)(radius * sin(angle));
    tft.fillCircle(x, y, dotR, i < newActive ? col : C_BG);
  }
}

void drawOuterDots() {
  const int N = 96;
  for (int i = 0; i < N; i++) {
    if (i % 6 != 0) continue;
    float angle = ((float)i / N) * TWO_PI;
    int x = CX + (int)(112 * cos(angle));
    int y = CY + (int)(112 * sin(angle));
    tft.drawPixel(x, y, C_FAINT);
  }
}

void drawGrid() {
  for (int deg = 0; deg < 360; deg += 30) {
    float rad = deg * DEG_TO_RAD;
    int x1 = CX + (int)(45 * cos(rad));
    int y1 = CY + (int)(45 * sin(rad));
    int x2 = CX + (int)(98 * cos(rad));
    int y2 = CY + (int)(98 * sin(rad));
    tft.drawLine(x1, y1, x2, y2, C_FAINT);
  }
}

void hline(int x1, int x2, int y, uint16_t col) {
  tft.drawLine(x1, y, x2, y, col);
}

// Status simplificado — solo 3 estados
const char* statusWord(float temp, float prev) {
  float d = temp - prev;
  if (d >  3.0) return "RISING";
  if (d < -3.0) return "COOLING";
  return "STABLE";
}

// ════════════════════════════════════════════════════════════════
//  FONDO ESTÁTICO — una sola vez en setup
// ════════════════════════════════════════════════════════════════
void drawBackground() {
  tft.fillScreen(C_BG);
  drawGrid();
  drawOuterDots();

  // Separador superior
  hline(CX - 52, CX + 52, CY - 46, C_MID);
  textCenter("CORE . SIGNAL", CX, CY - 52, 1, C_DIM);

  // Separador inferior
  hline(CX - 52, CX + 52, CY + 34, C_MID);

  // Label NOTIFY fijo — bien separado del status
  textCenter("NOTIFY", CX +5 , CY + 59, 1, C_NOTIF);
}

// ════════════════════════════════════════════════════════════════
//  UPDATES PARCIALES
// ════════════════════════════════════════════════════════════════

void updateTemp(float temp) {
  if (abs(temp - prevDisplayTemp) < 0.1f) return;

  // Borrar solo la franja del número — rápido, no toca arcos ni texto
  tft.fillRect(50, CY - 28, 155, 50, C_BG);

  char buf[10];
  snprintf(buf, sizeof(buf), "%.1f", temp);
  textCenter(buf, CX, CY + 4, 4, C_PARCHMENT);
  textCenter("o", CX + 50, CY - 14, 1, C_ACCENT);
  textCenter("C", CX + 56, CY - 8,  1, C_ACCENT);

  prevDisplayTemp = temp;
}

void updateStatus(float temp, float prev) {
  const char* sw = statusWord(temp, prev);
  if (strcmp(sw, prevStatusStr) == 0) return;

  char buf[12];
  snprintf(buf, sizeof(buf), "%-8s", sw);
  textCenter(buf, CX +9 , CY + 46, 1, C_DIM);
  strncpy(prevStatusStr, sw, sizeof(prevStatusStr));
}

void updateNotify(int target, bool fired) {
  if (target == prevDisplayTarget && fired == prevDisplayFired) return;

  bool showOff = (target == 0 || fired);

  char buf[18];
  if (showOff) {
    snprintf(buf, sizeof(buf), "%-14s", "[ OFF ]");
  } else {
    char val[12];
    snprintf(val, sizeof(val), "[ %d ]", target);
    snprintf(buf, sizeof(buf), "%-14s", val);
  }

  // Notify más abajo — separado del status
  textCenter(buf, CX + 25, CY + 68, 1,
             showOff ? C_DIM : C_PARCHMENT);

  prevDisplayTarget = target;
  prevDisplayFired  = fired;
}

void updateOuterArc(float temp) {
  float newProg = constrain(temp / (float)T_MAX, 0.0f, 1.0f);
  updateArcPartial(prevOuterProgress, newProg, R_OUTER, 2, C_ACCENT);
  prevOuterProgress = newProg;
}

void updateInnerArc(int target, bool fired) {
  if (!fired && target > 0) {
    float newProg = constrain((float)target / T_MAX, 0.0f, 1.0f);
    updateArcPartial(prevInnerProgress, newProg, R_INNER, 1, C_PARCHMENT);
    prevInnerProgress = newProg;
  } else if (prevInnerProgress > 0) {
    drawArcFull(0.0f, R_INNER, 1, C_BG);
    prevInnerProgress = 0.0f;
  }
}

// ════════════════════════════════════════════════════════════════
//  FAULT
// ════════════════════════════════════════════════════════════════
void drawFault(uint8_t faultCode) {
  tft.fillScreen(C_BG);
  drawOuterDots();
  tft.drawCircle(CX, CY, 82, C_MID);
  textCenter("FAULT", CX, CY - 18, 3, C_PARCHMENT);
  hline(CX - 44, CX + 44, CY - 2, C_MID);

  if      (faultCode & MAX31856_FAULT_OPEN)
    textCenter("OPEN CIRCUIT", CX, CY + 16, 1, C_DIM);
  else if (faultCode & MAX31856_FAULT_OVUV)
    textCenter("OVER/UNDER VOLT",  CX, CY + 16, 1, C_DIM);
  else if (faultCode & MAX31856_FAULT_TCRANGE)
    textCenter("THERMOCOUPLE RANGE", CX, CY + 16, 1, C_DIM);
  else if (faultCode & MAX31856_FAULT_CJRANGE)
    textCenter("COLD RANGE", CX, CY + 16, 1, C_DIM);
  else
    textCenter("SENSOR ERROR",     CX, CY + 16, 1, C_DIM);

  textCenter("CHECK WIRING", CX, CY + 36, 1, C_FAINT);
}




// ════════════════════════════════════════════════════════════════
//  HORA NTP
// ════════════════════════════════════════════════════════════════
String getTimeStr() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "??:??";
  char buf[20];
  strftime(buf, sizeof(buf), "%d/%m %H:%M:%S", &timeinfo);
  return String(buf);
}

// ════════════════════════════════════════════════════════════════
//  TELEGRAM
// ════════════════════════════════════════════════════════════════
void sendTelegram(String message) {
  if (tgToken.length() < 10) return;
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure tgClient;
  tgClient.setInsecure();
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + tgToken + "/sendMessage";
  http.begin(tgClient, url);
  http.addHeader("Content-Type", "application/json");
  String timestamped = "[" + getTimeStr() + "] " + message;
  timestamped.replace("\\", "\\\\");
  timestamped.replace("\"", "\\\"");
  timestamped.replace("\n", "\\n");
  String body = "{\"chat_id\":" + tgChatId +
                ",\"text\":\"" + timestamped + "\"}";
  int code = http.POST(body);
  Serial.printf("[TG] HTTP %d\n", code);
  if (code != 200) Serial.println("[TG] " + http.getString());
  http.end();
}

// ════════════════════════════════════════════════════════════════
//  MQTT
// ════════════════════════════════════════════════════════════════
void mqttConnect() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;
  if (mqttHost.length() < 5) return;
  Serial.print("[MQTT] Conectando...");

  tlsClient.setInsecure();
  tlsClient.setTimeout(15);
  mqttClient.setServer(mqttHost.c_str(), MQTT_PORT);
  mqttClient.setKeepAlive(60);
  mqttClient.setSocketTimeout(15);
  mqttClient.setCallback(mqttCallback);

  delay(200);

  bool ok = mqttClient.connect(MQTT_CLIENT,
                               mqttUser.c_str(), mqttPass.c_str(),
                               TOPIC_STATUS, 1, true, "offline");
  if (ok) {
    Serial.println(" OK");
    mqttClient.publish(TOPIC_STATUS, "online", true);
    mqttClient.subscribe(TOPIC_THRESH_SET);
    mqttClient.subscribe(TOPIC_RESET);
    Serial.println("[MQTT] Suscrito a threshold/set y reset");
  } else {
    Serial.printf(" FAIL rc=%d\n", mqttClient.state());
  }
}

void mqttPublish(float tc, float cj) {
  if (!mqttClient.connected()) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%.2f", tc);
  mqttClient.publish(TOPIC_TC, buf);
  snprintf(buf, sizeof(buf), "%.2f", cj);
  mqttClient.publish(TOPIC_CJ, buf);
  // Publicar threshold actual
  snprintf(buf, sizeof(buf), "%d", alertTarget);
  mqttClient.publish(TOPIC_THRESHOLD, buf, true);  // retained
}

void mqttPublishStatus() {
  if (!mqttClient.connected()) return;
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", millis() / 1000);
  mqttClient.publish(TOPIC_UPTIME, buf);
  snprintf(buf, sizeof(buf), "%d", WiFi.RSSI());
  mqttClient.publish(TOPIC_RSSI, buf);
  // Publicar status online como retained — así el dashboard lo ve al conectar
  mqttClient.publish(TOPIC_STATUS, "online", true);
}

void mqttPublishFault(const char* msg) {
  if (!mqttClient.connected()) return;
  mqttClient.publish(TOPIC_FAULT, msg, true);
}

void mqttPublishAlert(String msg) {
  if (!mqttClient.connected()) return;
  char buf[128];
  msg.toCharArray(buf, sizeof(buf));
  mqttClient.publish(TOPIC_ALERT, buf);
}

// ════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════



void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== KILN CTRL · ESP32-C6 ===");


  tft.begin();
  tft.setRotation(1);  // 0=normal 1=90° 2=180° 3=270°

  tft.fillScreen(C_BG);
  textCenter("PARAMO",            CX, CY - 10, 4, C_ACCENT);
  textCenter("by Victor Moreno", CX, CY + 16, 2, C_DIM);
  delay(5000);
  beepAlert();

  // WIFI MANAGER — todo configurado ANTES del autoConnect

  bool wifiOK;
  WiFi.mode(WIFI_STA);

  // 1 — Leer credenciales guardadas en flash
  prefs.begin("kiln", true);
  mqttHost = prefs.getString("mqtt_host", "");
  mqttUser = prefs.getString("mqtt_user", "");
  mqttPass = prefs.getString("mqtt_pass", "");
  tgToken  = prefs.getString("tg_token",  "");
  tgChatId = prefs.getString("tg_chat",   "");
  prefs.end();

  // 2 — Parámetros extra del portal
  WiFiManagerParameter p_host("host",    "HiveMQ Host",      mqttHost.c_str(), 80);
  WiFiManagerParameter p_user("user",    "HiveMQ Username",  mqttUser.c_str(), 40);
  WiFiManagerParameter p_pass("pass",    "HiveMQ Password",  mqttPass.c_str(), 40);
  WiFiManagerParameter p_tgt ("tgtoken", "Telegram Token",   tgToken.c_str(),  60);
  WiFiManagerParameter p_tgc ("tgchat",  "Telegram Chat ID", tgChatId.c_str(), 20);

  wm.addParameter(&p_host);
  wm.addParameter(&p_user);
  wm.addParameter(&p_pass);
  wm.addParameter(&p_tgt);
  wm.addParameter(&p_tgc);

  // 3 — CSS custom portal oscuro y elegante
  wm.setCustomHeadElement(
    "<style>"
    "body{background:#0a0c0f;color:#c9d1e0;font-family:'Courier New',monospace;"
    "max-width:460px;margin:0 auto;padding:0 16px;}"
    "h1{color:#e8622a;font-size:1.1rem;letter-spacing:0.15em;text-align:center;margin:20px 0 4px;}"
    "h2{color:#4a5270;font-size:0.7rem;letter-spacing:0.2em;text-transform:uppercase;"
    "margin:20px 0 10px;padding-bottom:6px;border-bottom:1px solid #1e2230;}"
    "label{font-size:0.72rem;color:#4a5270;display:block;margin-bottom:3px;}"
    "input[type=text],input[type=password]{"
    "background:#111318!important;color:#c9d1e0!important;width:100%!important;"
    "border:1px solid #2a2f42!important;border-radius:4px!important;"
    "padding:8px 10px!important;font-family:'Courier New'!important;"
    "font-size:0.82rem!important;box-sizing:border-box!important;margin-bottom:10px!important;}"
    "input[type=text]:focus,input[type=password]:focus{"
    "border-color:#e8622a!important;outline:none!important;}"
    "input[type=submit],button{"
    "background:#e8622a!important;color:#fff!important;width:100%!important;"
    "border:none!important;border-radius:4px!important;padding:10px!important;"
    "font-size:0.85rem!important;font-weight:700!important;letter-spacing:0.1em!important;"
    "cursor:pointer!important;margin-top:4px!important;}"
    "input[type=submit]:hover{background:#d45520!important;}"
    ".msg{background:#111318;border:1px solid #2a2f42;border-radius:4px;"
    "padding:12px 14px;margin:10px 0;font-size:0.78rem;line-height:1.6;}"
    ".info{color:#4a5270;font-size:0.68rem;line-height:1.7;margin:8px 0 14px;}"
    "a{color:#e8622a;text-decoration:none;}"
    "</style>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
  );

  wm.setCustomMenuHTML(
    "<div style='text-align:center;padding:24px 0 4px;'>"
    "<div style='font-size:1.8rem;'>🔥</div>"
    "<div style='font-size:1.2rem;font-weight:700;color:#e8622a;"
    "letter-spacing:0.25em;margin-top:6px;'>PARAMO</div>"
    "<div style='font-size:0.68rem;color:#4a5270;margin-top:4px;"
    "letter-spacing:0.15em;'>KILN MONITOR · SETUP</div>"
    "</div>"
    "<div class='info' style='margin-top:16px;'>"
    "Fill in your HiveMQ and Telegram credentials below.<br>"
    "These are saved on the device and only needed once."
    "</div>"
  );

  // 4 — Callback para guardar en flash cuando el usuario configura
  wm.setSaveConfigCallback([&]() {
    prefs.begin("kiln", false);
    prefs.putString("mqtt_host", p_host.getValue());
    prefs.putString("mqtt_user", p_user.getValue());
    prefs.putString("mqtt_pass", p_pass.getValue());
    prefs.putString("tg_token",  p_tgt.getValue());
    prefs.putString("tg_chat",   p_tgc.getValue());
    prefs.end();
    mqttHost = p_host.getValue();
    mqttUser = p_user.getValue();
    mqttPass = p_pass.getValue();
    tgToken  = p_tgt.getValue();
    tgChatId = p_tgc.getValue();
    Serial.println("[CONFIG] Credenciales guardadas en flash");
  });

  wm.setConnectTimeout(10);

  // 5 — Pantalla y autoConnect (lo ultimo)
  tft.fillScreen(C_BG);
  textCenter("WIFI CONFIG", CX, CY - 8, 2, C_ACCENT);
  textCenter("Searching networks...", CX, CY + 16, 1, C_DIM);

  wifiOK = wm.autoConnect("KILN-SETUP");

  if (!wifiOK) {
    tft.fillScreen(C_BG);
    textCenter("WIFI FAILED",    CX, CY - 10, 2, C_ACCENT);
    textCenter("RESTART DEVICE", CX, CY + 16, 1, C_DIM);
    beepFault();
    while (true) delay(1000);
  } else {
    tft.fillScreen(C_BG);
    textCenter("KILN",            CX, CY - 10, 4, C_ACCENT);
    textCenter("INITIALIZING...", CX, CY + 16, 2, C_DIM);
    delay(800);
    beepBoot();
  }

  // ── NTP — sincronizar hora ────────────────────────────────────
  configTime(3600, 3600, "pool.ntp.org", "time.google.com");
  Serial.print("[NTP] Sincronizando hora...");
  struct tm timeinfo;
  bool ntpOk = false;
  for (int i = 0; i < 3 && !ntpOk; i++) {
    if (getLocalTime(&timeinfo, 4000)) {
      ntpOk = true;
    } else {
      Serial.print(".");
      delay(1000);
    }
  }
  if (ntpOk) {
    Serial.printf(" OK — %s\n", getTimeStr().c_str());
  } else {
    Serial.println(" sin respuesta, continuando sin hora");
  }

  // ── MQTT ──────────────────────────────────────────────────────
  // Si no hay credenciales MQTT configuradas, forzar portal
  if (mqttHost.length() < 5) {
    tft.fillScreen(C_BG);
    textCenter("SETUP NEEDED", CX, CY - 18, 1, C_ACCENT);
    textCenter("Connect to:", CX, CY - 2, 1, C_DIM);
    textCenter("KILN-SETUP", CX, CY + 14, 2, C_PARCHMENT);
    textCenter("192.168.4.1", CX, CY + 36, 1, C_DIM);
    Serial.println("[CONFIG] Sin credenciales MQTT — abriendo portal");
    wm.startConfigPortal("KILN-SETUP");
  }

  tft.fillScreen(C_BG);
  textCenter("SERVER MQTT", CX, CY - 8, 2, C_ACCENT);
  textCenter("Connecting...", CX, CY + 16, 1, C_DIM);
  mqttConnect();
  if (mqttClient.connected()) {
    tft.fillScreen(C_BG);
    textCenter("MQTT OK", CX, CY - 8, 2, C_ACCENT);
    String ip = WiFi.localIP().toString();
    textCenter(ip.c_str(), CX, CY + 10, 1, C_DIM);
    Serial.printf("[WiFi] IP: %s\n", ip.c_str());
    delay(1200);
  } else {
    tft.fillScreen(C_BG);
    textCenter("MQTT OFFLINE", CX, CY - 8, 1, C_DIM);
    textCenter("Continuing...", CX, CY + 10, 1, C_DIM);
    delay(800);
  }

  if (!maxthermo.begin()) {
    Serial.println("ERROR: MAX31856 no detectado");
    tft.fillScreen(C_BG);
    textCenter("MAX31856",     CX, CY - 10, 2, C_ACCENT);
    textCenter("NO DETECTADO", CX, CY + 14, 1, C_DIM);
    while (1) delay(1000);
  }
  maxthermo.setThermocoupleType(MAX31856_TCTYPE_K); //Define l tipo de de Sonda, S, B, K,...
  Serial.println("MAX31856 OK · Tipo S");


  // Fondo estático
  drawBackground();

  // Estado inicial
  currentTemp = maxthermo.readThermocoupleTemperature();
  prevTemp    = currentTemp;
  alertTarget = readAlertTarget();

  updateTemp(currentTemp);
  updateStatus(currentTemp, prevTemp);
  updateNotify(alertTarget, alertFired);
  updateOuterArc(currentTemp);
  updateInnerArc(alertTarget, alertFired);

  Serial.println("Sistema listo");
}

// ════════════════════════════════════════════════════════════════
//  LOOP — sin delay(), todo millis()
// ════════════════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  // ── MQTT keep-alive ──────────────────────────────────────────
  if (mqttClient.connected()) {
    mqttClient.loop();
  } else if (now - tMqtt > 30000) {
    mqttConnect();
    tMqtt = now;
  }

  // ── Pulso cada 500ms ─────────────────────────────────────────
  if (now - tPulse > MS_PULSE) {
    pulseOn = !pulseOn;
    tPulse  = now;
  }

  // ── Pot cada 20ms ────────────────────────────────────────────
  if (now - tPot > MS_POT) {
    int rawPot = analogRead(PIN_POT);

    // Si el pot se mueve físicamente más de 120 cuentas → toma el control
    if (abs(rawPot - lastPotRaw) > 120) {
      webThreshold  = false;
      lastPotRaw    = rawPot;
    }

    if (!webThreshold) {
      // Pot manda — solo actualiza si el cambio es >= 50° (histéresis)
      int newTarget = readAlertTarget();
      if (abs(newTarget - alertTarget) >= 50 || newTarget == 0) {
        if (newTarget != lastAlertTarget) {
          alertFired      = false;
          lastAlertTarget = newTarget;
        }
        alertTarget = newTarget;
      }
    }

    // Inicializar lastPotRaw en primer ciclo
    if (lastPotRaw < 0) lastPotRaw = rawPot;

    tPot = now;
  }

  // ── Sensor cada 1000ms ───────────────────────────────────────
  if (now - tSensor > MS_SENSOR) {
    uint8_t fault = maxthermo.readFault();

    // Fault — publicar a MQTT
    if (fault != 0) {
      if (!isFaultState) {
        isFaultState = true;
        beepFault();
        mqttPublishFault("FAULT detected");
        mqttPublishAlert("Sensor fault: " + String(fault));
        sendTelegram("⚠️ PARAMO FAULT · código " + String(fault));
      }
      drawFault(fault);
      tSensor = now;
      return;
    }

    // Recuperación de fault
    if (isFaultState) {
      isFaultState      = false;
      prevDisplayTemp   = -999;
      prevDisplayTarget = -999;
      prevOuterProgress = -1;
      prevInnerProgress = -1;
      prevStatusStr[0]  = '\0';
      mqttPublishFault("none");
      sendTelegram("✅ PARAMO · sensor OK");
      drawBackground();
    }

    prevTemp    = currentTemp;
    currentTemp = maxthermo.readThermocoupleTemperature();
    float cjTemp = maxthermo.readCJTemperature();

    // Publicar a MQTT
    mqttPublish(currentTemp, cjTemp);

    // Disparar alerta
    if (alertTarget > 0 && !alertFired && currentTemp >= alertTarget) {
      alertFired = true;
      beepAlert();
      prevDisplayTarget = -999;

      // Obtener fecha formateada
      struct tm timeinfo;
      char dateBuf[24];
      if (getLocalTime(&timeinfo)) {
        strftime(dateBuf, sizeof(dateBuf), "%H:%M / %d/%m/%Y", &timeinfo);
      } else {
        strcpy(dateBuf, "??:?? / ??/??/????");
      }

      String alertMsg =
        "Threshold Crossed!\n"
        "🎯 " + String(currentTemp, 1) + "°C >= " + String(alertTarget) + "°C\n"
        "📅 " + String(dateBuf);

      mqttPublishAlert(alertMsg);
      sendTelegram(alertMsg);
      Serial.printf("ALERTA: %.1fC >= %dC\n", currentTemp, alertTarget);
    }

    Serial.printf("K: %.1fC CJ: %.1fC | Tgt:%dC | MQTT:%s\n",
      currentTemp, cjTemp, alertTarget,
      mqttClient.connected() ? "OK" : "OFF");

    tSensor = now;
  }

  // ── Status MQTT cada 10s ─────────────────────────────────────
  if (now - tStatus > 10000) {
    mqttPublishStatus();
    tStatus = now;
  }

  // ── Pantalla cada 150ms ──────────────────────────────────────
  if (now - tDraw > MS_DRAW) {
    updateOuterArc(currentTemp);
    updateInnerArc(alertTarget, alertFired);
    updateTemp(currentTemp);
    updateStatus(currentTemp, prevTemp);
    updateNotify(alertTarget, alertFired);
    tDraw = now;
  }
}
