#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_Fingerprint.h>
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

/* ================= TOPICS ESP2 ================= */
#define TOPIC_SELF_CMD              "sas/esp2/cmd"
#define TOPIC_SELF_STATE            "sas/esp2/state"
#define TOPIC_SELF_DOOR             "sas/esp2/door"
#define TOPIC_SELF_EVENT            "sas/esp2/event"
#define TOPIC_SELF_STATUS           "sas/esp2/status"
#define TOPIC_SELF_FINGER           "sas/esp2/fingerprint"
#define TOPIC_SELF_FINGER_RESULT    "sas/esp2/fingerprint_result"
#define TOPIC_SELF_FINGER_CONSUME   "sas/esp2/fingerprint_consume"
#define TOPIC_SELF_AUTH             "sas/esp2/auth"
#define TOPIC_SELF_WAIT             "sas/esp2/wait"
#define TOPIC_SELF_PIR              "sas/esp2/pir"
#define TOPIC_SELF_BELL             "sas/esp2/bell"

/* ===== Enrollment / Sensor management ===== */
#define TOPIC_FINGER_DB             "sas/finger/db"
#define TOPIC_FINGER_SENSOR_CMD     "sas/esp2/finger_sensor/cmd"
#define TOPIC_FINGER_ENROLL_STATUS  "sas/esp2/finger_sensor/enroll_status"
#define TOPIC_FINGER_ENROLL_RESULT  "sas/esp2/finger_sensor/enroll_result"
#define TOPIC_NOTIFY                "sas/notify"

/* ================= TOPICS ESP1 ================= */
#define TOPIC_PEER_STATE            "sas/esp1/state"
#define TOPIC_PEER_DOOR             "sas/esp1/door"

/* ================= PINS ESP2 ================= */
#define LED_GREEN   25
#define LED_RED     27
#define BUTTON      13
#define MC38        34
#define FINGER_RX   16
#define FINGER_TX   17
#define PIR_PIN     32
#define PIR_LED     33
#define BELL_BUTTON 26

/* ================= LOGIC ================= */
#define GREEN_ON   HIGH
#define GREEN_OFF  LOW
#define RED_ON     HIGH
#define RED_OFF    LOW
#define MC38_CLOSED LOW
#define MC38_OPEN   HIGH
#define PIR_ACTIVE_STATE HIGH
#define PIR_IDLE_STATE   LOW

#define MAX_FINGER_USERS 50

struct FingerUser {
  int fingerId;
  char name[32];
  bool enabled;
  unsigned long allowedFrom;
  unsigned long allowedTo;
  int maxUses;
  int usedCount;
  bool used;
};

FingerUser fingerDb[MAX_FINGER_USERS];
int fingerDbCount = 0;

HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

enum State { INIT, OPEN, WAIT, REFUSED };
State state = INIT;

const unsigned long WAIT_TIME = 4000;
const unsigned long MC38_DEBOUNCE_MS = 300;
const unsigned long REFUSE_DISPLAY_MS = 1500;
const unsigned long BUTTON_DEBOUNCE_MS = 300;
const unsigned long FINGER_RETRY_MS = 1500;
const unsigned long FINGER_SUCCESS_GUARD_MS = 1200;
const unsigned long PIR_DEBOUNCE_MS = 300;
const unsigned long PIR_WARMUP_MS = 30000;
const unsigned long BELL_DEBOUNCE_MS = 50;

unsigned long timerWait = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastButtonTime = 0;
unsigned long lastDoorChange = 0;
unsigned long refuseUntil = 0;
unsigned long lastFingerRejectTime = 0;
unsigned long lastFingerSuccessTime = 0;
unsigned long lastPirChange = 0;
unsigned long pirStartupTime = 0;
unsigned long lastBellChange = 0;

int stableDoorState = MC38_CLOSED;
int previousDoorState = MC38_CLOSED;
int pirStableState = PIR_IDLE_STATE;
int pirLastRaw = PIR_IDLE_STATE;

