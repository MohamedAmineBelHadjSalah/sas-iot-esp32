#include <WiFi.h>
#include <PubSubClient.h>

#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <MFRC522.h>
#include <DHT.h>
#include <ArduinoJson.h>

/* ================= WIFI ================= */
const char* ssid     = "Xiaomi";
const char* password = "72222388f";

/* ================= MQTT ================= */
const char* mqtt_server = "test.mosquitto.org";
const int   mqtt_port   = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

/* ================= TOPICS ================= */
#define T_DOOR1     "sas/door1"
#define T_DOOR2     "sas/door2"
#define T_STATE     "sas/state"
#define T_TEMP      "sas/temp"
#define T_HUM       "sas/hum"
#define T_ACCESS    "sas/access"
#define T_COUNTDOWN "sas/countdown"
#define T_FIRE      "sas/fire"     // incendie 1/0 (retained)
#define T_CARDS     "sas/cards"    // DB badges (retained conseillé)

#define T_D1_OPEN_NOBADGE "sas/stats/door1/open_nobadge"
#define T_D1_OPEN_BADGE   "sas/stats/door1/open_badge"
#define T_D2_OPEN_NOBADGE "sas/stats/door2/open_nobadge"
#define T_D2_OPEN_BADGE   "sas/stats/door2/open_badge"

#define T_CTRL_D1   "sas/control/door1"
#define T_CTRL_D2   "sas/control/door2"
#define T_CTRL_ALL  "sas/control/all"

/* ================= PINOUT ================= */
// Servos
#define SERVO_P1 4
#define SERVO_P2 14

// Buttons
#define BTN_P1 33
#define BTN_P2 27

// LEDs
#define RED_P1   2
#define GREEN_P1 12
#define RED_P2   15
#define GREEN_P2 5

// Buzzer
#define BUZZER_PIN 32

// LCD I2C
#define LCD_SDA 21
#define LCD_SCL 22

// DHT11
#define DHTPIN  17
#define DHTTYPE DHT11

// RFID RC522 (SPI)
#define RFID_SCK   18
#define RFID_MISO  19
#define RFID_MOSI  23
#define RFID_SS    25
#define RFID_RST   26

// Servo angles
#define OPEN_ANGLE  90
#define CLOSE_ANGLE 0

/* ================= OBJECTS ================= */
Servo servoP1;
Servo servoP2;
LiquidCrystal_I2C lcd(0x27, 20, 4);
DHT dht(DHTPIN, DHTTYPE);
MFRC522 rfid(RFID_SS, RFID_RST);

/* ================= STATE MACHINE ================= */
enum State { INIT, P1_OPEN, P2_OPEN, WAIT_AFTER_CLOSE, FIRE };
State currentState = INIT;

/* ================= TIMERS ================= */
const unsigned long WAIT_TIME       = 30000; // 30s
const unsigned long DOOR_OPEN_LIMIT = 6000;  // 6s

unsigned long waitTimerStart  = 0;
unsigned long lastSecondTick  = 0;
unsigned long doorOpenStart   = 0;
bool doorAlarmTriggered       = false;

unsigned long lastDhtRead     = 0;
float lastTemp                = NAN;
float lastHum                 = NAN;

unsigned long lastRFIDTrigger = 0;
const unsigned long RFID_COOLDOWN = 1500;

unsigned long lastMqttPublish = 0;

/* ================= INCENDIE ================= */
const float FIRE_ON  = 22.0; // >22 => ON
const float FIRE_OFF = 24.0; // <=24 => OFF (hystérésis)
bool fireActive = false;

// buzzer incendie non-bloquant
unsigned long fireBuzzLast = 0;
bool fireBuzzState = false;

/* ================= COUNTERS ================= */
enum OpenSource { SRC_BUTTON, SRC_WEB, SRC_RFID };

uint32_t d1_open_nobadge = 0;
uint32_t d1_open_badge   = 0;
uint32_t d2_open_nobadge = 0;
uint32_t d2_open_badge   = 0;

/* ================= RFID DB (Node-RED) ================= */
struct DbCard {
  uint8_t uid[4];
  char name[20];
  bool enabled;
  bool master;  // BOSS
  bool used;
};

static const int MAX_DB_CARDS = 30;
DbCard dbCards[MAX_DB_CARDS];

bool cardDetected = false;

/* ================= RFID ACTION ================= */
enum RfidAction { RFID_NONE, RFID_OPEN_NORMAL, RFID_OPEN_MASTER };

