#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <MFRC522.h>
#include <Adafruit_Fingerprint.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Preferences.h>

/* =========================================================
   WIFI
   ========================================================= */
const char* ssid = "Lenovo16";
const char* password = "Lenovo16";

/* =========================================================
   MQTT
   ========================================================= */
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);
Preferences prefs;

/* =========================================================
   SPI PRINCIPAL : 2 RFID
   ========================================================= */
#define RFID_SCK       18
#define RFID_MISO      19
#define RFID_MOSI      23

#define RFID_IN_SS      5
#define RFID_OUT_SS    27
#define RFID_RST       22

MFRC522 rfidIn(RFID_IN_SS, RFID_RST);
MFRC522 rfidOut(RFID_OUT_SS, RFID_RST);

/* =========================================================
   SPI SEPARE : CARTE SD
   ========================================================= */
#define SD_SCK         26
#define SD_MISO        35
#define SD_MOSI        13
#define SD_CS          15

SPIClass spiSD(HSPI);

bool sdReady = false;

const char* LOG_FILE = "/log.txt";
const char* ACCESS_HISTORY_FILE = "/access_history.csv";
const char* RFID_FILE = "/rfid_db.json";
const char* FINGER_FILE = "/finger_db.json";
const char* CONFIG_FILE = "/config.json";

/* =========================================================
   IDENTIFIANTS ESP / PORTE
   ========================================================= */
String deviceId = "";
String doorId = "";

String makeDeviceId() {
  return "esp_" + String((uint32_t)ESP.getEfuseMac(), HEX);
}

String baseTopic() {
  return "sas/" + doorId + "/";
}

String topic(String sub) {
  return baseTopic() + sub;
}

/* =========================================================
   NTP
   ========================================================= */
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";

const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 0;

/* =========================================================
   PINS
   ========================================================= */
#define LED_GREEN      25
#define LED_RED        12
#define LED_STATUS     21

/*
   GPIO36 = SP / VP
   GPIO39 = SN / VN

   Résistance externe de 10 kΩ vers 3.3 V obligatoire.
   Bouton entre GPIO et GND.
*/
#define BUTTON_OPEN    36
#define BUTTON_CLOSE   39

#define BUZZER          4
#define RELAY          14

/*
   GPIO34 nécessite aussi une résistance externe de 10 kΩ
   vers 3.3 V pour le contact MC38.
*/
#define MC38_PIN       34
#define PIR_PIN        33

#define DHTPIN         32
#define DHTTYPE        DHT11

#define FINGER_RX      16
#define FINGER_TX      17

HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger(&fingerSerial);
DHT dht(DHTPIN, DHTTYPE);

/* =========================================================
   LOGIQUE ELECTRIQUE
   ========================================================= */
#define MC38_CLOSED LOW
#define MC38_OPEN   HIGH

#define PIR_ACTIVE  HIGH

#define RELAY_ON    HIGH
#define RELAY_OFF   LOW

/* =========================================================
   TEMPS
   ========================================================= */
const unsigned long WAIT_TIME = 4000;
const unsigned long RELAY_TIME = 1200;
const unsigned long REFUSED_TIME = 1500;
const unsigned long BUTTON_DEBOUNCE = 80;
const unsigned long DHT_INTERVAL = 3000;
const unsigned long HEARTBEAT_TIME = 5000;
const unsigned long MQTT_RECONNECT_TIME = 3000;
const unsigned long WIFI_RECONNECT_TIME = 10000;

/* =========================================================
   ETAT
   ========================================================= */
enum State {
  READY,
  OPEN,
  WAIT,
  REFUSED
};

State state = READY;

unsigned long waitStart = 0;
unsigned long relayStart = 0;
unsigned long refusedStart = 0;
unsigned long lastDhtRead = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastMqttReconnect = 0;
unsigned long lastWifiReconnect = 0;

long lastWaitRemain = -2;

bool relayActive = false;
bool otherDoorBusy = false;
bool relationLocked = false;
bool doorWasPhysicallyOpen = false;

float lastTemp = NAN;
float lastHum = NAN;

/* =========================================================
   ANTIREBOND BOUTONS
   ========================================================= */
int openStableState = HIGH;
int openCandidateState = HIGH;
unsigned long openCandidateTime = 0;

int closeStableState = HIGH;
int closeCandidateState = HIGH;
unsigned long closeCandidateTime = 0;

/* =========================================================
   BASES DE DONNEES
   ========================================================= */
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

/* =========================================================
   CARTE SD
   ========================================================= */
void logSD(const String& text) {
  if (!sdReady) return;

  File f = SD.open(LOG_FILE, FILE_APPEND);

  if (!f) {
    Serial.println("Erreur ouverture log.txt");
    return;
  }

  f.print(millis());
  f.print(" | ");
  f.println(text);
  f.close();
}

String getDateTimeString() {
  struct tm timeInfo;

  if (!getLocalTime(&timeInfo, 100)) {
    return "NON_SYNCHRONISE_" + String(millis());
  }

  char buffer[25];

  strftime(
    buffer,
    sizeof(buffer),
    "%Y-%m-%d %H:%M:%S",
    &timeInfo
  );

  return String(buffer);
}

String csvSafe(String value) {
  value.replace(";", ",");
  value.replace("\r", " ");
  value.replace("\n", " ");
  return value;
}

