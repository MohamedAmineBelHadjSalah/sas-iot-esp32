#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <time.h>

/* ================= WIFI ================= */
const char* ssid = "Lenovo16";
const char* password = "Lenovo16";

/* ================= MQTT ================= */
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

/* ================= NTP ================= */
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";
const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 0;

/* ================= TOPICS ESP1 ================= */
#define TOPIC_SELF_STATE         "sas/esp1/state"
#define TOPIC_SELF_DOOR          "sas/esp1/door"
#define TOPIC_SELF_EVENT         "sas/esp1/event"
#define TOPIC_SELF_RFID          "sas/esp1/rfid"
#define TOPIC_SELF_RFID_RESULT   "sas/esp1/rfid_result"
#define TOPIC_SELF_RFID_CONSUME  "sas/esp1/rfid_consume"
#define TOPIC_SELF_AUTH          "sas/esp1/auth"
#define TOPIC_SELF_STATUS        "sas/esp1/status"
#define TOPIC_SELF_CMD           "sas/esp1/cmd"
#define TOPIC_SELF_WAIT          "sas/esp1/wait"
#define TOPIC_SELF_TEMP          "sas/esp1/temp"
#define TOPIC_SELF_HUM           "sas/esp1/hum"

/* ================= TOPICS RFID DB ================= */
#define TOPIC_RFID_DB            "sas/rfid/db"
#define TOPIC_NOTIFY             "sas/notify"

/* ================= TOPICS ESP2 ================= */
#define TOPIC_PEER_STATE         "sas/esp2/state"
#define TOPIC_PEER_DOOR          "sas/esp2/door"
#define TOPIC_PEER_AUTH          "sas/esp2/auth"
#define TOPIC_PEER_WAIT          "sas/esp2/wait"
#define TOPIC_PEER_EVENT         "sas/esp2/event"
#define TOPIC_PEER_BELL          "sas/esp2/bell"

/* ================= PINS ESP1 ================= */
#define LED_GREEN   25
#define LED_RED     27
#define STATUS_LED  21   // LED statut WiFi + MQTT
#define BUTTON      13
#define BUZZER      15
#define MC38        34
#define SS_PIN      5
#define RST_PIN     26
#define DHTPIN      17
#define DHTTYPE     DHT11

/* ================= LED LOGIC ================= */
#define GREEN_ON    HIGH
#define GREEN_OFF   LOW
#define RED_ON      HIGH
#define RED_OFF     LOW
#define STATUS_ON   HIGH
#define STATUS_OFF  LOW

/* ================= MC38 LOGIC ================= */
#define MC38_CLOSED LOW
#define MC38_OPEN   HIGH

/* ================= RFID DB ================= */
#define MAX_RFID_CARDS 50

struct RfidCard {
  char uid[24];
  char name[32];
  bool enabled;
  unsigned long allowedFrom;
  unsigned long allowedTo;
  int maxUses;
  int usedCount;
  bool used;
};

RfidCard rfidDb[MAX_RFID_CARDS];
int rfidDbCount = 0;

/* ================= OBJECTS ================= */
MFRC522 rfid(SS_PIN, RST_PIN);
DHT dht(DHTPIN, DHTTYPE);

/* ================= STATE ================= */
enum State { INIT, OPEN, WAIT, REFUSED };
State state = INIT;

/* ================= TIMERS ================= */
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

/* ================= RUNTIME ================= */
int stableDoorState = MC38_CLOSED;
int previousDoorState = MC38_CLOSED;
String peerState = "READY";
String peerDoor  = "CLOSED";
String lastAccessSource = "-";
int peerWaitRemaining = -1;
float lastTemp = NAN;
float lastHum = NAN;
bool bellFromEsp2 = false;

/* ================= HELPERS ================= */
void clearRfidDb() {
  rfidDbCount = 0;
  for (int i = 0; i < MAX_RFID_CARDS; i++) {
    rfidDb[i].uid[0] = '\0';
    rfidDb[i].name[0] = '\0';
    rfidDb[i].enabled = false;
    rfidDb[i].allowedFrom = 0;
    rfidDb[i].allowedTo = 0;
    rfidDb[i].maxUses = 0;
    rfidDb[i].usedCount = 0;
    rfidDb[i].used = false;
  }
}

String normalizeUid(String uid) {
  uid.trim();
  uid.toUpperCase();
  String out = "";
  for (unsigned int i = 0; i < uid.length(); i++) {
    char c = uid[i];
    bool isHex =
      (c >= '0' && c <= '9') ||
      (c >= 'A' && c <= 'F');
    if (isHex) out += c;
  }
  return out;
}