/* ================= UTILS ================= */
bool buttonPressed(int pin) {
  static unsigned long lastPress[40];
  if (digitalRead(pin) == LOW && millis() - lastPress[pin] > 300) {
    lastPress[pin] = millis();
    return true;
  }
  return false;
}

void buzz(int durationMs) { // buzzer "normal" (bloquant mais court)
  digitalWrite(BUZZER_PIN, HIGH);
  delay(durationMs);
  digitalWrite(BUZZER_PIN, LOW);
}

void showMessage(const String& msg) {
  Serial.println(msg);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(msg);
}

/* ================= DOORS + LEDS ================= */
void closeBothDoors() {
  servoP1.write(CLOSE_ANGLE);
  servoP2.write(CLOSE_ANGLE);
}

void openBothDoors() {
  servoP1.write(OPEN_ANGLE);
  servoP2.write(OPEN_ANGLE);
}

// INIT => accès possible : 2 verts ON
void ledsAccessOK() {
  digitalWrite(RED_P1, LOW);    digitalWrite(GREEN_P1, HIGH);
  digitalWrite(RED_P2, LOW);    digitalWrite(GREEN_P2, HIGH);
}

// WAIT => 2 rouges ON
void ledsWait() {
  digitalWrite(RED_P1, HIGH);   digitalWrite(GREEN_P1, LOW);
  digitalWrite(RED_P2, HIGH);   digitalWrite(GREEN_P2, LOW);
}

// FIRE => 2 rouges ON
void ledsFire() {
  digitalWrite(RED_P1, HIGH);   digitalWrite(GREEN_P1, LOW);
  digitalWrite(RED_P2, HIGH);   digitalWrite(GREEN_P2, LOW);
}

void openP1(OpenSource src) {
  if (fireActive) return;

  servoP1.write(OPEN_ANGLE);
  servoP2.write(CLOSE_ANGLE);

  digitalWrite(RED_P1, LOW);
  digitalWrite(GREEN_P1, HIGH);
  digitalWrite(RED_P2, HIGH);
  digitalWrite(GREEN_P2, LOW);

  currentState = P1_OPEN;
  doorOpenStart = millis();
  doorAlarmTriggered = false;

  if (src == SRC_RFID) d1_open_badge++;
  else d1_open_nobadge++;
}

void openP2(OpenSource src) {
  if (fireActive) return;

  servoP2.write(OPEN_ANGLE);
  servoP1.write(CLOSE_ANGLE);

  digitalWrite(RED_P2, LOW);
  digitalWrite(GREEN_P2, HIGH);
  digitalWrite(RED_P1, HIGH);
  digitalWrite(GREEN_P1, LOW);

  currentState = P2_OPEN;
  doorOpenStart = millis();
  doorAlarmTriggered = false;

  if (src == SRC_RFID) d2_open_badge++;
  else d2_open_nobadge++;
}

void countdownLCD() {
  if (millis() - lastSecondTick >= 1000) {
    lastSecondTick += 1000;
    int remaining = (int)((WAIT_TIME - (millis() - waitTimerStart)) / 1000);
    if (remaining < 0) remaining = 0;
    lcd.setCursor(0, 1);
    lcd.print(("Attente: " + String(remaining) + " s     ").c_str());
  }
}

/* ================= LCD: WiFi/MQTT ================= */
void updateNetLCD() {
  bool w = (WiFi.status() == WL_CONNECTED);
  bool m = client.connected();

  lcd.setCursor(0, 3);
  lcd.print("WiFi:");
  lcd.print(w ? "OK" : "  ");
  lcd.print("  MQTT:");
  lcd.print(m ? "OK" : "  ");
  lcd.print("     ");
}

/* ================= RFID HELPERS ================= */
String uidToStringBytes(const byte* u, byte n) {
  String s;
  for (byte i = 0; i < n; i++) {
    if (u[i] < 0x10) s += "0";
    s += String(u[i], HEX);
    if (i < n - 1) s += " ";
  }
  s.toUpperCase();
  return s;
}

String uidToString() {
  return uidToStringBytes(rfid.uid.uidByte, rfid.uid.size);
}

bool mqttReady() {
  return (WiFi.status() == WL_CONNECTED) && client.connected();
}