void logAccessHistory(
  const String& direction,
  const String& method,
  const String& identifier,
  const String& personName,
  const String& result = "AUTHORIZED"
) {
  if (!sdReady) {
    Serial.println("Historique non enregistre : SD absente");
    return;
  }

  bool newFile = !SD.exists(ACCESS_HISTORY_FILE);

  File f = SD.open(ACCESS_HISTORY_FILE, FILE_APPEND);

  if (!f) {
    Serial.println("Erreur ouverture access_history.csv");
    return;
  }

  if (newFile) {
    f.println("Nom;Date;Heure;Direction;Methode;Identifiant;Resultat");
  }

  struct tm timeInfo;

  String dateText = "DATE_INCONNUE";
  String heureText = "HEURE_INCONNUE";

  if (getLocalTime(&timeInfo, 100)) {
    char dateBuffer[15];
    char heureBuffer[15];

    strftime(
      dateBuffer,
      sizeof(dateBuffer),
      "%d/%m/%Y",
      &timeInfo
    );

    strftime(
      heureBuffer,
      sizeof(heureBuffer),
      "%H:%M:%S",
      &timeInfo
    );

    dateText = String(dateBuffer);
    heureText = String(heureBuffer);
  }

  String safeName = personName;
  safeName.replace(";", ",");

  f.print(safeName);
  f.print(";");

  f.print(dateText);
  f.print(";");

  f.print(heureText);
  f.print(";");

  f.print(direction);
  f.print(";");

  f.print(method);
  f.print(";");

  f.print(identifier);
  f.print(";");

  f.println(result);

  f.close();

  Serial.println(
    safeName +
    " | " +
    dateText +
    " | " +
    heureText +
    " | " +
    direction
  );
}

bool writeFileSD(const char* path, const String& data) {
  if (!sdReady) return false;

  if (SD.exists(path)) {
    SD.remove(path);
  }

  File f = SD.open(path, FILE_WRITE);

  if (!f) {
    Serial.println("Erreur ecriture SD : " + String(path));
    return false;
  }

  f.print(data);
  f.close();

  Serial.println("Fichier sauvegarde : " + String(path));
  return true;
}

String readFileSD(const char* path) {
  if (!sdReady) return "";

  File f = SD.open(path, FILE_READ);

  if (!f) {
    Serial.println("Fichier absent : " + String(path));
    return "";
  }

  String data = f.readString();
  f.close();

  return data;
}

void saveConfigToSD() {
  if (!sdReady) return;

  StaticJsonDocument<512> doc;

  doc["deviceId"] = deviceId;
  doc["doorId"] = doorId;

  String out;
  serializeJson(doc, out);

  writeFileSD(CONFIG_FILE, out);
}

void initSD() {
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);

  spiSD.begin(
    SD_SCK,
    SD_MISO,
    SD_MOSI,
    SD_CS
  );

  delay(100);

  if (!SD.begin(SD_CS, spiSD, 4000000)) {
    sdReady = false;
    Serial.println("SD ERROR sur SPI separe");
    return;
  }

  uint8_t cardType = SD.cardType();

  if (cardType == CARD_NONE) {
    sdReady = false;
    Serial.println("Aucune carte SD detectee");
    return;
  }

  sdReady = true;

  Serial.println("SD OK sur SPI separe");

  Serial.print("Type carte SD : ");

  if (cardType == CARD_MMC) {
    Serial.println("MMC");
  }
  else if (cardType == CARD_SD) {
    Serial.println("SDSC");
  }
  else if (cardType == CARD_SDHC) {
    Serial.println("SDHC");
  }
  else {
    Serial.println("INCONNU");
  }

  uint64_t cardSizeMB =
    SD.cardSize() / (1024ULL * 1024ULL);

  Serial.print("Taille carte : ");
  Serial.print(cardSizeMB);
  Serial.println(" MB");

  File f = SD.open(LOG_FILE, FILE_APPEND);

  if (f) {
    f.println("===== ESP START =====");
    f.println("Device ID: " + deviceId);
    f.println("Door ID: " + doorId);
    f.println("Date: " + getDateTimeString());
    f.close();
  }

  saveConfigToSD();
}

/* =========================================================
   MQTT
   ========================================================= */
void publishMsg(
  const String& t,
  const String& msg,
  bool retained = false
) {
  if (client.connected()) {
    client.publish(
      t.c_str(),
      msg.c_str(),
      retained
    );
  }
}

String stateToString() {
  switch (state) {
    case READY: return "READY";
    case OPEN: return "OPEN";
    case WAIT: return "WAIT";
    case REFUSED: return "REFUSED";
    default: return "UNKNOWN";
  }
}

/* =========================================================
   LEDS / BUZZER
   ========================================================= */
void updateLeds() {
  bool networkOk =
    WiFi.status() == WL_CONNECTED &&
    client.connected();

  digitalWrite(
    LED_STATUS,
    networkOk ? HIGH : LOW
  );

  if (relationLocked) {
    digitalWrite(LED_GREEN, LOW);
    digitalWrite(LED_RED, HIGH);
    return;
  }

  if (state == READY || state == OPEN) {
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_RED, LOW);
  }
  else {
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

/* =========================================================
   INITIALISATION BASES
   ========================================================= */
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

    if (
      (c >= '0' && c <= '9') ||
      (c >= 'A' && c <= 'F')
    ) {
      out += c;
    }
  }

  return out;
}

int findRfid(const String& uid) {
  String normalized = normalizeUid(uid);

  for (int i = 0; i < MAX_RFID_USERS; i++) {
    if (!rfidDb[i].used) continue;

    if (
      normalizeUid(String(rfidDb[i].uid)) ==
      normalized
    ) {
      return i;
    }
  }

  return -1;
}

int findFinger(int id) {
  for (int i = 0; i < MAX_FINGER_USERS; i++) {
    if (!fingerDb[i].used) continue;

    if (fingerDb[i].id == id) {
      return i;
    }
  }

  return -1;
}

/* =========================================================
   TEMPS
   ========================================================= */
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

bool checkTimeAccess(
  unsigned long from,
  unsigned long to
) {
  if (!isTimeSynced()) {
    return true;
  }

  unsigned long now = nowEpoch();

  if (from > 0 && now < from) {
    return false;
  }

  if (to > 0 && now > to) {
    return false;
  }

  return true;
}