bool pirDetected = false;
bool pirReady = false;
bool bellPressedState = false;

String peerState = "READY";
String peerDoor  = "CLOSED";

/* ================= ENROLL STATE ================= */
bool enrollRequested = false;
bool enrollInProgress = false;
int enrollFingerId = -1;

/* ================= PROTOTYPES ================= */
void publishMessage(const char* topic, const String& payload, bool retained = false);
void publishNotification(const String& type, const String& id, const String& name, const String& message);

/* ================= HELPERS ================= */
void clearFingerDb() {
  rfidDbCount:
  fingerDbCount = 0;
  for (int i = 0; i < MAX_FINGER_USERS; i++) {
    fingerDb[i].fingerId = -1;
    fingerDb[i].name[0] = '\0';
    fingerDb[i].enabled = false;
    fingerDb[i].allowedFrom = 0;
    fingerDb[i].allowedTo = 0;
    fingerDb[i].maxUses = 0;
    fingerDb[i].usedCount = 0;
    fingerDb[i].used = false;
  }
}

int findFingerIndex(int fingerId) {
  for (int i = 0; i < MAX_FINGER_USERS; i++) {
    if (!fingerDb[i].used) continue;
    if (fingerDb[i].fingerId == fingerId) return i;
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

void setGreen(bool on) { digitalWrite(LED_GREEN, on ? GREEN_ON : GREEN_OFF); }
void setRed(bool on)   { digitalWrite(LED_RED, on ? RED_ON : RED_OFF); }

String stateToString() {
  switch (state) {
    case INIT: return "READY";
    case OPEN: return "OPEN";
    case WAIT: return "WAIT";
    case REFUSED: return "REFUSED";
    default: return "UNKNOWN";
  }
}

bool localDoorSensorOpen() { return stableDoorState == MC38_OPEN; }
bool localDoorShownOpen()  { return (state == OPEN) || localDoorSensorOpen(); }
bool peerDoorShownOpen()   { return (peerState == "OPEN") || (peerDoor == "OPEN"); }
bool peerBusy()            { return (peerState != "READY") || peerDoorShownOpen(); }
bool globalWaitActive()    { return (state == WAIT) || (peerState == "WAIT"); }

void publishMessage(const char* topic, const String& payload, bool retained) {
  if (!client.connected()) return;
  client.publish(topic, payload.c_str(), retained);
}

void publishNotification(const String& type, const String& id, const String& name, const String& message) {
  StaticJsonDocument<256> doc;
  doc["type"] = type;
  doc["id"] = id;
  doc["name"] = name;
  doc["message"] = message;
  char buffer[256];
  serializeJson(doc, buffer);
  publishMessage(TOPIC_NOTIFY, buffer);
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

void publishPirState(bool detected) {
  publishMessage(TOPIC_SELF_PIR, detected ? "DETECTED" : "CLEAR", true);
}

void publishBellState(bool pressed) {
  publishMessage(TOPIC_SELF_BELL, pressed ? "ON" : "OFF", true);
}

void updatePirLed() {
  digitalWrite(PIR_LED, pirDetected ? HIGH : LOW);
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

void setRefused(const String& reason) {
  state = REFUSED;
  refuseUntil = millis();
  updateLEDs();
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
  updateLEDs();
  publishState();
  publishDoorShownState();
  publishWaitValue();
  publishMessage(TOPIC_SELF_AUTH, src, true);
  publishMessage(TOPIC_SELF_EVENT, "door2_opened");
}

void startWaitMode() {
  state = WAIT;
  timerWait = millis();
  updateLEDs();
  publishState();
  publishDoorShownState();
  publishWaitValue();
  publishMessage(TOPIC_SELF_EVENT, "door2_closed_wait");
}

bool loadFingerDbFromJson(const String& json) {
  StaticJsonDocument<16384> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    publishMessage(TOPIC_SELF_EVENT, "finger_db_parse_error");
    return false;
  }
  if (!doc.is<JsonArray>()) {
    publishMessage(TOPIC_SELF_EVENT, "finger_db_invalid_format");
    return false;
  }

  clearFingerDb();
  JsonArray arr = doc.as<JsonArray>();
  int idx = 0;

  for (JsonObject item : arr) {
    if (idx >= MAX_FINGER_USERS) break;

    int fingerId = item["fingerId"] | -1;
    String name = item["name"] | "";
    bool enabled = item["enabled"] | false;
    unsigned long allowedFrom = item["allowedFrom"] | 0UL;
    unsigned long allowedTo   = item["allowedTo"]   | 0UL;
    int maxUses = item["maxUses"] | 0;
    int usedCount = item["usedCount"] | 0;

    name.trim();
    if (fingerId < 0) continue;

    fingerDb[idx].fingerId = fingerId;
    strncpy(fingerDb[idx].name, name.c_str(), sizeof(fingerDb[idx].name) - 1);
    fingerDb[idx].name[sizeof(fingerDb[idx].name) - 1] = '\0';
    fingerDb[idx].enabled = enabled;
    fingerDb[idx].allowedFrom = allowedFrom;
    fingerDb[idx].allowedTo = allowedTo;
    fingerDb[idx].maxUses = maxUses;
    fingerDb[idx].usedCount = usedCount;
    fingerDb[idx].used = true;
    idx++;
  }

  fingerDbCount = idx;
  publishMessage(TOPIC_SELF_EVENT, "finger_db_loaded");
  return true;
}

/* ================= ENROLL ================= */
bool waitFingerImage(uint16_t timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    client.loop();
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_OK) return true;
    delay(50);
  }
  return false;
}

bool waitFingerRemoved(uint16_t timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    client.loop();
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) return true;
    delay(50);
  }
  return false;
}