void publishAccessEvent(const char* event, const char* nameOrNull) {
  if (!mqttReady()) return;

  String uidStr = uidToString();
  String nameStr = (nameOrNull && String(nameOrNull).length()) ? String(nameOrNull) : "Unknown";

  String json = "{";
  json += "\"event\":\"" + String(event) + "\",";
  json += "\"name\":\"" + nameStr + "\",";
  json += "\"uid\":\"" + uidStr + "\"";
  json += "}";

  client.publish(T_ACCESS, json.c_str(), false);
}

// Parse "8F AB 8D C2" -> bytes[4]
bool parseUidString(const char* s, uint8_t out[4]) {
  if (!s) return false;
  int vals[4] = {0, 0, 0, 0};
  int count = 0;

  const char* p = s;
  while (*p && count < 4) {
    while (*p == ' ') p++;
    if (!*p) break;
    char a = *p++;
    if (!*p) return false;
    char b = *p++;
    char tmp[3] = {a, b, 0};
    vals[count++] = (int)strtol(tmp, nullptr, 16);
    while (*p && *p != ' ') p++;
  }

  if (count != 4) return false;
  for (int i = 0; i < 4; i++) out[i] = (uint8_t)vals[i];
  return true;
}

bool uidEqual4(const byte* a, const uint8_t* b) {
  for (int i = 0; i < 4; i++) if (a[i] != b[i]) return false;
  return true;
}

const DbCard* findDbCardByUid(const byte* uid4) {
  for (int i = 0; i < MAX_DB_CARDS; i++) {
    if (dbCards[i].used && uidEqual4(uid4, dbCards[i].uid)) return &dbCards[i];
  }
  return nullptr;
}

void loadCardsFromJson(const char* json) {
  for (int i = 0; i < MAX_DB_CARDS; i++) dbCards[i].used = false;

  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.print("JSON cards parse error: ");
    Serial.println(err.c_str());
    return;
  }

  JsonArray arr = doc.as<JsonArray>();
  int idx = 0;

  for (JsonObject o : arr) {
    if (idx >= MAX_DB_CARDS) break;

    const char* uidStr = o["uid"] | "";
    const char* name   = o["name"] | "Unknown";
    bool enabled        = o["enabled"] | false;
    bool master         = o["master"]  | false;

    uint8_t u4[4];
    if (!parseUidString(uidStr, u4)) continue;

    dbCards[idx].uid[0] = u4[0];
    dbCards[idx].uid[1] = u4[1];
    dbCards[idx].uid[2] = u4[2];
    dbCards[idx].uid[3] = u4[3];

    strncpy(dbCards[idx].name, name, sizeof(dbCards[idx].name) - 1);
    dbCards[idx].name[sizeof(dbCards[idx].name) - 1] = 0;

    dbCards[idx].enabled = enabled;
    dbCards[idx].master  = master;
    dbCards[idx].used    = true;

    idx++;
  }

  Serial.print("Cards DB loaded: ");
  Serial.println(idx);
}

/* ================= INCENDIE ================= */
void publishFireFlag() {
  if (!mqttReady()) return;
  client.publish(T_FIRE, fireActive ? "1" : "0", true);
}

void enterFireMode() {
  fireActive = true;
  currentState = FIRE;

  openBothDoors();
  ledsFire();

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("!!! INCENDIE !!!");
  lcd.setCursor(0, 1); lcd.print("PORTES OUVERTES");

  fireBuzzLast = 0;
  fireBuzzState = false;

  if (mqttReady()) {
    client.publish(T_STATE, "FIRE", true);
    publishFireFlag();
    client.publish(T_DOOR1, "OPEN", true);
    client.publish(T_DOOR2, "OPEN", true);
    client.publish(T_COUNTDOWN, "0", true);
  }
}

void exitFireMode() {
  fireActive = false;
  digitalWrite(BUZZER_PIN, LOW);

  currentState = INIT;
  closeBothDoors();
  ledsAccessOK();
  showMessage("SAS MSD");

  if (mqttReady()) {
    publishFireFlag();
    client.publish(T_STATE, "INIT", true);
  }
}

void fireBuzzerTick() {
  if (!fireActive) return;
  const unsigned long period = 350;
  if (millis() - fireBuzzLast >= period) {
    fireBuzzLast = millis();
    fireBuzzState = !fireBuzzState;
    digitalWrite(BUZZER_PIN, fireBuzzState ? HIGH : LOW);
  }
}

