#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_Fingerprint.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <time.h>

/* ================= CONFIG PORTE ================= */
#define DOOR_ID "Porte_urgence"   // changer en door2, door3...

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

/* ================= PINS ================= */
#define LED_GREEN   25
#define LED_RED     27
#define LED_STATUS  21

#define BUTTON_OPEN   13
#define BUTTON_CLOSE  35

#define BUZZER 4
#define RELAY  14

#define MC38_PIN 34
#define PIR_PIN  33
#define DHTPIN   32
#define DHTTYPE  DHT11

#define RFID_SS   5
#define RFID_RST  22
#define RFID_SCK  18
#define RFID_MISO 19
#define RFID_MOSI 23

#define FINGER_RX 16
#define FINGER_TX 17

/* ================= LOGIC ================= */
#define MC38_CLOSED LOW
#define MC38_OPEN   HIGH
#define PIR_ACTIVE  HIGH

#define RELAY_ON  HIGH
#define RELAY_OFF LOW

const unsigned long WAIT_TIME = 4000;
const unsigned long RELAY_TIME = 1200;
const unsigned long REFUSED_TIME = 1500;
const unsigned long BUTTON_DEBOUNCE = 300;
const unsigned long DHT_INTERVAL = 3000;
const unsigned long HEARTBEAT_TIME = 5000;

/* ================= OBJECTS ================= */
MFRC522 rfid(RFID_SS, RFID_RST);
HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger(&fingerSerial);
DHT dht(DHTPIN, DHTTYPE);

/* ================= STATE ================= */
enum State { READY, OPEN, WAIT, REFUSED };
State state = READY;

unsigned long waitStart = 0;
unsigned long relayStart = 0;
unsigned long refusedStart = 0;
unsigned long lastDhtRead = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastButtonOpen = 0;
unsigned long lastButtonClose = 0;

bool relayActive = false;
bool otherDoorBusy = false;

/* NEW : verrouillage relation Node-RED */
bool relationLocked = false;

float lastTemp = NAN;
float lastHum = NAN;

/* ================= DATABASE ================= */
#define MAX_RFID_USERS 50
#define MAX_FINGER_USERS 50

struct RfidUser {
  char uid[24];
  char name[32];
  bool enabled;
  unsigned long allowedFrom;
  unsigned long allowedTo;
  int maxUses;
  int usedCount;
  char allowedDoors[120];
  bool used;
};

struct FingerUser {
  int id;
  char name[32];
  bool enabled;
  unsigned long allowedFrom;
  unsigned long allowedTo;
  int maxUses;
  int usedCount;
  char allowedDoors[120];
  bool used;
};

RfidUser rfidDb[MAX_RFID_USERS];
FingerUser fingerDb[MAX_FINGER_USERS];

/* ================= TOPICS ================= */
String baseTopic() {
  return "sas/" + String(DOOR_ID) + "/";
}

String topic(String sub) {
  return baseTopic() + sub;
}

/* ================= MQTT PUBLISH ================= */
void publishMsg(String t, String msg, bool retained = false) {
  if (client.connected()) {
    client.publish(t.c_str(), msg.c_str(), retained);
  }
}

String stateToString() {
  if (state == READY) return "READY";
  if (state == OPEN) return "OPEN";
  if (state == WAIT) return "WAIT";
  if (state == REFUSED) return "REFUSED";
  return "UNKNOWN";
}

/* ================= LEDs / BUZZER ================= */
void updateLeds() {
  digitalWrite(LED_STATUS, (WiFi.status() == WL_CONNECTED && client.connected()) ? HIGH : LOW);

  if (relationLocked) {
    digitalWrite(LED_GREEN, LOW);
    digitalWrite(LED_RED, HIGH);
    return;
  }

  if (state == READY || state == OPEN) {
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_RED, LOW);
  } else {
    digitalWrite(LED_GREEN, LOW);
    digitalWrite(LED_RED, HIGH);
  }
}

void beepShort() {
  digitalWrite(BUZZER, HIGH);
  delay(100);
  digitalWrite(BUZZER, LOW);
}

void beepDouble() {
  digitalWrite(BUZZER, HIGH);
  delay(80);
  digitalWrite(BUZZER, LOW);
  delay(80);
  digitalWrite(BUZZER, HIGH);
  delay(80);
  digitalWrite(BUZZER, LOW);
}