/* =========================================================
   PORTES AUTORISEES
   ========================================================= */
bool isDoorAllowedForCard(int idx) {
  String doors =
    String(rfidDb[idx].allowedDoors);

  doors.trim();

  if (doors.length() == 0) return true;
  if (doors.indexOf("ALL") >= 0) return true;

  return doors.indexOf(doorId) >= 0;
}

bool isDoorAllowedForFinger(int idx) {
  String doors =
    String(fingerDb[idx].allowedDoors);

  doors.trim();

  if (doors.length() == 0) return true;
  if (doors.indexOf("ALL") >= 0) return true;

  return doors.indexOf(doorId) >= 0;
}

/* =========================================================
   PUBLICATION ETAT
   ========================================================= */
void publishAll() {
  publishMsg(
    topic("state"),
    stateToString(),
    true
  );

  publishMsg(
    topic("door"),
    digitalRead(MC38_PIN) == MC38_OPEN
      ? "OPEN"
      : "CLOSED",
    true
  );

  publishMsg(
    topic("pir"),
    digitalRead(PIR_PIN) == PIR_ACTIVE
      ? "DETECTED"
      : "CLEAR",
    true
  );

  publishMsg(
    topic("lock_status"),
    relationLocked
      ? "LOCKED"
      : "UNLOCKED",
    true
  );

  publishMsg(
    topic("sd_status"),
    sdReady ? "OK" : "ERROR",
    true
  );

  publishMsg(
    "sas/" + deviceId + "/info",
    doorId,
    true
  );

  publishMsg(
    "sas/" + deviceId + "/status",
    WiFi.status() == WL_CONNECTED
      ? "online"
      : "offline",
    true
  );

  if (!isnan(lastTemp)) {
    publishMsg(
      topic("temp"),
      String(lastTemp, 1),
      true
    );
  }

  if (!isnan(lastHum)) {
    publishMsg(
      topic("hum"),
      String(lastHum, 1),
      true
    );
  }
}

/* =========================================================
   LOGIQUE PORTE
   ========================================================= */
bool canOpenDoor() {
  if (relationLocked) {
    publishMsg(
      topic("event"),
      "refused_relation_locked"
    );

    return false;
  }

  if (state != READY) {
    return false;
  }

  if (digitalRead(MC38_PIN) == MC38_OPEN) {
    publishMsg(
      topic("event"),
      "refused_door_already_open"
    );

    return false;
  }

  if (otherDoorBusy) {
    publishMsg(
      topic("event"),
      "refused_other_door_busy"
    );

    return false;
  }

  return true;
}

void setRefused(const String& reason) {
  state = REFUSED;
  refusedStart = millis();

  publishMsg(
    topic("state"),
    "REFUSED",
    true
  );

  publishMsg(
    topic("event"),
    "open_refused_" + reason
  );

  logSD("REFUSED: " + reason);

  updateLeds();
  beepDouble();
}

bool openDoor(const String& src) {
  if (!canOpenDoor()) {
    setRefused(src);
    return false;
  }

  doorWasPhysicallyOpen = false;
  state = OPEN;
  relayActive = true;
  relayStart = millis();

  digitalWrite(RELAY, RELAY_ON);

  publishMsg(
    topic("state"),
    "OPEN",
    true
  );

  publishMsg(
    topic("auth"),
    src,
    true
  );

  publishMsg(
    topic("event"),
    "door_opened_by_" + src
  );

  logSD("DOOR OPEN: " + src);

  updateLeds();
  beepShort();

  return true;
}

void startWaitMode() {
  digitalWrite(RELAY, RELAY_OFF);

  relayActive = false;
  state = WAIT;
  waitStart = millis();
  lastWaitRemain = -2;

  publishMsg(
    topic("state"),
    "WAIT",
    true
  );

  publishMsg(
    topic("event"),
    "door_closed_wait"
  );

  logSD("DOOR CLOSED -> WAIT");

  updateLeds();
  beepDouble();
}

void resetDoor() {
  digitalWrite(RELAY, RELAY_OFF);

  relayActive = false;
  doorWasPhysicallyOpen = false;
  state = READY;
  lastWaitRemain = -2;

  publishMsg(
    topic("state"),
    "READY",
    true
  );

  publishMsg(
    topic("wait"),
    "-1",
    true
  );

  publishMsg(
    topic("event"),
    "reset_done"
  );

  logSD("RESET");

  updateLeds();
}

void handleStateMachine() {
  unsigned long now = millis();

  if (
    relayActive &&
    now - relayStart >= RELAY_TIME
  ) {
    digitalWrite(RELAY, RELAY_OFF);
    relayActive = false;
  }

  if (state == OPEN) {
    int doorState = digitalRead(MC38_PIN);

    if (doorState == MC38_OPEN) {
      doorWasPhysicallyOpen = true;
    }

    if (
      doorWasPhysicallyOpen &&
      doorState == MC38_CLOSED
    ) {
      doorWasPhysicallyOpen = false;
      startWaitMode();
    }
  }

  if (state == WAIT) {
    unsigned long elapsed = now - waitStart;

    long remain =
      (long)((WAIT_TIME - min(elapsed, WAIT_TIME)) / 1000);

    if (remain < 0) remain = 0;

    if (remain != lastWaitRemain) {
      lastWaitRemain = remain;

      publishMsg(
        topic("wait"),
        String(remain),
        true
      );
    }

    if (elapsed >= WAIT_TIME) {
      state = READY;
      lastWaitRemain = -2;

      publishMsg(
        topic("state"),
        "READY",
        true
      );

      publishMsg(
        topic("wait"),
        "-1",
        true
      );

      publishMsg(
        topic("event"),
        "state_ready"
      );

      updateLeds();
    }
  }

  if (
    state == REFUSED &&
    now - refusedStart >= REFUSED_TIME
  ) {
    state = READY;

    publishMsg(
      topic("state"),
      "READY",
      true
    );

    updateLeds();
  }
}