void setGreen(bool on) { digitalWrite(LED_GREEN, on ? GREEN_ON : GREEN_OFF); }
void setRed(bool on)   { digitalWrite(LED_RED, on ? RED_ON : RED_OFF); }

void updateStatusLed() {
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bool mqttOk = client.connected();
  digitalWrite(STATUS_LED, (wifiOk && mqttOk) ? STATUS_ON : STATUS_OFF);
}

void applyBuzzerState() {
  digitalWrite(BUZZER, bellFromEsp2 ? HIGH : LOW);
}

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
  if (bellFromEsp2) return;
  digitalWrite(BUZZER, HIGH);
  delay(140);
  digitalWrite(BUZZER, LOW);
}

void buzzerDouble() {
  if (bellFromEsp2) return;
  digitalWrite(BUZZER, HIGH);
  delay(80);
  digitalWrite(BUZZER, LOW);
  delay(80);
  digitalWrite(BUZZER, HIGH);
  delay(80);
  digitalWrite(BUZZER, LOW);
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

int findRfidCardIndex(const String& uid) {
  String n1 = normalizeUid(uid);
  for (int i = 0; i < MAX_RFID_CARDS; i++) {
    if (!rfidDb[i].used) continue;
    String n2 = normalizeUid(String(rfidDb[i].uid));
    if (n1 == n2) return i;
  }
  return -1;
}

bool isTimeSynced() {
  time_t now;
  time(&now);
  return now > 1700000000;
}

unsigned long getNowEpoch() {
  time_t now;
  time(&now);
  return (unsigned long)now;
}

void setupTimeSync() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
}

void publishNotification(const String& type, const String& uid, const String& name, const String& message) {
  StaticJsonDocument<256> doc;
  doc["type"] = type;
  doc["uid"] = uid;
  doc["name"] = name;
  doc["message"] = message;
  char buffer[256];
  serializeJson(doc, buffer);
  publishMessage(TOPIC_NOTIFY, buffer);
}

bool loadRfidDbFromJson(const String& json) {
  StaticJsonDocument<16384> doc;

  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.print("Erreur JSON RFID DB: ");
    Serial.println(err.c_str());
    publishMessage(TOPIC_SELF_EVENT, "rfid_db_parse_error");
    return false;
  }

  if (!doc.is<JsonArray>()) {
    publishMessage(TOPIC_SELF_EVENT, "rfid_db_invalid_format");
    return false;
  }

  clearRfidDb();

  JsonArray arr = doc.as<JsonArray>();
  int idx = 0;

  for (JsonObject item : arr) {
    if (idx >= MAX_RFID_CARDS) break;

    String uid = item["uid"] | "";
    String name = item["name"] | "";
    bool enabled = item["enabled"] | false;
    unsigned long allowedFrom = item["allowedFrom"] | 0UL;
    unsigned long allowedTo   = item["allowedTo"]   | 0UL;
    int maxUses = item["maxUses"] | 0;
    int usedCount = item["usedCount"] | 0;

    uid = normalizeUid(uid);
    name.trim();

    if (uid.length() == 0) continue;

    strncpy(rfidDb[idx].uid, uid.c_str(), sizeof(rfidDb[idx].uid) - 1);
    rfidDb[idx].uid[sizeof(rfidDb[idx].uid) - 1] = '\0';

    strncpy(rfidDb[idx].name, name.c_str(), sizeof(rfidDb[idx].name) - 1);
    rfidDb[idx].name[sizeof(rfidDb[idx].name) - 1] = '\0';

    rfidDb[idx].enabled = enabled;
    rfidDb[idx].allowedFrom = allowedFrom;
    rfidDb[idx].allowedTo = allowedTo;
    rfidDb[idx].maxUses = maxUses;
    rfidDb[idx].usedCount = usedCount;
    rfidDb[idx].used = true;
    idx++;
  }

  rfidDbCount = idx;

  Serial.println("=== RFID DB RECUE ===");
  Serial.println(json);
  Serial.print("RFID DB chargee: ");
  Serial.println(rfidDbCount);

  for (int i = 0; i < rfidDbCount; i++) {
    Serial.print("Card ");
    Serial.print(i);
    Serial.print(" UID=");
    Serial.print(rfidDb[i].uid);
    Serial.print(" NAME=");
    Serial.println(rfidDb[i].name);
  }

  publishMessage(TOPIC_SELF_EVENT, "rfid_db_loaded");
  return true;
}

void setRefused(const String& reason, const String& src = "-") {
  state = REFUSED;
  refuseUntil = millis();
  if (src != "-") lastAccessSource = src;
  updateLEDs();
  publishState();
  publishDoorShownState();
  publishWaitValue();
  publishMessage(TOPIC_SELF_EVENT, "open_refused_" + reason);
}