/* ================= RFID (NORMAL / MASTER) ================= */
RfidAction handleRfid() {
  cardDetected = false;

  if (!rfid.PICC_IsNewCardPresent()) return RFID_NONE;
  if (!rfid.PICC_ReadCardSerial())   return RFID_NONE;

  cardDetected = true;

  if (fireActive) {
    showMessage("INCENDIE: OUVERT");
    rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
    return RFID_NONE;
  }

  if (rfid.uid.size != 4) {
    publishAccessEvent("RFID_REFUSE", "Unknown");
    showMessage("BADGE INVALIDE");
    buzz(300);
    rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
    return RFID_NONE;
  }

  const DbCard* c = findDbCardByUid(rfid.uid.uidByte);
  bool known   = (c != nullptr);
  bool enabled = known ? c->enabled : false;
  bool master  = (known && enabled) ? c->master : false;
  const char* name = known ? c->name : "Unknown";

  Serial.print("UID lu: ");
  Serial.println(uidToString());

  // inconnu / désactivé
  if (!known || !enabled) {
    publishAccessEvent("RFID_REFUSE", name);
    showMessage("BADGE REFUSE");
    buzz(500);
    rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
    return RFID_NONE;
  }

  // MASTER : ouvre à tout moment
  if (master) {
    publishAccessEvent("RFID_MASTER", name);
    showMessage(String("BOSS: ") + name);
    buzz(800);
    rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
    return RFID_OPEN_MASTER;
  }

  // normal : si pas INIT -> WAIT
  if (currentState != INIT) {
    publishAccessEvent("RFID_WAIT", name);
    showMessage("ATTENTE... SAS");
    buzz(500);
    rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
    return RFID_NONE;
  }

  // normal OK
  publishAccessEvent("RFID_OK", name);
  rfid.PICC_HaltA(); rfid.PCD_StopCrypto1();
  return RFID_OPEN_NORMAL;
}

/* ================= WIFI ================= */
bool connectWiFi(unsigned long timeoutMs = 8000) {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("WiFi: connexion ");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeoutMs) {
    delay(200);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi OK");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("\nWiFi OFFLINE");
    return false;
  }
}

/* ================= MQTT HELPERS ================= */
const char* stateToText() {
  switch (currentState) {
    case INIT: return "INIT";
    case P1_OPEN: return "P1_OPEN";
    case P2_OPEN: return "P2_OPEN";
    case WAIT_AFTER_CLOSE: return "WAIT_AFTER_CLOSE";
    case FIRE: return "FIRE";
  }
  return "UNK";
}

void computeDoorStates(const char*& d1, const char*& d2) {
  if (fireActive) { d1 = "OPEN"; d2 = "OPEN"; return; }

  if (currentState == INIT) {
    d1 = "CLOSED"; d2 = "CLOSED";
  } else if (currentState == P1_OPEN) {
    d1 = "OPEN"; d2 = "BLOCKED";
  } else if (currentState == P2_OPEN) {
    d1 = "BLOCKED"; d2 = "OPEN";
  } else {
    d1 = "BLOCKED"; d2 = "BLOCKED";
  }
}