/* =========================================================
   RFID ENTREE / SORTIE
   ========================================================= */
String readUidString(MFRC522& reader) {
  String uid = "";

  for (byte i = 0; i < reader.uid.size; i++) {
    if (reader.uid.uidByte[i] < 0x10) {
      uid += "0";
    }

    uid += String(
      reader.uid.uidByte[i],
      HEX
    );

    if (i < reader.uid.size - 1) {
      uid += ":";
    }
  }

  uid.toUpperCase();
  return uid;
}

void handleOneRfid(
  MFRC522& reader,
  const String& sens
) {
  if (!reader.PICC_IsNewCardPresent()) {
    return;
  }

  if (!reader.PICC_ReadCardSerial()) {
    return;
  }

  String uid = readUidString(reader);

  Serial.print("RFID ");
  Serial.print(sens);
  Serial.print(" UID: ");
  Serial.println(uid);

  int idx = findRfid(uid);

  if (idx < 0) {
    publishMsg(
      topic("rfid_result"),
      "UNKNOWN"
    );

    logAccessHistory(
      sens,
      "RFID",
      uid,
      "RFID inconnu",
      "UNKNOWN"
    );

    setRefused(
      "rfid_unknown_" + sens
    );
  }
  else if (!rfidDb[idx].enabled) {
    publishMsg(
      topic("rfid_result"),
      "DISABLED"
    );

    logAccessHistory(
      sens,
      "RFID",
      uid,
      String(rfidDb[idx].name),
      "DISABLED"
    );

    setRefused(
      "rfid_disabled_" + sens
    );
  }
  else if (
    !checkTimeAccess(
      rfidDb[idx].allowedFrom,
      rfidDb[idx].allowedTo
    )
  ) {
    publishMsg(
      topic("rfid_result"),
      "TIME_DENIED"
    );

    logAccessHistory(
      sens,
      "RFID",
      uid,
      String(rfidDb[idx].name),
      "TIME_DENIED"
    );

    setRefused(
      "rfid_time_denied_" + sens
    );
  }
  else if (
    rfidDb[idx].maxUses > 0 &&
    rfidDb[idx].usedCount >= rfidDb[idx].maxUses
  ) {
    publishMsg(
      topic("rfid_result"),
      "USE_LIMIT_REACHED"
    );

    logAccessHistory(
      sens,
      "RFID",
      uid,
      String(rfidDb[idx].name),
      "USE_LIMIT_REACHED"
    );

    setRefused(
      "rfid_use_limit_" + sens
    );
  }
  else if (!isDoorAllowedForCard(idx)) {
    publishMsg(
      topic("rfid_result"),
      "DOOR_NOT_ALLOWED"
    );

    logAccessHistory(
      sens,
      "RFID",
      uid,
      String(rfidDb[idx].name),
      "DOOR_NOT_ALLOWED"
    );

    setRefused(
      "door_not_allowed_" + sens
    );
  }
  else {
    String source = "RFID_" + sens;

    if (openDoor(source)) {
      rfidDb[idx].usedCount++;

      publishMsg(
        topic("rfid_result"),
        "AUTHORIZED"
      );

      if (sens == "ENTREE") {
        publishMsg(
          topic("rfid_consume"),
          uid
        );

        publishMsg(
          topic("rfid_entry"),
          uid
        );

        publishMsg(
          topic("access_direction"),
          "IN"
        );

        publishMsg(
          topic("event"),
          "rfid_entry_authorized"
        );
      }
      else {
        publishMsg(
          topic("rfid_exit"),
          uid
        );

        publishMsg(
          topic("access_direction"),
          "OUT"
        );

        publishMsg(
          topic("event"),
          "rfid_exit_authorized"
        );
      }

      logAccessHistory(
        sens,
        "RFID",
        uid,
        String(rfidDb[idx].name),
        "AUTHORIZED"
      );
    }
  }

  reader.PICC_HaltA();
  reader.PCD_StopCrypto1();
}

void handleRfid() {
  handleOneRfid(
    rfidIn,
    "ENTREE"
  );

  handleOneRfid(
    rfidOut,
    "SORTIE"
  );
}

/* =========================================================
   EMPREINTE
   ========================================================= */