void doEnrollFingerprint(int id) {
  enrollInProgress = true;

  if (!finger.verifyPassword()) {
    publishMessage(TOPIC_FINGER_ENROLL_STATUS, "capteur non detecte", true);
    publishMessage(TOPIC_FINGER_ENROLL_RESULT, "ERROR:sensor_not_found", true);
    enrollInProgress = false;
    return;
  }

  publishMessage(TOPIC_FINGER_ENROLL_STATUS, "mettre votre doigt sur le capteur", true);

  if (!waitFingerImage(15000)) {
    publishMessage(TOPIC_FINGER_ENROLL_RESULT, "ERROR:timeout_first", true);
    enrollInProgress = false;
    return;
  }

  if (finger.image2Tz(1) != FINGERPRINT_OK) {
    publishMessage(TOPIC_FINGER_ENROLL_RESULT, "ERROR:first_convert", true);
    enrollInProgress = false;
    return;
  }

  publishMessage(TOPIC_FINGER_ENROLL_STATUS, "retirer votre doigt", true);

  if (!waitFingerRemoved(10000)) {
    publishMessage(TOPIC_FINGER_ENROLL_RESULT, "ERROR:remove_timeout", true);
    enrollInProgress = false;
    return;
  }

  publishMessage(TOPIC_FINGER_ENROLL_STATUS, "mettre le meme doigt a nouveau", true);

  if (!waitFingerImage(15000)) {
    publishMessage(TOPIC_FINGER_ENROLL_RESULT, "ERROR:timeout_second", true);
    enrollInProgress = false;
    return;
  }

  if (finger.image2Tz(2) != FINGERPRINT_OK) {
    publishMessage(TOPIC_FINGER_ENROLL_RESULT, "ERROR:second_convert", true);
    enrollInProgress = false;
    return;
  }

  if (finger.createModel() != FINGERPRINT_OK) {
    publishMessage(TOPIC_FINGER_ENROLL_RESULT, "ERROR:create_model", true);
    enrollInProgress = false;
    return;
  }

  if (finger.storeModel(id) != FINGERPRINT_OK) {
    publishMessage(TOPIC_FINGER_ENROLL_RESULT, "ERROR:store_model", true);
    enrollInProgress = false;
    return;
  }

  publishMessage(TOPIC_FINGER_ENROLL_STATUS, "empreinte enregistree", true);
  publishMessage(TOPIC_FINGER_ENROLL_RESULT, "SUCCESS:" + String(id), true);

  enrollInProgress = false;
}