void mqttPublishAll() {
  if (!mqttReady()) return;

  const char* d1; const char* d2;
  computeDoorStates(d1, d2);

  client.publish(T_DOOR1, d1, true);
  client.publish(T_DOOR2, d2, true);
  client.publish(T_STATE, stateToText(), true);

  publishFireFlag();

  if (!fireActive && currentState == WAIT_AFTER_CLOSE) {
    int remaining = (int)((WAIT_TIME - (millis() - waitTimerStart)) / 1000);
    if (remaining < 0) remaining = 0;
    char cbuf[12];
    itoa(remaining, cbuf, 10);
    client.publish(T_COUNTDOWN, cbuf, true);
  } else {
    client.publish(T_COUNTDOWN, "0", true);
  }

  if (!isnan(lastTemp)) {
    char buf[16];
    dtostrf(lastTemp, 0, 1, buf);
    client.publish(T_TEMP, buf, true);
  }
  if (!isnan(lastHum)) {
    char buf[16];
    dtostrf(lastHum, 0, 1, buf);
    client.publish(T_HUM, buf, true);
  }

  char b[16];
  sprintf(b, "%lu", (unsigned long)d1_open_nobadge); client.publish(T_D1_OPEN_NOBADGE, b, true);
  sprintf(b, "%lu", (unsigned long)d1_open_badge);   client.publish(T_D1_OPEN_BADGE, b, true);
  sprintf(b, "%lu", (unsigned long)d2_open_nobadge); client.publish(T_D2_OPEN_NOBADGE, b, true);
  sprintf(b, "%lu", (unsigned long)d2_open_badge);   client.publish(T_D2_OPEN_BADGE, b, true);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  String t = String(topic);

  if (t == T_CARDS) { loadCardsFromJson(msg.c_str()); return; }

  // si incendie actif => ignore commandes
  if (fireActive) return;

  if (t == T_CTRL_D1) {
    if (msg == "OPEN") {
      if (currentState == INIT) { openP1(SRC_WEB); showMessage("WEB -> P1 OUV"); }
      else { buzz(500); showMessage("WEB P1 BLOQ"); }
    } else if (msg == "CLOSE") {
      if (currentState == P1_OPEN) {
        closeBothDoors();
        ledsWait();
        showMessage("WEB -> P1 FERM");
        waitTimerStart = millis();
        lastSecondTick = millis();
        currentState = WAIT_AFTER_CLOSE;
      } else { buzz(500); showMessage("WEB CLOSE REF"); }
    }
  }

  if (t == T_CTRL_D2) {
    if (msg == "OPEN") {
      if (currentState == INIT) { openP2(SRC_WEB); showMessage("WEB -> P2 OUV"); }
      else { buzz(500); showMessage("WEB P2 BLOQ"); }
    } else if (msg == "CLOSE") {
      if (currentState == P2_OPEN) {
        closeBothDoors();
        ledsWait();
        showMessage("WEB -> P2 FERM");
        waitTimerStart = millis();
        lastSecondTick = millis();
        currentState = WAIT_AFTER_CLOSE;
      } else { buzz(500); showMessage("WEB CLOSE REF"); }
    }
  }

  if (t == T_CTRL_ALL && msg == "LOCK") {
    closeBothDoors();
    ledsAccessOK();
    currentState = INIT;
    showMessage("WEB -> LOCK ALL");
  }

  mqttPublishAll();
}

bool mqttTryConnectOnce() {
  if (client.connected()) return true;
  if (WiFi.status() != WL_CONNECTED) return false;

  String cid = "SAS_ESP32_" + String((uint32_t)ESP.getEfuseMac(), HEX) + "_" + String((uint32_t)esp_random(), HEX);
  Serial.print("MQTT: tentative... ");
  bool ok = client.connect(cid.c_str());
  Serial.println(ok ? "OK" : "FAIL");

  if (ok) {
    client.subscribe(T_CTRL_D1);
    client.subscribe(T_CTRL_D2);
    client.subscribe(T_CTRL_ALL);
    client.subscribe(T_CARDS);

    client.publish(T_STATE, "BOOT", true);
    publishFireFlag();
    mqttPublishAll();
  }
  return ok;
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);

  pinMode(BTN_P1, INPUT_PULLUP);
  pinMode(BTN_P2, INPUT_PULLUP);

  pinMode(RED_P1, OUTPUT);
  pinMode(GREEN_P1, OUTPUT);
  pinMode(RED_P2, OUTPUT);
  pinMode(GREEN_P2, OUTPUT);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  servoP1.attach(SERVO_P1);
  servoP2.attach(SERVO_P2);

  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("SAS MSD");

  dht.begin();

  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_SS);
  rfid.PCD_Init();

  // INIT
  closeBothDoors();
  ledsAccessOK();

  // DB défaut (Amine) (master=false ici)
  for (int i = 0; i < MAX_DB_CARDS; i++) dbCards[i].used = false;
  dbCards[0].uid[0] = 0x8F; dbCards[0].uid[1] = 0xAB; dbCards[0].uid[2] = 0x8D; dbCards[0].uid[3] = 0xC2;
  strncpy(dbCards[0].name, "Amine", sizeof(dbCards[0].name) - 1);
  dbCards[0].enabled = true;
  dbCards[0].master  = false;
  dbCards[0].used    = true;

  connectWiFi(8000);

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  mqttTryConnectOnce();
}