void handleFinger() {
  uint8_t p = finger.getImage();

  if (p == FINGERPRINT_NOFINGER) return;
  if (p != FINGERPRINT_OK) return;
  if (finger.image2Tz() != FINGERPRINT_OK) return;

  p = finger.fingerSearch();

  if (p != FINGERPRINT_OK) {
    publishMsg(
      topic("fingerprint_result"),
      "UNKNOWN"
    );

    logAccessHistory(
      "ENTREE",
      "EMPREINTE",
      "INCONNU",
      "Empreinte inconnue",
      "UNKNOWN"
    );

    setRefused("finger_unknown");
    return;
  }

  int id = finger.fingerID;

  publishMsg(
    topic("fingerprint"),
    String(id)
  );

  int idx = findFinger(id);

  if (idx < 0) {
    publishMsg(
      topic("fingerprint_result"),
      "UNKNOWN"
    );

    logAccessHistory(
      "ENTREE",
      "EMPREINTE",
      String(id),
      "Empreinte absente de la base",
      "UNKNOWN"
    );

    setRefused("finger_not_in_db");
  }
  else if (!fingerDb[idx].enabled) {
    publishMsg(
      topic("fingerprint_result"),
      "DISABLED"
    );

    logAccessHistory(
      "ENTREE",
      "EMPREINTE",
      String(id),
      String(fingerDb[idx].name),
      "DISABLED"
    );

    setRefused("finger_disabled");
  }
  else if (
    !checkTimeAccess(
      fingerDb[idx].allowedFrom,
      fingerDb[idx].allowedTo
    )
  ) {
    publishMsg(
      topic("fingerprint_result"),
      "TIME_DENIED"
    );

    logAccessHistory(
      "ENTREE",
      "EMPREINTE",
      String(id),
      String(fingerDb[idx].name),
      "TIME_DENIED"
    );

    setRefused("finger_time_denied");
  }
  else if (
    fingerDb[idx].maxUses > 0 &&
    fingerDb[idx].usedCount >= fingerDb[idx].maxUses
  ) {
    publishMsg(
      topic("fingerprint_result"),
      "USE_LIMIT_REACHED"
    );

    logAccessHistory(
      "ENTREE",
      "EMPREINTE",
      String(id),
      String(fingerDb[idx].name),
      "USE_LIMIT_REACHED"
    );

    setRefused("finger_use_limit");
  }
  else if (!isDoorAllowedForFinger(idx)) {
    publishMsg(
      topic("fingerprint_result"),
      "DOOR_NOT_ALLOWED"
    );

    logAccessHistory(
      "ENTREE",
      "EMPREINTE",
      String(id),
      String(fingerDb[idx].name),
      "DOOR_NOT_ALLOWED"
    );

    setRefused("door_not_allowed");
  }
  else {
    if (openDoor("FINGER")) {
      fingerDb[idx].usedCount++;

      publishMsg(
        topic("fingerprint_result"),
        "AUTHORIZED"
      );

      publishMsg(
        topic("fingerprint_consume"),
        String(id)
      );

      logAccessHistory(
        "ENTREE",
        "EMPREINTE",
        String(id),
        String(fingerDb[idx].name),
        "AUTHORIZED"
      );
    }
  }
}

/* =========================================================
   CAPTEURS
   ========================================================= */
void handleSensors() {
  static int lastDoor = -1;
  static int lastPir = -1;

  int doorNow = digitalRead(MC38_PIN);
  int pirNow = digitalRead(PIR_PIN);

  if (doorNow != lastDoor) {
    lastDoor = doorNow;

    publishMsg(
      topic("door"),
      doorNow == MC38_OPEN
        ? "OPEN"
        : "CLOSED",
      true
    );
  }

  if (pirNow != lastPir) {
    lastPir = pirNow;

    publishMsg(
      topic("pir"),
      pirNow == PIR_ACTIVE
        ? "DETECTED"
        : "CLEAR",
      true
    );
  }

  if (
    millis() - lastDhtRead >=
    DHT_INTERVAL
  ) {
    lastDhtRead = millis();

    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (!isnan(t)) {
      lastTemp = t;

      publishMsg(
        topic("temp"),
        String(t, 1),
        true
      );
    }

    if (!isnan(h)) {
      lastHum = h;

      publishMsg(
        topic("hum"),
        String(h, 1),
        true
      );
    }
  }
}

/* =========================================================
   BOUTONS
   ========================================================= */
bool buttonPressedEdge(
  int pin,
  int& stableState,
  int& candidateState,
  unsigned long& candidateTime
) {
  int reading = digitalRead(pin);
  unsigned long now = millis();

  if (reading != candidateState) {
    candidateState = reading;
    candidateTime = now;
  }

  if (
    candidateState != stableState &&
    now - candidateTime >= BUTTON_DEBOUNCE
  ) {
    int previousState = stableState;
    stableState = candidateState;

    return (
      previousState == HIGH &&
      stableState == LOW
    );
  }

  return false;
}

void handleButtons() {
  if (
    buttonPressedEdge(
      BUTTON_OPEN,
      openStableState,
      openCandidateState,
      openCandidateTime
    )
  ) {
    if (openDoor("BUTTON_OPEN")) {
      logAccessHistory(
        "OUVERTURE",
        "BOUTON",
        "BUTTON_OPEN",
        "Ouverture manuelle",
        "AUTHORIZED"
      );
    }
  }

  if (
    buttonPressedEdge(
      BUTTON_CLOSE,
      closeStableState,
      closeCandidateState,
      closeCandidateTime
    )
  ) {
    if (state == OPEN) {
      startWaitMode();
    }
  }
}

/* =========================================================
   CHARGEMENT BASE RFID
   ========================================================= */
void loadRfidDb(
  const String& json,
  bool saveToSd = true
) {
  StaticJsonDocument<16384> doc;

  DeserializationError error =
    deserializeJson(doc, json);

  if (error) {
    Serial.print("RFID JSON error: ");
    Serial.println(error.c_str());

    publishMsg(
      topic("event"),
      "rfid_db_parse_error"
    );

    return;
  }

  clearRfidDb();

  JsonArray arr = doc.as<JsonArray>();
  int i = 0;

  for (JsonObject o : arr) {
    if (i >= MAX_RFID_USERS) break;

    String uid =
      normalizeUid(o["uid"] | "");

    String name =
      o["name"] | "";

    if (uid.length() == 0) continue;

    String allowedDoors = "ALL";

    if (o["allowedDoors"].is<JsonArray>()) {
      allowedDoors = "";

      for (
        String d :
        o["allowedDoors"].as<JsonArray>()
      ) {
        if (allowedDoors.length() > 0) {
          allowedDoors += ",";
        }

        allowedDoors += d;
      }
    }

    strncpy(
      rfidDb[i].uid,
      uid.c_str(),
      sizeof(rfidDb[i].uid) - 1
    );

    rfidDb[i].uid[
      sizeof(rfidDb[i].uid) - 1
    ] = '\0';

    strncpy(
      rfidDb[i].name,
      name.c_str(),
      sizeof(rfidDb[i].name) - 1
    );

    rfidDb[i].name[
      sizeof(rfidDb[i].name) - 1
    ] = '\0';

    strncpy(
      rfidDb[i].allowedDoors,
      allowedDoors.c_str(),
      sizeof(rfidDb[i].allowedDoors) - 1
    );

    rfidDb[i].allowedDoors[
      sizeof(rfidDb[i].allowedDoors) - 1
    ] = '\0';

    rfidDb[i].enabled =
      o["enabled"] | true;

    rfidDb[i].allowedFrom =
      o["allowedFrom"] | 0UL;

    rfidDb[i].allowedTo =
      o["allowedTo"] | 0UL;

    rfidDb[i].maxUses =
      o["maxUses"] | 0;

    rfidDb[i].usedCount =
      o["usedCount"] | 0;

    rfidDb[i].used = true;

    i++;
  }

  Serial.print("RFID users loaded: ");
  Serial.println(i);

  if (saveToSd && sdReady) {
    writeFileSD(RFID_FILE, json);
  }

  publishMsg(
    topic("event"),
    "rfid_db_loaded"
  );
}

