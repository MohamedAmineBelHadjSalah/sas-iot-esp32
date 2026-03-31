#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <MFRC522.h>
#include <DHT.h>

/* ================= WIFI ================= */
const char* ssid = "Xiaomi";
const char* password = "72222388f";

/* ================= MQTT ================= */
const char* mqtt_server = "test.mosquitto.org";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

/* ================= TOPICS ESP1 ================= */
#define TOPIC_SELF_STATE    "sas/esp1/state"
#define TOPIC_SELF_DOOR     "sas/esp1/door"
#define TOPIC_SELF_EVENT    "sas/esp1/event"
#define TOPIC_SELF_RFID     "sas/esp1/rfid"
#define TOPIC_SELF_AUTH     "sas/esp1/auth"
#define TOPIC_SELF_STATUS   "sas/esp1/status"
#define TOPIC_SELF_CMD      "sas/esp1/cmd"
#define TOPIC_SELF_WAIT     "sas/esp1/wait"
#define TOPIC_SELF_TEMP     "sas/esp1/temp"
#define TOPIC_SELF_HUM      "sas/esp1/hum"

/* ================= TOPICS ESP2 ================= */
#define TOPIC_PEER_STATE    "sas/esp2/state"
#define TOPIC_PEER_DOOR     "sas/esp2/door"
#define TOPIC_PEER_AUTH     "sas/esp2/auth"
#define TOPIC_PEER_WAIT     "sas/esp2/wait"
#define TOPIC_PEER_EVENT    "sas/esp2/event"

/* ================= PINS ESP1 ================= */
#define LED_GREEN  25
#define LED_RED    27
#define BUTTON     13
#define BUZZER     15
#define MC38       34

#define SDA_LCD    21
#define SCL_LCD    22

#define SS_PIN     5
#define RST_PIN    26

/* DHT11 */
#define DHTPIN     17
#define DHTTYPE    DHT11

/* ================= LED LOGIC ================= */
#define GREEN_ON   HIGH
#define GREEN_OFF  LOW
#define RED_ON     HIGH
#define RED_OFF    LOW

/* ================= MC38 LOGIC ================= */
#define MC38_CLOSED LOW
#define MC38_OPEN   HIGH

LiquidCrystal_I2C lcd(0x27, 20, 4);
MFRC522 rfid(SS_PIN, RST_PIN);
DHT dht(DHTPIN, DHTTYPE);

enum State { INIT, OPEN, WAIT, REFUSED };
State state = INIT;

const unsigned long WAIT_TIME = 4000;
const unsigned long MC38_DEBOUNCE_MS = 300;
const unsigned long REFUSE_DISPLAY_MS = 1500;
const unsigned long BUTTON_DEBOUNCE_MS = 300;
const unsigned long DHT_INTERVAL = 2000;

unsigned long timerWait = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastButtonTime = 0;
unsigned long lastDoorChange = 0;
unsigned long refuseUntil = 0;
unsigned long lastDhtRead = 0;

int stableDoorState = MC38_CLOSED;
int previousDoorState = MC38_CLOSED;

String peerState = "READY";
String peerDoor  = "CLOSED";
String lastAccessSource = "-";
int peerWaitRemaining = -1;

float lastTemp = NAN;
float lastHum = NAN;

void setGreen(bool on) { digitalWrite(LED_GREEN, on ? GREEN_ON : GREEN_OFF); }
void setRed(bool on)   { digitalWrite(LED_RED,   on ? RED_ON   : RED_OFF); }

String stateToString() {
  switch (state) {
    case INIT: return "READY";
    case OPEN: return "OPEN";
    case WAIT: return "WAIT";
    case REFUSED: return "REFUSED";
    default: return "UNKNOWN";
  }
}

String stateLabel() {
  switch (state) {
    case INIT: return "Accessible";
    case OPEN: return "Accessible";
    case WAIT: return "Attend";
    case REFUSED: return "Refuse";
    default: return "Inconnu";
  }
}

bool localDoorSensorOpen() { return stableDoorState == MC38_OPEN; }
bool localDoorShownOpen()  { return (state == OPEN) || localDoorSensorOpen(); }
bool peerDoorShownOpen()   { return (peerState == "OPEN") || (peerDoor == "OPEN"); }
bool peerBusy()            { return (peerState != "READY") || peerDoorShownOpen(); }
bool globalWaitActive()    { return (state == WAIT) || (peerState == "WAIT"); }

String doorShort(bool isOpen) { return isOpen ? "O" : "F"; }

void publishMessage(const char* topic, const String& payload, bool retained = false) {
  if (!client.connected()) return;
  client.publish(topic, payload.c_str(), retained);
}

void publishState() {
  publishMessage(TOPIC_SELF_STATE, stateToString(), true);
}

void publishDoorShownState() {
  publishMessage(TOPIC_SELF_DOOR, localDoorShownOpen() ? "OPEN" : "CLOSED", true);
}