/* ================= DB HELPERS ================= */
void clearRfidDb() {
  for (int i = 0; i < MAX_RFID_USERS; i++) {
    rfidDb[i].uid[0] = '\0';
    rfidDb[i].name[0] = '\0';
    rfidDb[i].allowedDoors[0] = '\0';
    rfidDb[i].enabled = false;
    rfidDb[i].allowedFrom = 0;
    rfidDb[i].allowedTo = 0;
    rfidDb[i].maxUses = 0;
    rfidDb[i].usedCount = 0;
    rfidDb[i].used = false;
  }
}

void clearFingerDb() {
  for (int i = 0; i < MAX_FINGER_USERS; i++) {
    fingerDb[i].id = -1;
    fingerDb[i].name[0] = '\0';
    fingerDb[i].allowedDoors[0] = '\0';
    fingerDb[i].enabled = false;
    fingerDb[i].allowedFrom = 0;
    fingerDb[i].allowedTo = 0;
    fingerDb[i].maxUses = 0;
    fingerDb[i].usedCount = 0;
    fingerDb[i].used = false;
  }
}

String normalizeUid(String uid) {
  uid.trim();
  uid.toUpperCase();

  String out = "";
  for (int i = 0; i < uid.length(); i++) {
    char c = uid[i];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) out += c;
  }
  return out;
}

int findRfid(String uid) {
  String n1 = normalizeUid(uid);

  for (int i = 0; i < MAX_RFID_USERS; i++) {
    if (!rfidDb[i].used) continue;
    if (normalizeUid(String(rfidDb[i].uid)) == n1) return i;
  }

  return -1;
}

int findFinger(int id) {
  for (int i = 0; i < MAX_FINGER_USERS; i++) {
    if (!fingerDb[i].used) continue;
    if (fingerDb[i].id == id) return i;
  }

  return -1;
}

/* ================= TIME ================= */
bool isTimeSynced() {
  time_t now;
  time(&now);
  return now > 1700000000;
}

unsigned long nowEpoch() {
  time_t now;
  time(&now);
  return (unsigned long)now;
}

bool checkTimeAccess(unsigned long from, unsigned long to) {
  if (!isTimeSynced()) return false;

  unsigned long now = nowEpoch();

  if (from > 0 && now < from) return false;
  if (to > 0 && now > to) return false;

  return true;
}

/* ================= ACCESS DOORS ================= */
bool isDoorAllowedForCard(int idx) {
  String doors = String(rfidDb[idx].allowedDoors);
  doors.trim();

  if (doors.length() == 0) return true;
  if (doors.indexOf("ALL") >= 0) return true;

  String currentDoor = String(DOOR_ID);
  return doors.indexOf(currentDoor) >= 0;
}

bool isDoorAllowedForFinger(int idx) {
  String doors = String(fingerDb[idx].allowedDoors);
  doors.trim();

  if (doors.length() == 0) return true;
  if (doors.indexOf("ALL") >= 0) return true;

  String currentDoor = String(DOOR_ID);
  return doors.indexOf(currentDoor) >= 0;
}

/* ================= PUBLISH STATE ================= */
void publishAll() {
  publishMsg(topic("state"), stateToString(), true);
  publishMsg(topic("door"), digitalRead(MC38_PIN) == MC38_OPEN ? "OPEN" : "CLOSED", true);
  publishMsg(topic("pir"), digitalRead(PIR_PIN) == PIR_ACTIVE ? "DETECTED" : "CLEAR", true);
  publishMsg(topic("lock"), relationLocked ? "LOCKED" : "UNLOCKED", true);

  if (!isnan(lastTemp)) publishMsg(topic("temp"), String(lastTemp, 1), true);
  if (!isnan(lastHum)) publishMsg(topic("hum"), String(lastHum, 1), true);
}

/* ================= DOOR LOGIC ================= */
bool canOpenDoor() {
  if (relationLocked) {
    publishMsg(topic("event"), "refused_relation_locked");
    publishMsg(topic("lock_status"), "LOCKED", true);
    beepDouble();
    return false;
  }

  if (state != READY) {
    publishMsg(topic("event"), "refused_state_not_ready");
    return false;
  }

  if (digitalRead(MC38_PIN) == MC38_OPEN) {
    publishMsg(topic("event"), "refused_local_door_open");
    return false;
  }

  if (otherDoorBusy) {
    publishMsg(topic("event"), "refused_other_door_busy");
    return false;
  }

  return true;
}