/* =========================================================
   CHARGEMENT BASE EMPREINTES
   ========================================================= */
void loadFingerDb(
  const String& json,
  bool saveToSd = true
) {
  StaticJsonDocument<16384> doc;

  DeserializationError error =
    deserializeJson(doc, json);

  if (error) {
    Serial.print("Finger JSON error: ");
    Serial.println(error.c_str());

    publishMsg(
      topic("event"),
      "finger_db_parse_error"
    );

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

      for (
        String d :
        o["allowedDoors"].as<JsonArray>()
      ) {
        if (allowedDoors.length() > 0) {
          allowedDoors += ",";
        }

        allowedDoors += d;
      }
    }

    fingerDb[i].id = id;

    strncpy(
      fingerDb[i].name,
      name.c_str(),
      sizeof(fingerDb[i].name) - 1
    );

    fingerDb[i].name[
      sizeof(fingerDb[i].name) - 1
    ] = '\0';

    strncpy(
      fingerDb[i].allowedDoors,
      allowedDoors.c_str(),
      sizeof(fingerDb[i].allowedDoors) - 1
    );

    fingerDb[i].allowedDoors[
      sizeof(fingerDb[i].allowedDoors) - 1
    ] = '\0';

    fingerDb[i].enabled =
      o["enabled"] | true;

    fingerDb[i].allowedFrom =
      o["allowedFrom"] | 0UL;

    fingerDb[i].allowedTo =
      o["allowedTo"] | 0UL;

    fingerDb[i].maxUses =
      o["maxUses"] | 0;

    fingerDb[i].usedCount =
      o["usedCount"] | 0;

    fingerDb[i].used = true;

    i++;
  }

  Serial.print("Finger users loaded: ");
  Serial.println(i);

  if (saveToSd && sdReady) {
    writeFileSD(FINGER_FILE, json);
  }

  publishMsg(
    topic("event"),
    "finger_db_loaded"
  );
}

void loadDatabasesFromSD() {
  if (!sdReady) {
    Serial.println(
      "SD indisponible : bases non chargees"
    );

    return;
  }

  String rfidJson =
    readFileSD(RFID_FILE);

  if (rfidJson.length() > 5) {
    loadRfidDb(rfidJson, false);
  }
  else {
    Serial.println(
      "RFID DB absente ou vide"
    );
  }

  String fingerJson =
    readFileSD(FINGER_FILE);

  if (fingerJson.length() > 5) {
    loadFingerDb(fingerJson, false);
  }
  else {
    Serial.println(
      "Finger DB absente ou vide"
    );
  }
}

/* =========================================================
   ENREGISTREMENT EMPREINTE
   ========================================================= */
void enrollFinger(int id) {
  if (id < 1 || id > 127) {
    publishMsg(
      topic("finger_enroll_result"),
      "ERROR:invalid_id",
      true
    );

    return;
  }

  publishMsg(
    topic("finger_enroll_status"),
    "put_finger",
    true
  );

  unsigned long start = millis();

  while (
    finger.getImage() !=
    FINGERPRINT_OK
  ) {
    client.loop();

    if (millis() - start > 30000) {
      publishMsg(
        topic("finger_enroll_result"),
        "ERROR:timeout_first_image",
        true
      );

      return;
    }

    delay(50);
  }

  if (
    finger.image2Tz(1) !=
    FINGERPRINT_OK
  ) {
    publishMsg(
      topic("finger_enroll_result"),
      "ERROR:first_image",
      true
    );

    return;
  }

  publishMsg(
    topic("finger_enroll_status"),
    "remove_finger",
    true
  );

  delay(2000);
  start = millis();

  while (
    finger.getImage() !=
    FINGERPRINT_NOFINGER
  ) {
    client.loop();

    if (millis() - start > 15000) {
      publishMsg(
        topic("finger_enroll_result"),
        "ERROR:remove_timeout",
        true
      );

      return;
    }

    delay(50);
  }

  publishMsg(
    topic("finger_enroll_status"),
    "put_same_finger",
    true
  );

  start = millis();

  while (
    finger.getImage() !=
    FINGERPRINT_OK
  ) {
    client.loop();

    if (millis() - start > 30000) {
      publishMsg(
        topic("finger_enroll_result"),
        "ERROR:timeout_second_image",
        true
      );

      return;
    }

    delay(50);
  }

  if (
    finger.image2Tz(2) !=
    FINGERPRINT_OK
  ) {
    publishMsg(
      topic("finger_enroll_result"),
      "ERROR:second_image",
      true
    );

    return;
  }

  if (
    finger.createModel() !=
    FINGERPRINT_OK
  ) {
    publishMsg(
      topic("finger_enroll_result"),
      "ERROR:create_model",
      true
    );

    return;
  }

  if (
    finger.storeModel(id) !=
    FINGERPRINT_OK
  ) {
    publishMsg(
      topic("finger_enroll_result"),
      "ERROR:store_model",
      true
    );

    return;
  }

  publishMsg(
    topic("finger_enroll_status"),
    "finger_saved",
    true
  );

  publishMsg(
    topic("finger_enroll_result"),
    "SUCCESS:" + String(id),
    true
  );

  logSD(
    "FINGER ENROLLED: " +
    String(id)
  );
}