/* ================= LOOP ================= */
void loop() {
  static unsigned long lastNetTry = 0;

  // réseau
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastNetTry > 10000) {
      lastNetTry = millis();
      connectWiFi(2500);
    }
  } else {
    if (!client.connected()) {
      if (millis() - lastNetTry > 3000) {
        lastNetTry = millis();
        mqttTryConnectOnce();
      }
    } else {
      client.loop();
    }
  }

  // buzzer incendie
  fireBuzzerTick();

  // LCD WiFi/MQTT si pas incendie
  static unsigned long lastNetLcd = 0;
  if (!fireActive && millis() - lastNetLcd >= 1000) {
    lastNetLcd = millis();
    updateNetLCD();
  }

  // DHT + détection incendie + affichage T/H
  if (millis() - lastDhtRead >= 2000) {
    lastDhtRead = millis();
    lastTemp = dht.readTemperature();
    lastHum  = dht.readHumidity();

    if (!isnan(lastTemp)) {
      if (!fireActive && lastTemp > FIRE_ON) enterFireMode();
      else if (fireActive && lastTemp <= FIRE_OFF) exitFireMode();
    }

    if (!fireActive) {
      lcd.setCursor(0, 2);
      if (!isnan(lastTemp) && !isnan(lastHum)) {
        lcd.print("T:");
        lcd.print(lastTemp, 1);
        lcd.print((char)223);
        lcd.print("C ");
        lcd.print("H:");
        lcd.print(lastHum, 0);
        lcd.print("%");
        lcd.print("        ");
      } else {
        lcd.print("T:--.-C H:--%      ");
      }
    }
  }

  // incendie : forcer portes ouvertes + publish + stop logique SAS
  if (fireActive) {
    openBothDoors();
    ledsFire();

    if (millis() - lastMqttPublish >= 1000) {
      lastMqttPublish = millis();
      mqttPublishAll();
    }
    return;
  }

  // RFID
  RfidAction ract = RFID_NONE;
  if (millis() - lastRFIDTrigger > RFID_COOLDOWN) {
    ract = handleRfid();
    if (cardDetected) lastRFIDTrigger = millis();
  }

  // MASTER : ouvre même si WAIT / bloqué
  if (ract == RFID_OPEN_MASTER) {
    openP1(SRC_RFID);
    showMessage("BOSS -> P1 OUV");
  }

  bool rfidOpenP1 = (ract == RFID_OPEN_NORMAL);

  // INIT
  if (currentState == INIT) {
    if (rfidOpenP1) {
      openP1(SRC_RFID);
      showMessage("RFID OK -> P1 OUV");
      buzz(1000);
    } else if (buttonPressed(BTN_P1)) {
      openP1(SRC_BUTTON);
      showMessage("Porte 1 OUVERTE");
    } else if (buttonPressed(BTN_P2)) {
      openP2(SRC_BUTTON);
      showMessage("Porte 2 OUVERTE");
    }
  }

  // refus
  if (currentState == P1_OPEN && buttonPressed(BTN_P2)) {
    showMessage("P2 BLOQUEE (P1 OUV)");
    buzz(1000);
  } else if (currentState == P2_OPEN && (buttonPressed(BTN_P1) || rfidOpenP1)) {
    showMessage("P1 BLOQUEE (P2 OUV)");
    buzz(1000);
  } else if (currentState == WAIT_AFTER_CLOSE &&
             (buttonPressed(BTN_P1) || buttonPressed(BTN_P2) || rfidOpenP1)) {
    showMessage("ACCES REFUSE ATTEND");
    buzz(1000);
  }

  // fermeture => WAIT (timer repart à zéro, y compris après ouverture boss)
  if (currentState == P1_OPEN && buttonPressed(BTN_P1)) {
    closeBothDoors();
    ledsWait();
    showMessage("P1 FERMEE->ATTEND");
    waitTimerStart = millis();
    lastSecondTick = millis();
    currentState = WAIT_AFTER_CLOSE;
  } else if (currentState == P2_OPEN && buttonPressed(BTN_P2)) {
    closeBothDoors();
    ledsWait();
    showMessage("P2 FERMEE->ATTEND");
    waitTimerStart = millis();
    lastSecondTick = millis();
    currentState = WAIT_AFTER_CLOSE;
  }

  // alarme porte > 6s
  if ((currentState == P1_OPEN || currentState == P2_OPEN) &&
      !doorAlarmTriggered &&
      millis() - doorOpenStart >= DOOR_OPEN_LIMIT) {
    buzz(500);
    doorAlarmTriggered = true;
  }

  // WAIT countdown + fin => INIT
  if (currentState == WAIT_AFTER_CLOSE) {
    countdownLCD();
    if (millis() - waitTimerStart >= WAIT_TIME) {
      showMessage("SAS MSD");
      currentState = INIT;
      closeBothDoors();
      ledsAccessOK();
    }
  }

  // MQTT publish périodique
  if (millis() - lastMqttPublish >= 1000) {
    lastMqttPublish = millis();
    mqttPublishAll();
  }
}