void setRefused(String reason) {
  state = REFUSED;
  refusedStart = millis();

  publishMsg(topic("state"), "REFUSED", true);
  publishMsg(topic("event"), "open_refused_" + reason);

  updateLeds();
  beepDouble();
}

void openDoor(String src) {
  if (!canOpenDoor()) {
    setRefused(src);
    return;
  }

  state = OPEN;
  relayActive = true;
  relayStart = millis();

  digitalWrite(RELAY, RELAY_ON);

  publishMsg(topic("state"), "OPEN", true);
  publishMsg(topic("auth"), src, true);
  publishMsg(topic("event"), "door_opened_by_" + src);

  updateLeds();
  beepShort();
}

void startWaitMode() {
  digitalWrite(RELAY, RELAY_OFF);
  relayActive = false;

  state = WAIT;
  waitStart = millis();

  publishMsg(topic("state"), "WAIT", true);
  publishMsg(topic("event"), "door_closed_wait");

  updateLeds();
  beepDouble();
}

void resetDoor() {
  digitalWrite(RELAY, RELAY_OFF);
  relayActive = false;

  state = READY;

  publishMsg(topic("state"), "READY", true);
  publishMsg(topic("wait"), "-1", true);
  publishMsg(topic("event"), "reset_done");

  updateLeds();
}

void handleStateMachine() {
  if (relayActive && millis() - relayStart > RELAY_TIME) {
    digitalWrite(RELAY, RELAY_OFF);
    relayActive = false;
  }

  if (state == OPEN) {
    if (digitalRead(MC38_PIN) == MC38_CLOSED) {
      startWaitMode();
    }
  }

  if (state == WAIT) {
    long remain = (WAIT_TIME - (millis() - waitStart)) / 1000;
    if (remain < 0) remain = 0;

    publishMsg(topic("wait"), String(remain), true);

    if (millis() - waitStart > WAIT_TIME) {
      state = READY;

      publishMsg(topic("state"), "READY", true);
      publishMsg(topic("wait"), "-1", true);
      publishMsg(topic("event"), "state_ready");

      updateLeds();
    }
  }

  if (state == REFUSED && millis() - refusedStart > REFUSED_TIME) {
    state = READY;
    publishMsg(topic("state"), "READY", true);
    updateLeds();
  }
}

/* ================= RFID ================= */
String readUidString() {
  String uid = "";

  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) uid += ":";
  }

  uid.toUpperCase();
  return uid;
}