void deleteFinger(int id) {
  if (
    finger.deleteModel(id) ==
    FINGERPRINT_OK
  ) {
    publishMsg(
      topic("finger_enroll_result"),
      "DELETED:" + String(id),
      true
    );

    logSD(
      "FINGER DELETED: " +
      String(id)
    );
  }
  else {
    publishMsg(
      topic("finger_enroll_result"),
      "ERROR:delete",
      true
    );
  }
}

void clearFingerSensor() {
  if (
    finger.emptyDatabase() ==
    FINGERPRINT_OK
  ) {
    publishMsg(
      topic("finger_enroll_result"),
      "CLEARED_ALL",
      true
    );

    logSD("FINGER DATABASE CLEARED");
  }
  else {
    publishMsg(
      topic("finger_enroll_result"),
      "ERROR:clear_all",
      true
    );
  }
}

/* =========================================================
   MQTT CALLBACK
   ========================================================= */
bool validName(String n) {
  n.trim();

  if (n.length() == 0) return false;
  if (n.indexOf("/") >= 0) return false;
  if (n.indexOf("+") >= 0) return false;
  if (n.indexOf("#") >= 0) return false;

  return true;
}

void mqttCallback(
  char* top,
  byte* payload,
  unsigned int length
) {
  String msg = "";

  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  msg.trim();

  String t = String(top);

  if (
    t == "sas/" + deviceId + "/config/name" ||
    t == "sas/" + doorId + "/config/name"
  ) {
    if (validName(msg)) {
      prefs.putString("doorId", msg);
      doorId = msg;

      saveConfigToSD();

      publishMsg(
        "sas/" + deviceId + "/config/status",
        "name_saved_restart",
        true
      );

      delay(500);
      ESP.restart();
    }
    else {
      publishMsg(
        "sas/" + deviceId + "/config/status",
        "invalid_name",
        true
      );
    }
  }

  else if (t == topic("cmd")) {
    if (msg == "OPEN") {
      if (openDoor("CMD")) {
        logAccessHistory(
          "OUVERTURE",
          "MQTT",
          "CMD_OPEN",
          "Commande distante",
          "AUTHORIZED"
        );
      }
    }
    else if (msg == "CLOSE") {
      if (state == OPEN) {
        startWaitMode();
      }
    }
    else if (msg == "RESET") {
      resetDoor();
    }
  }

  else if (t == topic("lock")) {
    if (msg == "LOCKED") {
      relationLocked = true;

      publishMsg(
        topic("event"),
        "relation_locked"
      );

      publishMsg(
        topic("lock_status"),
        "LOCKED",
        true
      );

      logSD("RELATION LOCKED");

      beepDouble();
      updateLeds();
    }
    else if (msg == "UNLOCKED") {
      relationLocked = false;

      publishMsg(
        topic("event"),
        "relation_unlocked"
      );

      publishMsg(
        topic("lock_status"),
        "UNLOCKED",
        true
      );

      logSD("RELATION UNLOCKED");

      beepShort();
      updateLeds();
    }
  }

  else if (t == "sas/rfid/db") {
    loadRfidDb(msg, true);
  }

  else if (t == "sas/finger/db") {
    loadFingerDb(msg, true);
  }

  else if (t == topic("finger/cmd")) {
    if (msg.startsWith("ENROLL:")) {
      enrollFinger(
        msg.substring(7).toInt()
      );
    }
    else if (msg.startsWith("DELETE:")) {
      deleteFinger(
        msg.substring(7).toInt()
      );
    }
    else if (msg == "CLEAR_ALL") {
      clearFingerSensor();
    }
  }

  else if (
    t.startsWith("sas/") &&
    t.endsWith("/state")
  ) {
    String other =
      t.substring(
        4,
        t.length() - 6
      );

    if (
      other != doorId &&
      other != deviceId
    ) {
      if (
        msg == "OPEN" ||
        msg == "WAIT"
      ) {
        otherDoorBusy = true;
      }
      else if (
        msg == "READY" ||
        msg == "REFUSED"
      ) {
        otherDoorBusy = false;
      }
    }
  }
}

/* =========================================================
   WIFI
   ========================================================= */
void setupWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connexion WiFi");

  unsigned long start = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < 10000
  ) {
    Serial.print(".");
    delay(500);
    updateLeds();
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connecte");

    Serial.print("IP : ");
    Serial.println(WiFi.localIP());
  }
  else {
    Serial.println("WiFi non connecte");
  }
}

/* =========================================================
   MQTT RECONNECT
   ========================================================= */
void reconnectMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (client.connected()) return;

  String clientId =
    "SAS_" +
    deviceId +
    "_" +
    String((uint32_t)millis(), HEX);

  Serial.print("Connexion MQTT...");

  if (!client.connect(clientId.c_str())) {
    Serial.print("Erreur : ");
    Serial.println(client.state());
    return;
  }

  Serial.println("OK");

  client.subscribe(topic("cmd").c_str());
  client.subscribe(topic("finger/cmd").c_str());
  client.subscribe(topic("lock").c_str());

  client.subscribe(
    (
      "sas/" +
      deviceId +
      "/config/name"
    ).c_str()
  );

  client.subscribe(
    (
      "sas/" +
      doorId +
      "/config/name"
    ).c_str()
  );

  client.subscribe("sas/rfid/db");
  client.subscribe("sas/finger/db");
  client.subscribe("sas/+/state");

  publishMsg(
    topic("status"),
    "online",
    true
  );

  publishMsg(
    topic("event"),
    "boot",
    true
  );

  publishMsg(
    topic("lock_status"),
    relationLocked
      ? "LOCKED"
      : "UNLOCKED",
    true
  );

  publishMsg(
    "sas/" + deviceId + "/info",
    doorId,
    true
  );

  publishMsg(
    "sas/" + deviceId + "/status",
    "online",
    true
  );

  publishAll();
  updateLeds();
}