bool canOpenDoor() {
  Serial.println("=== CHECK canOpenDoor ===");
  Serial.print("state = ");
  Serial.println(stateToString());
  Serial.print("localDoorSensorOpen = ");
  Serial.println(localDoorSensorOpen() ? "true" : "false");
  Serial.print("peerState = ");
  Serial.println(peerState);
  Serial.print("peerDoor = ");
  Serial.println(peerDoor);
  Serial.print("peerBusy = ");
  Serial.println(peerBusy() ? "true" : "false");

  if (state != INIT) {
    Serial.println("REFUS: state_not_ready");
    setRefused("state_not_ready");
    return false;
  }
  if (localDoorSensorOpen()) {
    Serial.println("REFUS: local_door_already_open");
    setRefused("local_door_already_open");
    return false;
  }
  if (peerBusy()) {
    Serial.println("REFUS: other_door_busy");
    setRefused("other_door_busy");
    return false;
  }

  Serial.println("OK: canOpenDoor = true");
  return true;
}

void openDoor(const String& src) {
  state = OPEN;
  lastAccessSource = src;
  updateLEDs();
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
  publishState();
  publishDoorShownState();
  publishWaitValue();
  publishMessage(TOPIC_SELF_EVENT, "door1_closed_wait");
  buzzerDouble();
}

bool readRFID(String &uid) {
  if (!rfid.PICC_IsNewCardPresent()) return false;
  if (!rfid.PICC_ReadCardSerial()) return false;

  uid = uidToString();
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
}

void setup_wifi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    updateStatusLed();
  }
  updateStatusLed();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  String t = String(topic);

  if (t == TOPIC_PEER_STATE) {
    peerState = msg;
    updateLEDs();
  }
  else if (t == TOPIC_PEER_DOOR) {
    peerDoor = msg;
  }
  else if (t == TOPIC_PEER_AUTH) {
    if (msg == "FINGER") lastAccessSource = "P2-FINGER";
    else if (msg == "BUTTON") lastAccessSource = "P2-BUTTON";
    else if (msg == "CMD") lastAccessSource = "P2-CMD";
    else lastAccessSource = "P2-" + msg;
  }
  else if (t == TOPIC_PEER_WAIT) {
    peerWaitRemaining = (msg == "-1") ? -1 : msg.toInt();
  }
  else if (t == TOPIC_PEER_BELL) {
    bellFromEsp2 = (msg == "ON");
    applyBuzzerState();
  }
  else if (t == TOPIC_PEER_EVENT) {
    if (msg == "finger_not_recognized") {
      lastAccessSource = "P2-FINGER";
    } else if (msg == "door2_opened") {
      buzzerShort();
      peerState = "OPEN";
      peerDoor = "OPEN";
      updateLEDs();
    } else if (msg == "door2_closed_wait") {
      buzzerDouble();
      peerState = "WAIT";
      updateLEDs();
    } else if (msg == "state_ready") {
      peerState = "READY";
      peerWaitRemaining = -1;
      updateLEDs();
    }
  }
  else if (t == TOPIC_RFID_DB) {
    Serial.println("MQTT RFID DB recu");
    Serial.print("Longueur payload = ");
    Serial.println(msg.length());
    loadRfidDbFromJson(msg);
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
      publishState();
      publishDoorShownState();
      publishWaitValue();
      publishMessage(TOPIC_SELF_EVENT, "reset_done");
    }
  }
}

void reconnect() {
  while (!client.connected()) {
    updateStatusLed();

    String clientId = "ESP32_P1_";
    clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

    if (client.connect(clientId.c_str())) {
      client.subscribe(TOPIC_SELF_CMD);
      client.subscribe(TOPIC_PEER_STATE);
      client.subscribe(TOPIC_PEER_DOOR);
      client.subscribe(TOPIC_PEER_AUTH);
      client.subscribe(TOPIC_PEER_WAIT);
      client.subscribe(TOPIC_PEER_EVENT);
      client.subscribe(TOPIC_PEER_BELL);
      client.subscribe(TOPIC_RFID_DB);

      publishMessage(TOPIC_SELF_STATUS, "online", true);
      publishState();
      publishDoorShownState();
      publishWaitValue();
      publishDHT();
      publishMessage(TOPIC_SELF_EVENT, "boot", true);
      publishMessage(TOPIC_SELF_EVENT, "rfid_db_waiting");
    } else {
      delay(2000);
    }

    updateStatusLed();
  }
}