void doDeleteFingerprint(int id) {
  uint8_t p = finger.deleteModel(id);
  if (p == FINGERPRINT_OK) {
    publishMessage(TOPIC_FINGER_ENROLL_RESULT, "DELETED:" + String(id), true);
  } else {
    publishMessage(TOPIC_FINGER_ENROLL_RESULT, "ERROR:delete_" + String(id), true);
  }
}

void doClearAllFingerprints() {
  uint8_t p = finger.emptyDatabase();
  if (p == FINGERPRINT_OK) {
    publishMessage(TOPIC_FINGER_ENROLL_STATUS, "base capteur videe", true);
    publishMessage(TOPIC_FINGER_ENROLL_RESULT, "CLEARED_ALL", true);
    publishMessage(TOPIC_SELF_EVENT, "finger_sensor_cleared");
  } else {
    publishMessage(TOPIC_FINGER_ENROLL_STATUS, "erreur vidage capteur", true);
    publishMessage(TOPIC_FINGER_ENROLL_RESULT, "ERROR:clear_all", true);
  }
}

/* ================= ACCESS ================= */
int getFingerprintId() {
  uint8_t p = finger.getImage();
  if (p == FINGERPRINT_NOFINGER) return -1;
  if (p != FINGERPRINT_OK) return -1;

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return 0;

  p = finger.fingerSearch();
  if (p == FINGERPRINT_OK) {
    lastFingerSuccessTime = millis();
    return finger.fingerID;
  }

  if (millis() - lastFingerSuccessTime > FINGER_SUCCESS_GUARD_MS) {
    publishMessage(TOPIC_SELF_EVENT, "finger_not_recognized");
  }
  return 0;
}

void processFingerprintAccess(int fingerId) {
  publishMessage(TOPIC_SELF_FINGER, String(fingerId));
  publishMessage(TOPIC_SELF_EVENT, "finger_detected");

  int idx = findFingerIndex(fingerId);

  if (idx < 0) {
    publishMessage(TOPIC_SELF_FINGER_RESULT, "UNKNOWN");
    publishNotification("finger_denied", String(fingerId), "", "Personne inconnue");
    setRefused("finger_unknown");
    return;
  }

  if (!fingerDb[idx].enabled) {
    publishMessage(TOPIC_SELF_FINGER_RESULT, "DISABLED");
    publishNotification("finger_denied", String(fingerId), String(fingerDb[idx].name), "Empreinte désactivée");
    setRefused("finger_disabled");
    return;
  }

  if (!isTimeSynced()) {
    publishMessage(TOPIC_SELF_FINGER_RESULT, "TIME_NOT_SYNCED");
    publishNotification("finger_denied", String(fingerId), String(fingerDb[idx].name), "Heure non synchronisée");
    setRefused("finger_time_not_synced");
    return;
  }

  unsigned long nowEpoch = getNowEpoch();

  if (fingerDb[idx].allowedFrom > 0 && nowEpoch < fingerDb[idx].allowedFrom) {
    publishMessage(TOPIC_SELF_FINGER_RESULT, "TOO_EARLY");
    publishNotification("finger_denied", String(fingerId), String(fingerDb[idx].name), "Accès pas encore autorisé");
    setRefused("finger_too_early");
    return;
  }

  if (fingerDb[idx].allowedTo > 0 && nowEpoch > fingerDb[idx].allowedTo) {
    publishMessage(TOPIC_SELF_FINGER_RESULT, "EXPIRED");
    publishNotification("finger_denied", String(fingerId), String(fingerDb[idx].name), "Autorisation expirée");
    setRefused("finger_expired");
    return;
  }

  if (fingerDb[idx].maxUses > 0 && fingerDb[idx].usedCount >= fingerDb[idx].maxUses) {
    publishMessage(TOPIC_SELF_FINGER_RESULT, "USE_LIMIT_REACHED");
    publishNotification("finger_denied", String(fingerId), String(fingerDb[idx].name), "Nombre d'utilisations dépassé");
    setRefused("finger_use_limit");
    return;
  }

  if (canOpenDoor()) {
    fingerDb[idx].usedCount++;
    publishMessage(TOPIC_SELF_FINGER_RESULT, "AUTHORIZED");
    publishMessage(TOPIC_SELF_FINGER_CONSUME, String(fingerId));
    publishNotification("finger_ok", String(fingerId), String(fingerDb[idx].name), "Bienvenue " + String(fingerDb[idx].name));
    openDoor("FINGER");
  }
}