/* =========================================================
   INITIALISATION RFID
   ========================================================= */
void initRfidReaders() {
  SPI.begin(
    RFID_SCK,
    RFID_MISO,
    RFID_MOSI
  );

  pinMode(RFID_IN_SS, OUTPUT);
  pinMode(RFID_OUT_SS, OUTPUT);

  digitalWrite(RFID_IN_SS, HIGH);
  digitalWrite(RFID_OUT_SS, HIGH);

  delay(100);

  rfidIn.PCD_Init();
  delay(100);

  byte versionIn =
    rfidIn.PCD_ReadRegister(
      MFRC522::VersionReg
    );

  Serial.print("RFID ENTREE version: 0x");
  Serial.println(versionIn, HEX);

  digitalWrite(RFID_IN_SS, HIGH);
  digitalWrite(RFID_OUT_SS, HIGH);

  rfidOut.PCD_Init();
  delay(100);

  byte versionOut =
    rfidOut.PCD_ReadRegister(
      MFRC522::VersionReg
    );

  Serial.print("RFID SORTIE version: 0x");
  Serial.println(versionOut, HEX);

  if (
    versionIn == 0x00 ||
    versionIn == 0xFF
  ) {
    Serial.println("ERREUR RFID ENTREE");
  }
  else {
    Serial.println("RFID ENTREE OK");
  }

  if (
    versionOut == 0x00 ||
    versionOut == 0xFF
  ) {
    Serial.println("ERREUR RFID SORTIE");
  }
  else {
    Serial.println("RFID SORTIE OK");
  }
}

/* =========================================================
   SETUP
   ========================================================= */
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("==============================");
  Serial.println("DEMARRAGE SYSTEME SAS");
  Serial.println("==============================");

  prefs.begin(
    "sas_config",
    false
  );

  deviceId = makeDeviceId();

  doorId = prefs.getString(
    "doorId",
    deviceId
  );

  Serial.println(
    "Device ID: " + deviceId
  );

  Serial.println(
    "Door ID: " + doorId
  );

  clearRfidDb();
  clearFingerDb();

  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_STATUS, OUTPUT);

  pinMode(BUZZER, OUTPUT);
  pinMode(RELAY, OUTPUT);

  pinMode(BUTTON_OPEN, INPUT);
  pinMode(BUTTON_CLOSE, INPUT);

  pinMode(MC38_PIN, INPUT);
  pinMode(PIR_PIN, INPUT);

  digitalWrite(RELAY, RELAY_OFF);
  digitalWrite(BUZZER, LOW);

  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_STATUS, LOW);

  delay(100);

  openStableState =
    digitalRead(BUTTON_OPEN);

  openCandidateState =
    openStableState;

  closeStableState =
    digitalRead(BUTTON_CLOSE);

  closeCandidateState =
    closeStableState;

  openCandidateTime = millis();
  closeCandidateTime = millis();

  initRfidReaders();

  initSD();
  loadDatabasesFromSD();

  fingerSerial.begin(
    57600,
    SERIAL_8N1,
    FINGER_RX,
    FINGER_TX
  );

  finger.begin(57600);
  delay(100);

  if (finger.verifyPassword()) {
    Serial.println("Capteur empreinte OK");
  }
  else {
    Serial.println("Capteur empreinte ERROR");
  }

  dht.begin();

  setupWifi();

  if (WiFi.status() == WL_CONNECTED) {
    configTime(
      gmtOffset_sec,
      daylightOffset_sec,
      ntpServer1,
      ntpServer2
    );

    client.setServer(
      mqtt_server,
      mqtt_port
    );

    client.setCallback(
      mqttCallback
    );

    client.setBufferSize(4096);

    reconnectMqtt();
  }

  if (finger.verifyPassword()) {
    publishMsg(
      topic("event"),
      "finger_sensor_ok"
    );
  }
  else {
    publishMsg(
      topic("event"),
      "finger_sensor_error"
    );
  }

  publishMsg(
    topic("sd_status"),
    sdReady ? "OK" : "ERROR",
    true
  );

  updateLeds();

  Serial.println("Setup termine");
}

/* =========================================================
   LOOP
   ========================================================= */
void loop() {
  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (
      !client.connected() &&
      now - lastMqttReconnect >=
      MQTT_RECONNECT_TIME
    ) {
      lastMqttReconnect = now;
      reconnectMqtt();
    }

    client.loop();
  }
  else if (
    now - lastWifiReconnect >=
    WIFI_RECONNECT_TIME
  ) {
    lastWifiReconnect = now;
    setupWifi();
  }

  handleStateMachine();
  handleSensors();
  handleButtons();

  if (state == READY) {
    handleRfid();
    handleFinger();
  }

  if (
    now - lastHeartbeat >=
    HEARTBEAT_TIME
  ) {
    lastHeartbeat = now;

    publishMsg(
      topic("status"),
      WiFi.status() == WL_CONNECTED
        ? "online"
        : "offline",
      true
    );

    publishMsg(
      "sas/" + deviceId + "/status",
      WiFi.status() == WL_CONNECTED
        ? "online"
        : "offline",
      true
    );

    publishAll();
    updateLeds();
  }
}