void publishWaitValue() {
  if (state == WAIT) {
    long remaining = (long)((WAIT_TIME - (millis() - timerWait)) / 1000);
    if (remaining < 0) remaining = 0;
    publishMessage(TOPIC_SELF_WAIT, String(remaining), true);
  } else {
    publishMessage(TOPIC_SELF_WAIT, "-1", true);
  }
}

void publishDHT() {
  if (!isnan(lastTemp)) publishMessage(TOPIC_SELF_TEMP, String(lastTemp, 1), true);
  if (!isnan(lastHum))  publishMessage(TOPIC_SELF_HUM, String(lastHum, 1), true);
}

void updateLEDs() {
  if (globalWaitActive()) {
    setGreen(false);
    setRed(true);
  } else if (state == INIT || state == OPEN) {
    setGreen(true);
    setRed(false);
  } else if (state == REFUSED) {
    setGreen(false);
    setRed(true);
  }
}

void buzzerShort() {
  digitalWrite(BUZZER, HIGH);
  delay(140);
  digitalWrite(BUZZER, LOW);
}

void buzzerDouble() {
  digitalWrite(BUZZER, HIGH);
  delay(80);
  digitalWrite(BUZZER, LOW);
  delay(80);
  digitalWrite(BUZZER, HIGH);
  delay(80);
  digitalWrite(BUZZER, LOW);
}

void updateLCD() {
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("Etat:");
  lcd.print(stateLabel());

  lcd.setCursor(0, 1);
  lcd.print("P1:");
  lcd.print(doorShort(localDoorShownOpen()));
  lcd.print(" P2:");
  lcd.print(doorShort(peerDoorShownOpen()));

  lcd.setCursor(0, 2);
  if (state == WAIT) {
    long remaining = (long)((WAIT_TIME - (millis() - timerWait)) / 1000);
    if (remaining < 0) remaining = 0;
    lcd.print("Temps P1:");
    lcd.print(remaining);
    lcd.print("s   ");
  } else if (peerState == "WAIT" && peerWaitRemaining >= 0) {
    lcd.print("Temps P2:");
    lcd.print(peerWaitRemaining);
    lcd.print("s   ");
  } else {
    lcd.print("Temps:-           ");
  }

  lcd.setCursor(0, 3);
  if (!isnan(lastTemp) && !isnan(lastHum)) {
    lcd.print("T:");
    lcd.print(lastTemp, 1);
    lcd.print((char)223);
    lcd.print("C H:");
    lcd.print(lastHum, 0);
    lcd.print("%   ");
  } else {
    lcd.print("Src:");
    lcd.print(lastAccessSource);
    lcd.print("            ");
  }
}

String uidToString() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) uid += ":";
  }
  uid.toUpperCase();
  return uid;
}

void setRefused(const String& reason, const String& src = "-") {
  state = REFUSED;
  refuseUntil = millis();
  if (src != "-") lastAccessSource = src;
  updateLEDs();
  updateLCD();
  publishState();
  publishDoorShownState();
  publishWaitValue();
  publishMessage(TOPIC_SELF_EVENT, "open_refused_" + reason);
}

bool canOpenDoor() {
  if (state != INIT) {
    setRefused("state_not_ready");
    return false;
  }
  if (localDoorSensorOpen()) {
    setRefused("local_door_already_open");
    return false;
  }
  if (peerBusy()) {
    setRefused("other_door_busy");
    return false;
  }
  return true;
}

void openDoor(const String& src) {
  state = OPEN;
  lastAccessSource = src;
  updateLEDs();
  updateLCD();
  publishState();
  publishDoorShownState();
  publishWaitValue();
  publishMessage(TOPIC_SELF_AUTH, src, true);
  publishMessage(TOPIC_SELF_EVENT, "door1_opened");
  buzzerShort();
}

void startWaitMode() {
  state = WAIT;
  timerWait = millis();
  updateLEDs();
  updateLCD();
  publishState();
  publishDoorShownState();
  publishWaitValue();
  publishMessage(TOPIC_SELF_EVENT, "door1_closed_wait");
  buzzerDouble();
}

bool checkRFID() {
  if (!rfid.PICC_IsNewCardPresent()) return false;
  if (!rfid.PICC_ReadCardSerial()) return false;

  String uid = uidToString();
  publishMessage(TOPIC_SELF_RFID, uid);
  publishMessage(TOPIC_SELF_EVENT, "rfid_detected");

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return true;
}

bool buttonPressed() {
  if (digitalRead(BUTTON) == LOW && millis() - lastButtonTime > BUTTON_DEBOUNCE_MS) {
    lastButtonTime = millis();
    return true;
  }
  return false;
}

void readDHT11() {
  if (millis() - lastDhtRead < DHT_INTERVAL) return;
  lastDhtRead = millis();

  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(t) || isnan(h)) {
    Serial.println("Erreur lecture DHT11");
    return;
  }

  lastTemp = t;
  lastHum = h;

  publishDHT();
  updateLCD();

  Serial.print("Temp: ");
  Serial.print(lastTemp);
  Serial.print(" C | Hum: ");
  Serial.print(lastHum);
  Serial.println(" %");
}