bool buttonPressed() {
  if (digitalRead(BUTTON) == LOW && millis() - lastButtonTime > BUTTON_DEBOUNCE_MS) {
    lastButtonTime = millis();
    return true;
  }
  return false;
}

void handlePir() {
  if (!pirReady) {
    if (millis() - pirStartupTime >= PIR_WARMUP_MS) {
      pirReady = true;
      pirLastRaw = digitalRead(PIR_PIN);
      pirStableState = pirLastRaw;
      pirDetected = (pirStableState == PIR_ACTIVE_STATE);
      updatePirLed();
      publishPirState(pirDetected);
    }
    return;
  }

  int pirRaw = digitalRead(PIR_PIN);
  if (pirRaw != pirLastRaw) {
    pirLastRaw = pirRaw;
    lastPirChange = millis();
  }

  if (millis() - lastPirChange >= PIR_DEBOUNCE_MS) {
    if (pirStableState != pirLastRaw) {
      pirStableState = pirLastRaw;
      pirDetected = (pirStableState == PIR_ACTIVE_STATE);
      updatePirLed();
      if (pirDetected) {
        publishPirState(true);
        publishMessage(TOPIC_SELF_EVENT, "pir_detected");
      } else {
        publishPirState(false);
        publishMessage(TOPIC_SELF_EVENT, "pir_clear");
      }
    }
  }
}