void setup() {
  Serial.begin(115200);

  clearRfidDb();

  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(STATUS_LED, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);
  pinMode(MC38, INPUT);

  setGreen(false);
  setRed(false);
  digitalWrite(STATUS_LED, STATUS_OFF);
  digitalWrite(BUZZER, LOW);

  SPI.begin();
  rfid.PCD_Init();

  dht.begin();
  delay(2000);

  stableDoorState = digitalRead(MC38);
  previousDoorState = stableDoorState;

  state = INIT;
  bellFromEsp2 = false;
  updateLEDs();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  client.setBufferSize(4096);

  setup_wifi();
  setupTimeSync();
  reconnect();

  publishDoorShownState();
  publishWaitValue();
  applyBuzzerState();
  updateStatusLed();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) setup_wifi();
  if (!client.connected()) reconnect();
  client.loop();

  updateStatusLed();
  readDHT11();

  if (millis() - lastHeartbeat > 5000) {
    lastHeartbeat = millis();
    publishMessage(TOPIC_SELF_STATUS, "online", true);
    publishState();
    publishDoorShownState();
    publishWaitValue();
    publishDHT();
    updateLEDs();
    applyBuzzerState();
    updateStatusLed();
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
  }

  String scannedUid = "";
  if (readRFID(scannedUid)) {
    Serial.print("Badge scanne brut: ");
    Serial.println(scannedUid);
    Serial.print("Badge scanne normalise: ");
    Serial.println(normalizeUid(scannedUid));

    int idx = findRfidCardIndex(scannedUid);

    if (idx < 0) {
      Serial.println("Resultat: UNKNOWN");
      publishMessage(TOPIC_SELF_RFID_RESULT, "UNKNOWN");
      publishNotification("rfid_denied", scannedUid, "", "Badge inconnu");
      setRefused("rfid_unknown", "RFID");
    }
    else if (!rfidDb[idx].enabled) {
      Serial.println("Resultat: DISABLED");
      publishMessage(TOPIC_SELF_RFID_RESULT, "DISABLED");
      publishNotification("rfid_denied", scannedUid, String(rfidDb[idx].name), "Badge désactivé");
      setRefused("rfid_disabled", "RFID");
    }
    else if (!isTimeSynced()) {
      Serial.println("Resultat: TIME_NOT_SYNCED");
      publishMessage(TOPIC_SELF_RFID_RESULT, "TIME_NOT_SYNCED");
      publishNotification("rfid_denied", scannedUid, String(rfidDb[idx].name), "Heure non synchronisée");
      setRefused("rfid_time_not_synced", "RFID");
    }
    else {
      unsigned long nowEpoch = getNowEpoch();

      if (rfidDb[idx].allowedFrom > 0 && nowEpoch < rfidDb[idx].allowedFrom) {
        Serial.println("Resultat: TOO_EARLY");
        publishMessage(TOPIC_SELF_RFID_RESULT, "TOO_EARLY");
        publishNotification("rfid_denied", scannedUid, String(rfidDb[idx].name), "Accès pas encore autorisé");
        setRefused("rfid_too_early", "RFID");
      }
      else if (rfidDb[idx].allowedTo > 0 && nowEpoch > rfidDb[idx].allowedTo) {
        Serial.println("Resultat: EXPIRED");
        publishMessage(TOPIC_SELF_RFID_RESULT, "EXPIRED");
        publishNotification("rfid_denied", scannedUid, String(rfidDb[idx].name), "Autorisation expirée");
        setRefused("rfid_expired", "RFID");
      }
      else if (rfidDb[idx].maxUses > 0 && rfidDb[idx].usedCount >= rfidDb[idx].maxUses) {
        Serial.println("Resultat: USE_LIMIT_REACHED");
        publishMessage(TOPIC_SELF_RFID_RESULT, "USE_LIMIT_REACHED");
        publishNotification("rfid_denied", scannedUid, String(rfidDb[idx].name), "Nombre d'utilisations dépassé");
        setRefused("rfid_use_limit", "RFID");
      }
      else {
        if (canOpenDoor()) {
          Serial.print("Badge trouve: ");
          Serial.println(rfidDb[idx].name);
          Serial.println("OUVERTURE PORTE...");
          rfidDb[idx].usedCount++;
          publishMessage(TOPIC_SELF_RFID_RESULT, "AUTHORIZED");
          publishMessage(TOPIC_SELF_RFID_CONSUME, scannedUid);
          openDoor("RFID");
        } else {
          Serial.println("PORTE NON OUVERTE: canOpenDoor a refuse");
        }
      }
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
    publishState();
    publishDoorShownState();
    publishWaitValue();
  }

  if (state == WAIT) {
    publishWaitValue();

    if (millis() - timerWait > WAIT_TIME) {
      state = INIT;
      updateLEDs();
      publishState();
      publishDoorShownState();
      publishWaitValue();
      publishMessage(TOPIC_SELF_EVENT, "state_ready");
    }
  }

  applyBuzzerState();
  updateStatusLed();
}