void handleRfid() {
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  String uid = readUidString();

  publishMsg(topic("rfid"), uid);
  publishMsg(topic("event"), "rfid_detected");

  int idx = findRfid(uid);

  if (idx < 0) {
    publishMsg(topic("rfid_result"), "UNKNOWN");
    setRefused("rfid_unknown");
  }

  else if (!rfidDb[idx].enabled) {
    publishMsg(topic("rfid_result"), "DISABLED");
    setRefused("rfid_disabled");
  }

  else if (!checkTimeAccess(rfidDb[idx].allowedFrom, rfidDb[idx].allowedTo)) {
    publishMsg(topic("rfid_result"), "TIME_DENIED");
    setRefused("rfid_time_denied");
  }

  else if (rfidDb[idx].maxUses > 0 && rfidDb[idx].usedCount >= rfidDb[idx].maxUses) {
    publishMsg(topic("rfid_result"), "USE_LIMIT_REACHED");
    setRefused("rfid_use_limit");
  }

  else if (!isDoorAllowedForCard(idx)) {
    publishMsg(topic("rfid_result"), "DOOR_NOT_ALLOWED");
    publishMsg(topic("event"), "rfid_door_not_allowed");
    setRefused("door_not_allowed");
  }

  else {
    rfidDb[idx].usedCount++;

    publishMsg(topic("rfid_result"), "AUTHORIZED");
    publishMsg(topic("rfid_consume"), uid);

    openDoor("RFID");
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}
/* ================= FINGERPRINT ================= */
void handleFinger() {
  uint8_t p = finger.getImage();

  if (p == FINGERPRINT_NOFINGER) return;
  if (p != FINGERPRINT_OK) return;
  if (finger.image2Tz() != FINGERPRINT_OK) return;

  p = finger.fingerSearch();

  if (p != FINGERPRINT_OK) {
    publishMsg(topic("fingerprint_result"), "UNKNOWN");
    setRefused("finger_unknown");
    return;
  }

  int id = finger.fingerID;

  publishMsg(topic("fingerprint"), String(id));
  publishMsg(topic("event"), "finger_detected");

  int idx = findFinger(id);

  if (idx < 0) {
    publishMsg(topic("fingerprint_result"), "UNKNOWN");
    setRefused("finger_not_in_db");
  }
  else if (!fingerDb[idx].enabled) {
    publishMsg(topic("fingerprint_result"), "DISABLED");
    setRefused("finger_disabled");
  }
  else if (!checkTimeAccess(fingerDb[idx].allowedFrom, fingerDb[idx].allowedTo)) {
    publishMsg(topic("fingerprint_result"), "TIME_DENIED");
    setRefused("finger_time_denied");
  }
  else if (fingerDb[idx].maxUses > 0 && fingerDb[idx].usedCount >= fingerDb[idx].maxUses) {
    publishMsg(topic("fingerprint_result"), "USE_LIMIT_REACHED");
    setRefused("finger_use_limit");
  }
  else if (!isDoorAllowedForFinger(idx)) {
    publishMsg(topic("fingerprint_result"), "DOOR_NOT_ALLOWED");
    publishMsg(topic("event"), "finger_door_not_allowed");
    setRefused("door_not_allowed");
  }
  else {
    fingerDb[idx].usedCount++;

    publishMsg(topic("fingerprint_result"), "AUTHORIZED");
    publishMsg(topic("fingerprint_consume"), String(id));

    openDoor("FINGER");
  }
}

/* ================= SENSORS / BUTTONS ================= */
void handleSensors() {
  publishMsg(topic("door"), digitalRead(MC38_PIN) == MC38_OPEN ? "OPEN" : "CLOSED", true);
  publishMsg(topic("pir"), digitalRead(PIR_PIN) == PIR_ACTIVE ? "DETECTED" : "CLEAR", true);

  if (millis() - lastDhtRead > DHT_INTERVAL) {
    lastDhtRead = millis();

    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (!isnan(t)) {
      lastTemp = t;
      publishMsg(topic("temp"), String(t, 1), true);
    }

    if (!isnan(h)) {
      lastHum = h;
      publishMsg(topic("hum"), String(h, 1), true);
    }
  }
}

void handleButtons() {
  if (digitalRead(BUTTON_OPEN) == LOW && millis() - lastButtonOpen > BUTTON_DEBOUNCE) {
    lastButtonOpen = millis();
    openDoor("BUTTON_OPEN");
  }

  if (digitalRead(BUTTON_CLOSE) == LOW && millis() - lastButtonClose > BUTTON_DEBOUNCE) {
    lastButtonClose = millis();

    if (state == OPEN) startWaitMode();
  }
}

/* ================= LOAD DATABASES ================= */
void loadRfidDb(String json) {
  StaticJsonDocument<16384> doc;

  if (deserializeJson(doc, json)) {
    publishMsg(topic("event"), "rfid_db_parse_error");
    return;
  }

  clearRfidDb();

  JsonArray arr = doc.as<JsonArray>();
  int i = 0;

  for (JsonObject o : arr) {
    if (i >= MAX_RFID_USERS) break;

    String uid = normalizeUid(o["uid"] | "");
    String name = o["name"] | "";

    if (uid.length() == 0) continue;

    String allowedDoors = "ALL";

    if (o["allowedDoors"].is<JsonArray>()) {
      allowedDoors = "";
      JsonArray doorsArr = o["allowedDoors"].as<JsonArray>();

      for (String d : doorsArr) {
        if (allowedDoors.length() > 0) allowedDoors += ",";
        allowedDoors += d;
      }
    }

    strncpy(rfidDb[i].uid, uid.c_str(), sizeof(rfidDb[i].uid) - 1);
    rfidDb[i].uid[sizeof(rfidDb[i].uid) - 1] = '\0';

    strncpy(rfidDb[i].name, name.c_str(), sizeof(rfidDb[i].name) - 1);
    rfidDb[i].name[sizeof(rfidDb[i].name) - 1] = '\0';

    strncpy(rfidDb[i].allowedDoors, allowedDoors.c_str(), sizeof(rfidDb[i].allowedDoors) - 1);
    rfidDb[i].allowedDoors[sizeof(rfidDb[i].allowedDoors) - 1] = '\0';

    rfidDb[i].enabled = o["enabled"] | true;
    rfidDb[i].allowedFrom = o["allowedFrom"] | 0UL;
    rfidDb[i].allowedTo = o["allowedTo"] | 0UL;
    rfidDb[i].maxUses = o["maxUses"] | 0;
    rfidDb[i].usedCount = o["usedCount"] | 0;
    rfidDb[i].used = true;

    i++;
  }

  publishMsg(topic("event"), "rfid_db_loaded");
}

void loadFingerDb(String json) {
  StaticJsonDocument<16384> doc;

  if (deserializeJson(doc, json)) {
    publishMsg(topic("event"), "finger_db_parse_error");
    return;
  }

  clearFingerDb();

  JsonArray arr = doc.as<JsonArray>();
  int i = 0;

  for (JsonObject o : arr) {
    if (i >= MAX_FINGER_USERS) break;

    int id = o["fingerId"] | -1;
    String name = o["name"] | "";

    if (id < 0) continue;

    String allowedDoors = "ALL";

    if (o["allowedDoors"].is<JsonArray>()) {
      allowedDoors = "";
      JsonArray doorsArr = o["allowedDoors"].as<JsonArray>();

      for (String d : doorsArr) {
        if (allowedDoors.length() > 0) allowedDoors += ",";
        allowedDoors += d;
      }
    }

    fingerDb[i].id = id;

    strncpy(fingerDb[i].name, name.c_str(), sizeof(fingerDb[i].name) - 1);
    fingerDb[i].name[sizeof(fingerDb[i].name) - 1] = '\0';

    strncpy(fingerDb[i].allowedDoors, allowedDoors.c_str(), sizeof(fingerDb[i].allowedDoors) - 1);
    fingerDb[i].allowedDoors[sizeof(fingerDb[i].allowedDoors) - 1] = '\0';

    fingerDb[i].enabled = o["enabled"] | true;
    fingerDb[i].allowedFrom = o["allowedFrom"] | 0UL;
    fingerDb[i].allowedTo = o["allowedTo"] | 0UL;
    fingerDb[i].maxUses = o["maxUses"] | 0;
    fingerDb[i].usedCount = o["usedCount"] | 0;
    fingerDb[i].used = true;

    i++;
  }

  publishMsg(topic("event"), "finger_db_loaded");
}

/* ================= ENROLL FINGER ================= */
void enrollFinger(int id) {
  publishMsg(topic("finger_enroll_status"), "put_finger", true);

  while (finger.getImage() != FINGERPRINT_OK) {
    client.loop();
    delay(50);
  }

  if (finger.image2Tz(1) != FINGERPRINT_OK) {
    publishMsg(topic("finger_enroll_result"), "ERROR:first_image", true);
    return;
  }

  publishMsg(topic("finger_enroll_status"), "remove_finger", true);
  delay(2000);

  while (finger.getImage() != FINGERPRINT_NOFINGER) {
    client.loop();
    delay(50);
  }

  publishMsg(topic("finger_enroll_status"), "put_same_finger", true);

  while (finger.getImage() != FINGERPRINT_OK) {
    client.loop();
    delay(50);
  }

  if (finger.image2Tz(2) != FINGERPRINT_OK) {
    publishMsg(topic("finger_enroll_result"), "ERROR:second_image", true);
    return;
  }

  if (finger.createModel() != FINGERPRINT_OK) {
    publishMsg(topic("finger_enroll_result"), "ERROR:create_model", true);
    return;
  }

  if (finger.storeModel(id) != FINGERPRINT_OK) {
    publishMsg(topic("finger_enroll_result"), "ERROR:store_model", true);
    return;
  }

  publishMsg(topic("finger_enroll_status"), "finger_saved", true);
  publishMsg(topic("finger_enroll_result"), "SUCCESS:" + String(id), true);
}

void deleteFinger(int id) {
  if (finger.deleteModel(id) == FINGERPRINT_OK) {
    publishMsg(topic("finger_enroll_result"), "DELETED:" + String(id), true);
  } else {
    publishMsg(topic("finger_enroll_result"), "ERROR:delete", true);
  }
}

void clearFingerSensor() {
  if (finger.emptyDatabase() == FINGERPRINT_OK) {
    publishMsg(topic("finger_enroll_result"), "CLEARED_ALL", true);
  } else {
    publishMsg(topic("finger_enroll_result"), "ERROR:clear_all", true);
  }
}

/* ================= MQTT CALLBACK ================= */
void mqttCallback(char* top, byte* payload, unsigned int length) {
  String msg = "";

  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  String t = String(top);

  if (t == topic("cmd")) {
    if (msg == "OPEN") openDoor("CMD");
    else if (msg == "CLOSE") {
      if (state == OPEN) startWaitMode();
    }
    else if (msg == "RESET") resetDoor();
  }

  else if (t == topic("lock")) {
    if (msg == "LOCKED") {
      relationLocked = true;
      publishMsg(topic("event"), "relation_locked");
      publishMsg(topic("lock_status"), "LOCKED", true);
      beepDouble();
      updateLeds();
    }
    else if (msg == "UNLOCKED") {
      relationLocked = false;
      publishMsg(topic("event"), "relation_unlocked");
      publishMsg(topic("lock_status"), "UNLOCKED", true);
      beepShort();
      updateLeds();
    }
  }

  else if (t == "sas/rfid/db") {
    loadRfidDb(msg);
  }

  else if (t == "sas/finger/db") {
    loadFingerDb(msg);
  }

  else if (t == topic("finger/cmd")) {
    if (msg.startsWith("ENROLL:")) enrollFinger(msg.substring(7).toInt());
    else if (msg.startsWith("DELETE:")) deleteFinger(msg.substring(7).toInt());
    else if (msg == "CLEAR_ALL") clearFingerSensor();
  }

  else if (t.startsWith("sas/") && t.endsWith("/state")) {
    if (t != topic("state")) {
      if (msg == "OPEN" || msg == "WAIT") otherDoorBusy = true;
      if (msg == "READY" || msg == "REFUSED") otherDoorBusy = false;
    }
  }
}

/* ================= WIFI / MQTT ================= */
void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    updateLeds();
  }
}