void setup_wifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();
  String t = String(topic);

  if (t == TOPIC_PEER_STATE) {
    peerState = msg;
    updateLEDs();
    updateLCD();
  }
  else if (t == TOPIC_PEER_DOOR) {
    peerDoor = msg;
    updateLCD();
  }
  else if (t == TOPIC_PEER_AUTH) {
    if (msg == "FINGER") lastAccessSource = "P2-FINGER";
    else if (msg == "BUTTON") lastAccessSource = "P2-BUTTON";
    else if (msg == "CMD") lastAccessSource = "P2-CMD";
    else lastAccessSource = "P2-" + msg;
    updateLCD();
  }
  else if (t == TOPIC_PEER_WAIT) {
    peerWaitRemaining = (msg == "-1") ? -1 : msg.toInt();
    updateLCD();
  }
  else if (t == TOPIC_PEER_EVENT) {
    if (msg == "finger_not_recognized") {
      lastAccessSource = "P2-FINGER";
      updateLCD();
    } else if (msg == "door2_opened") {
      buzzerShort();
      peerState = "OPEN";
      peerDoor = "OPEN";
      updateLEDs();
      updateLCD();
    } else if (msg == "door2_closed_wait") {
      buzzerDouble();
      peerState = "WAIT";
      updateLEDs();
      updateLCD();
    } else if (msg == "state_ready") {
      peerState = "READY";
      peerWaitRemaining = -1;
      updateLEDs();
      updateLCD();
    }
  }
  else if (t == TOPIC_SELF_CMD) {
    if (msg == "OPEN") {
      if (canOpenDoor()) openDoor("CMD");
    }
    else if (msg == "CLOSE") {
      if (state == OPEN) startWaitMode();
    }
    else if (msg == "RESET") {
      state = INIT;
      updateLEDs();
      updateLCD();
      publishState();
      publishDoorShownState();
      publishWaitValue();
      publishMessage(TOPIC_SELF_EVENT, "reset_done");
    }
  }
}

void reconnect() {
  while (!client.connected()) {
    String clientId = "ESP32_P1_";
    clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

    if (client.connect(clientId.c_str())) {
      client.subscribe(TOPIC_SELF_CMD);
      client.subscribe(TOPIC_PEER_STATE);
      client.subscribe(TOPIC_PEER_DOOR);
      client.subscribe(TOPIC_PEER_AUTH);
      client.subscribe(TOPIC_PEER_WAIT);
      client.subscribe(TOPIC_PEER_EVENT);

      publishMessage(TOPIC_SELF_STATUS, "online", true);
      publishState();
      publishDoorShownState();
      publishWaitValue();
      publishDHT();
      publishMessage(TOPIC_SELF_EVENT, "boot", true);
    } else {
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);
  pinMode(MC38, INPUT);

  setGreen(false);
  setRed(false);

  Wire.begin(SDA_LCD, SCL_LCD);
  lcd.init();
  lcd.backlight();

  SPI.begin();
  rfid.PCD_Init();

  dht.begin();
  delay(2000);

  stableDoorState = digitalRead(MC38);
  previousDoorState = stableDoorState;

  state = INIT;
  updateLEDs();
  updateLCD();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  setup_wifi();
  reconnect();

  publishDoorShownState();
  publishWaitValue();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) setup_wifi();
  if (!client.connected()) reconnect();
  client.loop();

  readDHT11();

  if (millis() - lastHeartbeat > 5000) {
    lastHeartbeat = millis();
    publishMessage(TOPIC_SELF_STATUS, "online", true);
    publishState();
    publishDoorShownState();
    publishWaitValue();
    publishDHT();
    updateLEDs();
    updateLCD();
  }

  int rawDoorState = digitalRead(MC38);
  if (rawDoorState != stableDoorState && millis() - lastDoorChange > MC38_DEBOUNCE_MS) {
    previousDoorState = stableDoorState;
    stableDoorState = rawDoorState;
    lastDoorChange = millis();

    publishDoorShownState();

    if (previousDoorState == MC38_OPEN && stableDoorState == MC38_CLOSED) {
      if (state != WAIT) {
        startWaitMode();
      }
    }

    updateLEDs();
    updateLCD();
  }

  if (checkRFID()) {
    if (canOpenDoor()) {
      openDoor("RFID");
    }
  }

  if (buttonPressed()) {
    if (state == INIT) {
      if (canOpenDoor()) openDoor("BUTTON");
    } else if (state == OPEN) {
      startWaitMode();
    }
  }

  if (state == REFUSED && millis() - refuseUntil > REFUSE_DISPLAY_MS) {
    state = INIT;
    updateLEDs();
    updateLCD();
    publishState();
    publishDoorShownState();
    publishWaitValue();
  }

  if (state == WAIT) {
    updateLCD();
    publishWaitValue();

    if (millis() - timerWait > WAIT_TIME) {
      state = INIT;
      updateLEDs();
      updateLCD();
      publishState();
      publishDoorShownState();
      publishWaitValue();
      publishMessage(TOPIC_SELF_EVENT, "state_ready");
    }
  }
}