void handleBellButton() {
  bool rawPressed = (digitalRead(BELL_BUTTON) == LOW);
  if (rawPressed != bellPressedState && millis() - lastBellChange >= BELL_DEBOUNCE_MS) {
    lastBellChange = millis();
    bellPressedState = rawPressed;
    if (bellPressedState) {
      publishBellState(true);
      publishMessage(TOPIC_SELF_EVENT, "bell_on");
    } else {
      publishBellState(false);
      publishMessage(TOPIC_SELF_EVENT, "bell_off");
    }
  }
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

void callback(char* topic, byte* payload, unsigned int length) {
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
  else if (t == TOPIC_FINGER_DB) {
    loadFingerDbFromJson(msg);
  }
  else if (t == TOPIC_FINGER_SENSOR_CMD) {
    if (msg.startsWith("ENROLL:")) {
      enrollFingerId = msg.substring(7).toInt();
      if (enrollFingerId > 0) {
        enrollRequested = true;
      }
    } else if (msg.startsWith("DELETE:")) {
      int id = msg.substring(7).toInt();
      if (id > 0) doDeleteFingerprint(id);
    } else if (msg == "CLEAR_ALL") {
      doClearAllFingerprints();
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
      publishState();
      publishDoorShownState();
      publishWaitValue();
      publishMessage(TOPIC_SELF_EVENT, "reset_done");
    }
  }
}

void reconnect() {
  while (!client.connected()) {
    String clientId = "ESP32_P2_";
    clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

    if (client.connect(clientId.c_str())) {
      client.subscribe(TOPIC_SELF_CMD);
      client.subscribe(TOPIC_PEER_STATE);
      client.subscribe(TOPIC_PEER_DOOR);
      client.subscribe(TOPIC_FINGER_DB);
      client.subscribe(TOPIC_FINGER_SENSOR_CMD);

      publishMessage(TOPIC_SELF_STATUS, "online", true);
      publishState();
      publishDoorShownState();
      publishWaitValue();
      publishPirState(pirDetected);
      publishBellState(bellPressedState);
      publishMessage(TOPIC_SELF_EVENT, "boot", true);
      publishMessage(TOPIC_SELF_EVENT, "finger_db_waiting");
    } else {
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);

  clearFingerDb();

  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);
  pinMode(BELL_BUTTON, INPUT_PULLUP);
  pinMode(MC38, INPUT);
  pinMode(PIR_PIN, INPUT);
  pinMode(PIR_LED, OUTPUT);

  setGreen(false);
  setRed(false);
  pirDetected = false;
  pirReady = false;
  bellPressedState = false;
  digitalWrite(PIR_LED, LOW);

  fingerSerial.begin(57600, SERIAL_8N1, FINGER_RX, FINGER_TX);
  finger.begin(57600);

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
  client.setBufferSize(4096);

  stableDoorState = digitalRead(MC38);
  previousDoorState = stableDoorState;
  pirStartupTime = millis();
  pirLastRaw = digitalRead(PIR_PIN);
  pirStableState = pirLastRaw;

  state = INIT;
  updateLEDs();

  setup_wifi();
  setupTimeSync();
  reconnect();

  if (finger.verifyPassword()) {
    publishMessage(TOPIC_SELF_EVENT, "finger_sensor_ok");
  } else {
    publishMessage(TOPIC_SELF_EVENT, "finger_sensor_error");
  }

  publishDoorShownState();
  publishWaitValue();
  publishPirState(false);
  publishBellState(false);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) setup_wifi();
  if (!client.connected()) reconnect();
  client.loop();

  handlePir();
  handleBellButton();

  if (enrollRequested && !enrollInProgress) {
    enrollRequested = false;
    doEnrollFingerprint(enrollFingerId);
    return;
  }

  if (enrollInProgress) {
    client.loop();
    delay(20);
    return;
  }

  if (millis() - lastHeartbeat > 5000) {
    lastHeartbeat = millis();
    publishMessage(TOPIC_SELF_STATUS, "online", true);
    publishState();
    publishDoorShownState();
    publishWaitValue();
    publishPirState(pirDetected);
    publishBellState(bellPressedState);
    updateLEDs();
  }

  int rawDoorState = digitalRead(MC38);
  if (rawDoorState != stableDoorState && millis() - lastDoorChange > MC38_DEBOUNCE_MS) {
    previousDoorState = stableDoorState;
    stableDoorState = rawDoorState;
    lastDoorChange = millis();
    publishDoorShownState();

    if (previousDoorState == MC38_OPEN && stableDoorState == MC38_CLOSED) {
      if (state != WAIT) startWaitMode();
    }
    updateLEDs();
  }

  /* IMPORTANT : lecture normale seulement hors enrôlement */
  if (!enrollInProgress) {
    int fingerId = getFingerprintId();
    if (fingerId > 0) {
      processFingerprintAccess(fingerId);
    } else if (fingerId == 0) {
      if (state == INIT &&
          millis() - lastFingerRejectTime > FINGER_RETRY_MS &&
          millis() - lastFingerSuccessTime > FINGER_SUCCESS_GUARD_MS) {
        lastFingerRejectTime = millis();
        publishNotification("finger_denied", "", "", "Personne inconnue");
        setRefused("finger_not_recognized");
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
}