void reconnectMqtt() {
  while (!client.connected()) {
    updateLeds();

    String clientId = "SAS_" + String(DOOR_ID) + "_" + String((uint32_t)ESP.getEfuseMac(), HEX);

    if (client.connect(clientId.c_str())) {
      client.subscribe(topic("cmd").c_str());
      client.subscribe(topic("finger/cmd").c_str());
      client.subscribe(topic("lock").c_str());

      client.subscribe("sas/rfid/db");
      client.subscribe("sas/finger/db");
      client.subscribe("sas/+/state");

      publishMsg(topic("status"), "online", true);
      publishMsg(topic("event"), "boot", true);
      publishMsg(topic("lock_status"), relationLocked ? "LOCKED" : "UNLOCKED", true);

      publishAll();
    } else {
      delay(2000);
    }
  }
}

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);

  clearRfidDb();
  clearFingerDb();

  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_STATUS, OUTPUT);

  pinMode(BUZZER, OUTPUT);
  pinMode(RELAY, OUTPUT);

  pinMode(BUTTON_OPEN, INPUT_PULLUP);
  pinMode(BUTTON_CLOSE, INPUT_PULLUP);

  pinMode(MC38_PIN, INPUT);
  pinMode(PIR_PIN, INPUT);

  digitalWrite(RELAY, RELAY_OFF);
  digitalWrite(BUZZER, LOW);

  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_SS);
  rfid.PCD_Init();

  fingerSerial.begin(57600, SERIAL_8N1, FINGER_RX, FINGER_TX);
  finger.begin(57600);

  dht.begin();

  setupWifi();

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  client.setBufferSize(4096);

  reconnectMqtt();

  if (finger.verifyPassword()) {
    publishMsg(topic("event"), "finger_sensor_ok");
  } else {
    publishMsg(topic("event"), "finger_sensor_error");
  }

  updateLeds();
}

/* ================= LOOP ================= */
void loop() {
  if (WiFi.status() != WL_CONNECTED) setupWifi();
  if (!client.connected()) reconnectMqtt();

  client.loop();

  handleStateMachine();
  handleSensors();
  handleButtons();

  if (state == READY) {
    handleRfid();
    handleFinger();
  }

  if (millis() - lastHeartbeat > HEARTBEAT_TIME) {
    lastHeartbeat = millis();

    publishMsg(topic("status"), "online", true);
    publishAll();
    updateLeds();
  }
}