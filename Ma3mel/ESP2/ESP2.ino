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

#include <Wire.h>
#include <Adafruit_MCP23X17.h>
#include <ESP_I2S.h>
#include <RTClib.h>

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

const bool USE_GLOBAL_DOOR_STATE_LOCK = false;

/* =========================================================
   I2C
   ========================================================= */
#define I2C_SDA 21
#define I2C_SCL 25

/* =========================================================
   MCP23017
   ========================================================= */
#define MCP_ADDRESS   0x20

#define MCP_LED_GREEN 0
#define MCP_LED_RED   1
#define MCP_BUTTON    2

Adafruit_MCP23X17 mcp;

bool mcpReady = false;

/* =========================================================
   RTC DS3231
   ========================================================= */
RTC_DS3231 rtc;

bool rtcReady = false;

const unsigned long RTC_FIRST_SYNC_TIME =
  60000UL;

const unsigned long RTC_SYNC_INTERVAL =
  3600000UL;

unsigned long lastRtcSync = 0;

bool rtcHasBeenSynced = false;

/* =========================================================
   MAX98357A
   ========================================================= */
#define I2S_BCLK 12
#define I2S_LRC   4
#define I2S_DOUT  2

#define AUDIO_SAMPLE_RATE 44100

I2SClass I2S;

bool audioReady = false;

/* =========================================================
   FICHIERS AUDIO
   ========================================================= */
const char* AUDIO_DOOR_OPEN =
  "/porte_ouverte.wav";

const char* AUDIO_DOOR_CLOSED =
  "/porte_fermee.wav";

const char* AUDIO_ACCESS_DENIED =
  "/acces_refuse.wav";

/* =========================================================
   ALARME TEMPERATURE HORS LIGNE
   ========================================================= */

/*
   Déclenchement :
   - WiFi/MQTT indisponible
   - température >= 50°C

   Arrêt :
   - température <= 48°C
   OU
   - connexion réseau revenue

   L'hystérésis 50 / 48 évite les déclenchements
   et arrêts rapides autour du seuil.
*/
const float TEMP_ALARM_ON =
  28.0;

const float TEMP_ALARM_OFF =
  27.0;

const unsigned long TEMP_ALARM_BLINK_TIME =
  400;

const unsigned long TEMP_ALARM_SOUND_TIME =
  1200;

bool temperatureAlarmActive =
  false;

bool temperatureAlarmLedOn =
  false;

bool temperatureAlarmToneHigh =
  false;

unsigned long lastTemperatureAlarmBlink =
  0;

unsigned long lastTemperatureAlarmSound =
  0;

/* =========================================================
   SPI PRINCIPAL : DEUX RFID
   ========================================================= */
#define RFID_SCK       18
#define RFID_MISO      19
#define RFID_MOSI      23

#define RFID_IN_SS      5
#define RFID_OUT_SS    27
#define RFID_RST       22

MFRC522 rfidIn(
  RFID_IN_SS,
  RFID_RST
);

MFRC522 rfidOut(
  RFID_OUT_SS,
  RFID_RST
);

/* =========================================================
   SPI SEPARE : CARTE SD
   ========================================================= */
#define SD_SCK         26
#define SD_MISO        35
#define SD_MOSI        13
#define SD_CS          15

SPIClass spiSD(HSPI);

bool sdReady = false;

const char* LOG_FILE =
  "/log.txt";

const char* ACCESS_HISTORY_FILE =
  "/access_history.csv";

const char* PENDING_EVENTS_FILE =
  "/pending_events.ndjson";

const char* PENDING_TEMP_FILE =
  "/pending_temp.ndjson";

const char* RFID_FILE =
  "/rfid_db.json";

const char* FINGER_FILE =
  "/finger_db.json";

const char* CONFIG_FILE =
  "/config.json";

/* =========================================================
   SYNCHRONISATION DES EVENEMENTS
   ========================================================= */
const unsigned long SYNC_RETRY_TIME =
  5000;

const unsigned long SYNC_CHECK_TIME =
  1000;

bool waitingAccessAck =
  false;

String waitingEventId =
  "";

String waitingEventJson =
  "";

unsigned long lastSyncPublish =
  0;

unsigned long lastSyncCheck =
  0;

/* =========================================================
   IDENTIFIANTS
   ========================================================= */
String deviceId =
  "";

String doorId =
  "";

String makeDeviceId() {

  uint64_t chipId =
    ESP.getEfuseMac();

  uint32_t shortId =
    (uint32_t)(
      chipId ^
      (chipId >> 32)
    );

  return
    "esp_" +
    String(
      shortId,
      HEX
    );
}

String baseTopic() {

  return
    "sas/" +
    doorId +
    "/";
}

String topic(
  const String& sub
) {

  return
    baseTopic() +
    sub;
}

/* =========================================================
   NTP
   ========================================================= */
const char* ntpServer1 =
  "pool.ntp.org";

const char* ntpServer2 =
  "time.nist.gov";

const long gmtOffset_sec =
  3600;

const int daylightOffset_sec =
  0;

/* =========================================================
   PINS ESP32
   ========================================================= */
#define RELAY          14

#define MC38_PIN       34
#define PIR_PIN        33

#define DHTPIN         32
#define DHTTYPE        DHT11

#define FINGER_RX      16
#define FINGER_TX      17

HardwareSerial fingerSerial(2);

Adafruit_Fingerprint finger(
  &fingerSerial
);

DHT dht(
  DHTPIN,
  DHTTYPE
);

/* =========================================================
   LOGIQUE ELECTRIQUE
   ========================================================= */
#define MC38_CLOSED LOW
#define MC38_OPEN   HIGH

#define PIR_ACTIVE  HIGH

#define RELAY_ON    HIGH
#define RELAY_OFF   LOW

/* =========================================================
   TEMPORISATIONS
   ========================================================= */
const unsigned long WAIT_TIME =
  4000;

const unsigned long RELAY_TIME =
  1200;

const unsigned long REFUSED_TIME =
  1500;

const unsigned long BUTTON_DEBOUNCE =
  80;

const unsigned long DHT_INTERVAL =
  3000;

const unsigned long HEARTBEAT_TIME =
  5000;

const unsigned long MQTT_RECONNECT_TIME =
  5000;

const unsigned long WIFI_RECONNECT_TIME =
  15000;

/* =========================================================
   ETAT PORTE
   ========================================================= */
enum State {
  READY,
  OPEN,
  WAIT,
  REFUSED
};

State state =
  READY;

unsigned long waitStart =
  0;

unsigned long relayStart =
  0;

unsigned long refusedStart =
  0;

unsigned long lastDhtRead =
  0;

unsigned long lastHeartbeat =
  0;

unsigned long lastMqttReconnect =
  0;

unsigned long lastWifiReconnect =
  0;

long lastWaitRemain =
  -2;

bool relayActive =
  false;

bool otherDoorBusy =
  false;

bool relationLocked =
  false;

bool doorWasPhysicallyOpen =
  false;

float lastTemp =
  NAN;

float lastHum =
  NAN;

/* =========================================================
   BOUTON MCP23017
   ========================================================= */
int buttonStableState =
  HIGH;

int buttonCandidateState =
  HIGH;

unsigned long buttonCandidateTime =
  0;

/* =========================================================
   BASES DONNEES
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

RfidUser rfidDb[
  MAX_RFID_USERS
];

FingerUser fingerDb[
  MAX_FINGER_USERS
];

/* =========================================================
   PROTOTYPES
   ========================================================= */

/*
   Nécessaire car certaines fonctions utilisent
   publishMsg() avant sa définition complète.
*/
void publishMsg(
  const String& mqttTopic,
  const String& message,
  bool retained
);

void updateLeds();

void logSD(
  const String& text
);

void playTone(
  int frequency,
  int durationMs
);

/* =========================================================
   INITIALISATION MCP23017
   ========================================================= */
bool initMcp23017() {

  Serial.println(
    "Initialisation I2C..."
  );

  Wire.begin(
    I2C_SDA,
    I2C_SCL
  );

  delay(50);

  if (
    !mcp.begin_I2C(
      MCP_ADDRESS
    )
  ) {

    mcpReady =
      false;

    Serial.println(
      "ERREUR MCP23017"
    );

    return false;
  }

  mcpReady =
    true;

  Serial.println(
    "MCP23017 OK adresse 0x20"
  );

  mcp.pinMode(
    MCP_LED_GREEN,
    OUTPUT
  );

  mcp.pinMode(
    MCP_LED_RED,
    OUTPUT
  );

  mcp.pinMode(
    MCP_BUTTON,
    INPUT_PULLUP
  );

  mcp.digitalWrite(
    MCP_LED_GREEN,
    LOW
  );

  mcp.digitalWrite(
    MCP_LED_RED,
    LOW
  );

  buttonStableState =
    mcp.digitalRead(
      MCP_BUTTON
    );

  buttonCandidateState =
    buttonStableState;

  buttonCandidateTime =
    millis();

  return true;
}

/* =========================================================
   SCAN I2C
   ========================================================= */
void scanI2CBus() {

  Serial.println();

  Serial.println(
    "===== SCAN I2C SYSTEME ====="
  );

  int count =
    0;

  for (
    int address = 1;
    address < 127;
    address++
  ) {

    Wire.beginTransmission(
      address
    );

    byte error =
      Wire.endTransmission();

    if (
      error == 0
    ) {

      Serial.print(
        "I2C detecte : 0x"
      );

      if (
        address < 16
      ) {

        Serial.print(
          "0"
        );
      }

      Serial.println(
        address,
        HEX
      );

      count++;
    }
  }

  Serial.print(
    "Nombre modules I2C : "
  );

  Serial.println(
    count
  );

  Serial.println(
    "0x20 = MCP23017"
  );

  Serial.println(
    "0x57 = EEPROM AT24C32"
  );

  Serial.println(
    "0x68 = DS3231"
  );

  Serial.println(
    "============================"
  );

  Serial.println();
}

/* =========================================================
   RTC -> HEURE SYSTEME ESP32
   ========================================================= */
bool setSystemTimeFromRtc() {

  if (
    !rtcReady
  ) {

    return false;
  }

  DateTime rtcNow =
    rtc.now();

  if (
    rtcNow.year() < 2024 ||
    rtcNow.year() > 2099
  ) {

    Serial.println(
      "RTC : date invalide"
    );

    return false;
  }

  /*
     Le DS3231 contient ici l'heure locale.
     On reconvertit vers UTC pour l'heure système ESP32.
  */
  int64_t utcEpoch =
    (int64_t)
      rtcNow.unixtime() -
    gmtOffset_sec -
    daylightOffset_sec;

  if (
    utcEpoch <= 0
  ) {

    return false;
  }

  struct timeval tv;

  tv.tv_sec =
    (time_t)utcEpoch;

  tv.tv_usec =
    0;

  settimeofday(
    &tv,
    nullptr
  );

  Serial.println(
    "Heure systeme chargee depuis DS3231"
  );

  return true;
}

/* =========================================================
   INITIALISATION RTC DS3231
   ========================================================= */
bool initRtc() {

  Serial.println(
    "Initialisation DS3231..."
  );

  if (
    !rtc.begin(
      &Wire
    )
  ) {

    rtcReady =
      false;

    Serial.println(
      "ERREUR : DS3231 non detecte"
    );

    return false;
  }

  rtcReady =
    true;

  Serial.println(
    "DS3231 OK adresse 0x68"
  );

  if (
    rtc.lostPower()
  ) {

    Serial.println(
      "ATTENTION : DS3231 a perdu son alimentation"
    );

    Serial.println(
      "Synchronisation NTP necessaire"
    );

    return true;
  }

  DateTime rtcNow =
    rtc.now();

  Serial.print(
    "RTC : "
  );

  Serial.print(
    rtcNow.day()
  );

  Serial.print(
    "/"
  );

  Serial.print(
    rtcNow.month()
  );

  Serial.print(
    "/"
  );

  Serial.print(
    rtcNow.year()
  );

  Serial.print(
    " "
  );

  if (
    rtcNow.hour() < 10
  ) {

    Serial.print(
      "0"
    );
  }

  Serial.print(
    rtcNow.hour()
  );

  Serial.print(
    ":"
  );

  if (
    rtcNow.minute() < 10
  ) {

    Serial.print(
      "0"
    );
  }

  Serial.print(
    rtcNow.minute()
  );

  Serial.print(
    ":"
  );

  if (
    rtcNow.second() < 10
  ) {

    Serial.print(
      "0"
    );
  }

  Serial.println(
    rtcNow.second()
  );

  setSystemTimeFromRtc();

  return true;
}

/* =========================================================
   SYSTEME/NTP -> RTC
   ========================================================= */
void syncRtcFromSystemTime() {

  if (
    !rtcReady
  ) {

    return;
  }

  time_t systemNow;

  time(
    &systemNow
  );

  if (
    systemNow <=
    1700000000
  ) {

    return;
  }

  uint32_t localEpoch =
    (uint32_t)(
      systemNow +
      gmtOffset_sec +
      daylightOffset_sec
    );

  rtc.adjust(
    DateTime(
      localEpoch
    )
  );

  Serial.println(
    "DS3231 synchronise depuis heure systeme/NTP"
  );
}

/* =========================================================
   MAINTENANCE RTC
   ========================================================= */
void maintainRtc() {

  if (
    !rtcReady
  ) {

    return;
  }

  if (
    WiFi.status() !=
    WL_CONNECTED
  ) {

    return;
  }

  unsigned long now =
    millis();

  if (
    !rtcHasBeenSynced
  ) {

    if (
      now <
      RTC_FIRST_SYNC_TIME
    ) {

      return;
    }

    time_t systemNow;

    time(
      &systemNow
    );

    if (
      systemNow >
      1700000000
    ) {

      syncRtcFromSystemTime();

      rtcHasBeenSynced =
        true;

      lastRtcSync =
        now;
    }

    return;
  }

  if (
    now -
      lastRtcSync >=
    RTC_SYNC_INTERVAL
  ) {

    syncRtcFromSystemTime();

    lastRtcSync =
      now;
  }
}

/* =========================================================
   INITIALISATION AUDIO MAX98357A
   ========================================================= */
bool initAudio() {

  Serial.println(
    "Initialisation MAX98357A..."
  );

  I2S.setPins(
    I2S_BCLK,
    I2S_LRC,
    I2S_DOUT,
    -1,
    -1
  );

  if (
    !I2S.begin(
      I2S_MODE_STD,
      AUDIO_SAMPLE_RATE,
      I2S_DATA_BIT_WIDTH_16BIT,
      I2S_SLOT_MODE_MONO
    )
  ) {

    audioReady =
      false;

    Serial.println(
      "ERREUR MAX98357A / I2S"
    );

    return false;
  }

  audioReady =
    true;

  Serial.println(
    "MAX98357A AUDIO OK"
  );

  return true;
}

/* =========================================================
   LECTURE WAV DEPUIS SD
   ========================================================= */
void playAudioFile(
  const char* path
) {

  if (
    !audioReady
  ) {

    Serial.println(
      "Audio indisponible"
    );

    return;
  }

  if (
    !sdReady
  ) {

    Serial.println(
      "Lecture audio impossible : SD absente"
    );

    return;
  }

  File audioFile =
    SD.open(
      path,
      FILE_READ
    );

  if (
    !audioFile
  ) {

    Serial.print(
      "Fichier audio absent : "
    );

    Serial.println(
      path
    );

    return;
  }

  Serial.print(
    "Lecture audio : "
  );

  Serial.println(
    path
  );

  if (
    audioFile.size() <=
    44
  ) {

    Serial.println(
      "Fichier WAV invalide"
    );

    audioFile.close();

    return;
  }

  audioFile.seek(
    44
  );

  uint8_t buffer[
    512
  ];

  while (
    audioFile.available()
  ) {

    int bytesRead =
      audioFile.read(
        buffer,
        sizeof(buffer)
      );

    if (
      bytesRead <=
      0
    ) {

      break;
    }

    I2S.write(
      buffer,
      bytesRead
    );

    if (
      client.connected()
    ) {

      client.loop();
    }

    yield();
  }

  audioFile.close();

  int16_t silence[
    128
  ] = {0};

  I2S.write(
    (uint8_t*)silence,
    sizeof(silence)
  );

  Serial.println(
    "Audio termine"
  );
}

/* =========================================================
   BIPS SUR LE HAUT-PARLEUR
   ========================================================= */
void playTone(
  int frequency,
  int durationMs
) {

  if (
    !audioReady
  ) {

    return;
  }

  int totalSamples =
    (
      AUDIO_SAMPLE_RATE *
      durationMs
    ) /
    1000;

  int16_t samples[
    128
  ];

  float phase =
    0.0;

  float phaseStep =
    (
      2.0 *
      PI *
      frequency
    ) /
    AUDIO_SAMPLE_RATE;

  int generated =
    0;

  while (
    generated <
    totalSamples
  ) {

    int count =
      totalSamples -
      generated;

    if (
      count >
      128
    ) {

      count =
        128;
    }

    for (
      int i = 0;
      i < count;
      i++
    ) {

      samples[i] =
        (int16_t)(
          sin(phase) *
          7000
        );

      phase +=
        phaseStep;

      if (
        phase >
        2.0 * PI
      ) {

        phase -=
          2.0 * PI;
      }
    }

    I2S.write(
      (uint8_t*)samples,
      count *
        sizeof(int16_t)
    );

    generated +=
      count;
  }

  int16_t silence[
    128
  ] = {0};

  I2S.write(
    (uint8_t*)silence,
    sizeof(silence)
  );
}

void beepShort() {

  playTone(
    1100,
    100
  );
}

void beepDouble() {

  playTone(
    700,
    90
  );

  delay(
    80
  );

  playTone(
    700,
    90
  );
}

/* =========================================================
   OUTILS SD
   ========================================================= */
void logSD(
  const String& text
) {

  if (
    !sdReady
  ) {

    return;
  }

  File file =
    SD.open(
      LOG_FILE,
      FILE_APPEND
    );

  if (
    !file
  ) {

    Serial.println(
      "Erreur ouverture log.txt"
    );

    return;
  }

  file.print(
    millis()
  );

  file.print(
    " | "
  );

  file.println(
    text
  );

  file.close();
}

bool writeFileSD(
  const char* path,
  const String& data
) {

  if (
    !sdReady
  ) {

    return false;
  }

  if (
    SD.exists(
      path
    )
  ) {

    SD.remove(
      path
    );
  }

  File file =
    SD.open(
      path,
      FILE_WRITE
    );

  if (
    !file
  ) {

    Serial.println(
      "Erreur ecriture SD : " +
      String(path)
    );

    return false;
  }

  file.print(
    data
  );

  file.close();

  Serial.println(
    "Fichier sauvegarde : " +
    String(path)
  );

  return true;
}

String readFileSD(
  const char* path
) {

  if (
    !sdReady
  ) {

    return "";
  }

  File file =
    SD.open(
      path,
      FILE_READ
    );

  if (
    !file
  ) {

    Serial.println(
      "Fichier absent : " +
      String(path)
    );

    return "";
  }

  String data =
    file.readString();

  file.close();

  return data;
}

/* =========================================================
   DATE ET HEURE
   ========================================================= */
bool timeIsValid() {

  time_t now;

  time(
    &now
  );

  return
    now >
    1700000000;
}

String getDateTimeString() {

  struct tm timeInfo;

  if (
    !getLocalTime(
      &timeInfo,
      20
    )
  ) {

    return "";
  }

  char buffer[
    25
  ];

  strftime(
    buffer,
    sizeof(buffer),
    "%Y-%m-%d %H:%M:%S",
    &timeInfo
  );

  return
    String(
      buffer
    );
}

void getDateAndTime(
  String& dateText,
  String& timeText
) {

  struct tm timeInfo;

  dateText =
    "";

  timeText =
    "";

  if (
    !getLocalTime(
      &timeInfo,
      20
    )
  ) {

    return;
  }

  char dateBuffer[
    15
  ];

  char timeBuffer[
    15
  ];

  strftime(
    dateBuffer,
    sizeof(dateBuffer),
    "%d/%m/%Y",
    &timeInfo
  );

  strftime(
    timeBuffer,
    sizeof(timeBuffer),
    "%H:%M:%S",
    &timeInfo
  );

  dateText =
    String(
      dateBuffer
    );

  timeText =
    String(
      timeBuffer
    );
}

String csvSafe(
  String value
) {

  value.replace(
    ";",
    ","
  );

  value.replace(
    "\r",
    " "
  );

  value.replace(
    "\n",
    " "
  );

  return value;
}

/* =========================================================
   CONTROLE DU TEMPS
   ========================================================= */
unsigned long nowEpoch() {

  time_t now;

  time(
    &now
  );

  return
    (unsigned long)now;
}

bool checkTimeAccess(
  unsigned long allowedFrom,
  unsigned long allowedTo
) {

  /*
     Aucun horaire défini :
     aucune restriction temporelle.
  */
  if (
    allowedFrom == 0 &&
    allowedTo == 0
  ) {

    return true;
  }

  /*
     Une restriction existe mais aucune
     heure fiable n'est disponible :
     accès refusé.
  */
  if (
    !timeIsValid()
  ) {

    Serial.println(
      "ACCES REFUSE : heure indisponible pour controle horaire"
    );

    publishMsg(
      topic("event"),
      "time_unavailable_access_denied",
      false
    );

    logSD(
      "ACCESS DENIED: TIME UNAVAILABLE"
    );

    return false;
  }

  unsigned long currentTime =
    nowEpoch();

  if (
    allowedFrom > 0 &&
    currentTime <
      allowedFrom
  ) {

    Serial.println(
      "ACCES REFUSE : periode pas encore commencee"
    );

    return false;
  }

  if (
    allowedTo > 0 &&
    currentTime >
      allowedTo
  ) {

    Serial.println(
      "ACCES REFUSE : periode terminee"
    );

    return false;
  }

  return true;
}

/* =========================================================
   HISTORIQUE CSV
   ========================================================= */
void logAccessHistory(
  const String& direction,
  const String& method,
  const String& identifier,
  const String& personName,
  const String& result,
  const String& dateText,
  const String& timeText
) {

  if (
    !sdReady
  ) {

    Serial.println(
      "Historique non enregistre : SD absente"
    );

    return;
  }

  bool newFile =
    !SD.exists(
      ACCESS_HISTORY_FILE
    );

  File file =
    SD.open(
      ACCESS_HISTORY_FILE,
      FILE_APPEND
    );

  if (
    !file
  ) {

    Serial.println(
      "Erreur ouverture access_history.csv"
    );

    return;
  }

  if (
    newFile
  ) {

    file.println(
      "Nom;Date;Heure;Direction;Methode;Identifiant;Resultat"
    );
  }

  file.print(
    csvSafe(
      personName
    )
  );

  file.print(
    ";"
  );

  file.print(
    dateText.length() > 0
      ? csvSafe(
          dateText
        )
      : "A_SYNCHRONISER"
  );

  file.print(
    ";"
  );

  file.print(
    timeText.length() > 0
      ? csvSafe(
          timeText
        )
      : "A_SYNCHRONISER"
  );

  file.print(
    ";"
  );

  file.print(
    csvSafe(
      direction
    )
  );

  file.print(
    ";"
  );

  file.print(
    csvSafe(
      method
    )
  );

  file.print(
    ";"
  );

  file.print(
    csvSafe(
      identifier
    )
  );

  file.print(
    ";"
  );

  file.println(
    csvSafe(
      result
    )
  );

  file.close();

  Serial.println(
    personName +
    " | " +
    (
      dateText.length() > 0
        ? dateText
        : "A_SYNCHRONISER"
    ) +
    " | " +
    (
      timeText.length() > 0
        ? timeText
        : "A_SYNCHRONISER"
    ) +
    " | " +
    direction
  );
}

/* =========================================================
   IDENTIFIANT UNIQUE
   ========================================================= */
String createEventId() {

  unsigned long sequence =
    prefs.getULong(
      "eventSeq",
      0
    );

  sequence++;

  prefs.putULong(
    "eventSeq",
    sequence
  );

  return
    deviceId +
    "-" +
    String(
      sequence
    );
}

/* =========================================================
   FILE HORS LIGNE
   ========================================================= */
bool appendPendingEvent(
  const String& jsonLine
) {

  if (
    !sdReady
  ) {

    Serial.println(
      "Impossible d'ajouter l'evenement : SD absente"
    );

    return false;
  }

  File file =
    SD.open(
      PENDING_EVENTS_FILE,
      FILE_APPEND
    );

  if (
    !file
  ) {

    Serial.println(
      "Erreur ouverture pending_events.ndjson"
    );

    return false;
  }

  file.println(
    jsonLine
  );

  file.close();

  Serial.println(
    "Evenement ajoute a la file hors ligne"
  );

  return true;
}

/* =========================================================
   ENREGISTREMENT EVENEMENT
   ========================================================= */
String recordAccessEvent(
  const String& direction,
  const String& method,
  const String& identifier,
  const String& personName,
  const String& result =
    "AUTHORIZED"
) {

  String dateText;
  String timeText;

  getDateAndTime(
    dateText,
    timeText
  );

  logAccessHistory(
    direction,
    method,
    identifier,
    personName,
    result,
    dateText,
    timeText
  );

  String eventId =
    createEventId();

  StaticJsonDocument<1024>
    document;

  document["eventId"] =
    eventId;

  document["deviceId"] =
    deviceId;

  document["door"] =
    doorId;

  document["direction"] =
    direction;

  document["method"] =
    method;

  document["identifier"] =
    identifier;

  document["name"] =
    personName;

  document["result"] =
    result;

  document["date"] =
    dateText.length() > 0
      ? dateText
      : "";

  document["time"] =
    timeText.length() > 0
      ? timeText
      : "";

  document["dateTime"] =
    getDateTimeString();

  document["millisAtEvent"] =
    millis();

  document["timeSynced"] =
    timeIsValid();

  document["rtcReady"] =
    rtcReady;

  String jsonLine;

  serializeJson(
    document,
    jsonLine
  );

  appendPendingEvent(
    jsonLine
  );

  lastSyncCheck =
    0;

  return eventId;
}

/* =========================================================
   LECTURE PREMIER EVENEMENT
   ========================================================= */
bool readFirstPendingEvent(
  String& jsonLine,
  String& eventId
) {

  jsonLine =
    "";

  eventId =
    "";

  if (
    !sdReady
  ) {

    return false;
  }

  if (
    !SD.exists(
      PENDING_EVENTS_FILE
    )
  ) {

    return false;
  }

  File file =
    SD.open(
      PENDING_EVENTS_FILE,
      FILE_READ
    );

  if (
    !file
  ) {

    return false;
  }

  while (
    file.available()
  ) {

    String line =
      file.readStringUntil(
        '\n'
      );

    line.trim();

    if (
      line.length() ==
      0
    ) {

      continue;
    }

    StaticJsonDocument<1024>
      document;

    DeserializationError error =
      deserializeJson(
        document,
        line
      );

    if (
      error
    ) {

      Serial.println(
        "Ligne NDJSON invalide ignoree"
      );

      continue;
    }

    String id =
      document[
        "eventId"
      ] | "";

    if (
      id.length() ==
      0
    ) {

      continue;
    }

    jsonLine =
      line;

    eventId =
      id;

    file.close();

    return true;
  }

  file.close();

  return false;
}

/* =========================================================
   SUPPRESSION APRES ACK
   ========================================================= */
bool removePendingEvent(
  const String& acknowledgedEventId
) {

  if (
    !sdReady
  ) {

    return false;
  }

  if (
    !SD.exists(
      PENDING_EVENTS_FILE
    )
  ) {

    return true;
  }

  File source =
    SD.open(
      PENDING_EVENTS_FILE,
      FILE_READ
    );

  if (
    !source
  ) {

    return false;
  }

  if (
    SD.exists(
      PENDING_TEMP_FILE
    )
  ) {

    SD.remove(
      PENDING_TEMP_FILE
    );
  }

  File temp =
    SD.open(
      PENDING_TEMP_FILE,
      FILE_WRITE
    );

  if (
    !temp
  ) {

    source.close();

    return false;
  }

  bool removed =
    false;

  while (
    source.available()
  ) {

    String line =
      source.readStringUntil(
        '\n'
      );

    line.trim();

    if (
      line.length() ==
      0
    ) {

      continue;
    }

    StaticJsonDocument<1024>
      document;

    DeserializationError error =
      deserializeJson(
        document,
        line
      );

    if (
      error
    ) {

      temp.println(
        line
      );

      continue;
    }

    String currentEventId =
      document[
        "eventId"
      ] | "";

    if (
      !removed &&
      currentEventId ==
        acknowledgedEventId
    ) {

      removed =
        true;

      continue;
    }

    temp.println(
      line
    );
  }

  source.close();

  temp.close();

  SD.remove(
    PENDING_EVENTS_FILE
  );

  if (
    !SD.rename(
      PENDING_TEMP_FILE,
      PENDING_EVENTS_FILE
    )
  ) {

    Serial.println(
      "Erreur remplacement pending_events.ndjson"
    );

    return false;
  }

  if (
    removed
  ) {

    Serial.println(
      "ACK recu, evenement supprime : " +
      acknowledgedEventId
    );
  }

  return removed;
}

/* =========================================================
   SYNCHRONISATION MQTT
   ========================================================= */
void processPendingEvents() {

  if (
    !sdReady
  ) {

    return;
  }

  if (
    !client.connected()
  ) {

    return;
  }

  unsigned long now =
    millis();

  if (
    now -
      lastSyncCheck <
    SYNC_CHECK_TIME
  ) {

    return;
  }

  lastSyncCheck =
    now;

  if (
    waitingAccessAck
  ) {

    if (
      now -
        lastSyncPublish >=
      SYNC_RETRY_TIME
    ) {

      bool published =
        client.publish(
          topic(
            "access_sync"
          ).c_str(),
          waitingEventJson.c_str(),
          false
        );

      if (
        published
      ) {

        lastSyncPublish =
          now;

        Serial.println(
          "Nouvelle tentative sync : " +
          waitingEventId
        );
      }
    }

    return;
  }

  String jsonLine;
  String eventId;

  if (
    !readFirstPendingEvent(
      jsonLine,
      eventId
    )
  ) {

    return;
  }

  bool published =
    client.publish(
      topic(
        "access_sync"
      ).c_str(),
      jsonLine.c_str(),
      false
    );

  if (
    !published
  ) {

    Serial.println(
      "Echec publication evenement en attente"
    );

    return;
  }

  waitingAccessAck =
    true;

  waitingEventId =
    eventId;

  waitingEventJson =
    jsonLine;

  lastSyncPublish =
    now;

  Serial.println(
    "Evenement envoye, attente ACK : " +
    eventId
  );
}

/* =========================================================
   ACK NODE-RED
   ========================================================= */
void handleAccessAck(
  const String& message
) {

  String acknowledgedId =
    message;

  if (
    message.startsWith(
      "{"
    )
  ) {

    StaticJsonDocument<256>
      document;

    DeserializationError error =
      deserializeJson(
        document,
        message
      );

    if (
      !error
    ) {

      acknowledgedId =
        document[
          "eventId"
        ] | "";
    }
  }

  acknowledgedId.trim();

  if (
    acknowledgedId.length() ==
    0
  ) {

    return;
  }

  if (
    !waitingAccessAck ||
    acknowledgedId !=
      waitingEventId
  ) {

    removePendingEvent(
      acknowledgedId
    );

    return;
  }

  if (
    removePendingEvent(
      acknowledgedId
    )
  ) {

    waitingAccessAck =
      false;

    waitingEventId =
      "";

    waitingEventJson =
      "";

    lastSyncPublish =
      0;

    lastSyncCheck =
      0;
  }
}

/* =========================================================
   CONFIGURATION SD
   ========================================================= */
void saveConfigToSD() {

  if (
    !sdReady
  ) {

    return;
  }

  StaticJsonDocument<512>
    document;

  document["deviceId"] =
    deviceId;

  document["doorId"] =
    doorId;

  String output;

  serializeJson(
    document,
    output
  );

  writeFileSD(
    CONFIG_FILE,
    output
  );
}

void initSD() {

  pinMode(
    SD_CS,
    OUTPUT
  );

  digitalWrite(
    SD_CS,
    HIGH
  );

  spiSD.begin(
    SD_SCK,
    SD_MISO,
    SD_MOSI,
    SD_CS
  );

  delay(
    100
  );

  if (
    !SD.begin(
      SD_CS,
      spiSD,
      4000000
    )
  ) {

    sdReady =
      false;

    Serial.println(
      "SD ERROR sur SPI separe"
    );

    return;
  }

  uint8_t cardType =
    SD.cardType();

  if (
    cardType ==
    CARD_NONE
  ) {

    sdReady =
      false;

    Serial.println(
      "Aucune carte SD detectee"
    );

    return;
  }

  sdReady =
    true;

  Serial.println(
    "SD OK sur SPI separe"
  );

  Serial.print(
    "Type carte SD : "
  );

  if (
    cardType ==
    CARD_MMC
  ) {

    Serial.println(
      "MMC"
    );
  }

  else if (
    cardType ==
    CARD_SD
  ) {

    Serial.println(
      "SDSC"
    );
  }

  else if (
    cardType ==
    CARD_SDHC
  ) {

    Serial.println(
      "SDHC"
    );
  }

  else {

    Serial.println(
      "INCONNU"
    );
  }

  uint64_t cardSizeMB =
    SD.cardSize() /
    (
      1024ULL *
      1024ULL
    );

  Serial.print(
    "Taille carte : "
  );

  Serial.print(
    cardSizeMB
  );

  Serial.println(
    " MB"
  );

  File file =
    SD.open(
      LOG_FILE,
      FILE_APPEND
    );

  if (
    file
  ) {

    file.println(
      "===== ESP START ====="
    );

    file.println(
      "Device ID: " +
      deviceId
    );

    file.println(
      "Door ID: " +
      doorId
    );

    file.close();
  }

  saveConfigToSD();
}

/* =========================================================
   PUBLICATION MQTT
   ========================================================= */
void publishMsg(
  const String& mqttTopic,
  const String& message,
  bool retained = false
) {

  if (
    !client.connected()
  ) {

    return;
  }

  client.publish(
    mqttTopic.c_str(),
    message.c_str(),
    retained
  );
}

String stateToString() {

  switch (
    state
  ) {

    case READY:
      return "READY";

    case OPEN:
      return "OPEN";

    case WAIT:
      return "WAIT";

    case REFUSED:
      return "REFUSED";

    default:
      return "UNKNOWN";
  }
}

/* =========================================================
   LEDS MCP23017
   ========================================================= */
void updateLeds() {

  if (
    !mcpReady
  ) {

    Serial.println(
      "LED ERROR : MCP23017 indisponible"
    );

    return;
  }

  /*
     PRIORITE MAXIMA :
     alarme température.
  */
  if (
    temperatureAlarmActive
  ) {

    mcp.digitalWrite(
      MCP_LED_GREEN,
      LOW
    );

    mcp.digitalWrite(
      MCP_LED_RED,
      temperatureAlarmLedOn
        ? HIGH
        : LOW
    );

    return;
  }

  mcp.digitalWrite(
    MCP_LED_GREEN,
    LOW
  );

  mcp.digitalWrite(
    MCP_LED_RED,
    LOW
  );

  /*
     Verrouillage SAS ou accès refusé :
     rouge.
  */
  if (
    relationLocked ||
    state ==
      REFUSED
  ) {

    mcp.digitalWrite(
      MCP_LED_RED,
      HIGH
    );

    return;
  }

  /*
     READY / OPEN :
     vert.
  */
  if (
    state ==
      READY ||
    state ==
      OPEN
  ) {

    mcp.digitalWrite(
      MCP_LED_GREEN,
      HIGH
    );

    return;
  }

  /*
     WAIT :
     rouge.
  */
  if (
    state ==
      WAIT
  ) {

    mcp.digitalWrite(
      MCP_LED_RED,
      HIGH
    );

    return;
  }
}

/* =========================================================
   ALARME TEMPERATURE : ETAT CONNEXION
   ========================================================= */
bool systemIsOfflineForAlarm() {

  /*
     On considère le système hors ligne
     si WiFi OU MQTT n'est pas opérationnel.
  */
  return !(
    WiFi.status() ==
      WL_CONNECTED &&
    client.connected()
  );
}

/* =========================================================
   ALARME TEMPERATURE : ACTIVATION / ARRET
   ========================================================= */
void setTemperatureAlarm(
  bool enabled
) {

  if (
    enabled ==
    temperatureAlarmActive
  ) {

    return;
  }

  temperatureAlarmActive =
    enabled;

  if (
    enabled
  ) {

    Serial.println();
    Serial.println(
      "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
    );

    Serial.println(
      "ALARME TEMPERATURE ACTIVE"
    );

    Serial.print(
      "Temperature : "
    );

    Serial.print(
      lastTemp,
      1
    );

    Serial.println(
      " C"
    );

    Serial.println(
      "SYSTEME HORS LIGNE"
    );

    Serial.println(
      "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
    );

    temperatureAlarmLedOn =
      true;

    temperatureAlarmToneHigh =
      false;

    lastTemperatureAlarmBlink =
      millis();

    lastTemperatureAlarmSound =
      millis();

    logSD(
      "TEMP ALARM START : " +
      String(
        lastTemp,
        1
      ) +
      " C"
    );

    publishMsg(
      topic(
        "temp_alarm"
      ),
      "ALARM",
      true
    );

    publishMsg(
      topic(
        "event"
      ),
      "temperature_alarm_started",
      false
    );

    updateLeds();

    /*
       Premier signal sonore immédiatement.
    */
    playTone(
      1300,
      250
    );
  }

  else {

    Serial.println();
    Serial.println(
      "ALARME TEMPERATURE ARRETEE"
    );

    Serial.print(
      "Temperature actuelle : "
    );

    Serial.print(
      lastTemp,
      1
    );

    Serial.println(
      " C"
    );

    logSD(
      "TEMP ALARM STOP : " +
      String(
        lastTemp,
        1
      ) +
      " C"
    );

    publishMsg(
      topic(
        "temp_alarm"
      ),
      "NORMAL",
      true
    );

    publishMsg(
      topic(
        "event"
      ),
      "temperature_alarm_stopped",
      false
    );

    temperatureAlarmLedOn =
      false;

    temperatureAlarmToneHigh =
      false;

    updateLeds();
  }
}

/* =========================================================
   ALARME TEMPERATURE : GESTION
   ========================================================= */
void handleTemperatureAlarm() {

  /*
     Pas encore de température valide.
  */
  if (
    isnan(
      lastTemp
    )
  ) {

    return;
  }

  bool offline =
    systemIsOfflineForAlarm();

  /*
     Déclenchement.
  */
  if (
    !temperatureAlarmActive
  ) {

    if (
      offline &&
      lastTemp >=
        TEMP_ALARM_ON
    ) {

      setTemperatureAlarm(
        true
      );
    }

    return;
  }

  /*
     Une fois active, l'alarme s'arrête
     si :
     - la température redescend à 48°C ou moins,
     - OU la connexion complète revient.
  */
  if (
    !offline ||
    lastTemp <=
      TEMP_ALARM_OFF
  ) {

    setTemperatureAlarm(
      false
    );

    return;
  }

  unsigned long now =
    millis();

  /*
     Clignotement LED rouge.
  */
  if (
    now -
      lastTemperatureAlarmBlink >=
    TEMP_ALARM_BLINK_TIME
  ) {

    lastTemperatureAlarmBlink =
      now;

    temperatureAlarmLedOn =
      !temperatureAlarmLedOn;

    updateLeds();
  }

  /*
     Alarme sonore répétitive.
  */
  if (
    now -
      lastTemperatureAlarmSound >=
    TEMP_ALARM_SOUND_TIME
  ) {

    lastTemperatureAlarmSound =
      now;

    temperatureAlarmToneHigh =
      !temperatureAlarmToneHigh;

    playTone(
      temperatureAlarmToneHigh
        ? 1400
        : 850,
      250
    );
  }
}

/* =========================================================
   BASES RFID / EMPREINTES
   ========================================================= */
void clearRfidDb() {

  for (
    int i = 0;
    i < MAX_RFID_USERS;
    i++
  ) {

    rfidDb[i].uid[0] =
      '\0';

    rfidDb[i].name[0] =
      '\0';

    rfidDb[i].allowedDoors[0] =
      '\0';

    rfidDb[i].enabled =
      false;

    rfidDb[i].allowedFrom =
      0;

    rfidDb[i].allowedTo =
      0;

    rfidDb[i].maxUses =
      0;

    rfidDb[i].usedCount =
      0;

    rfidDb[i].used =
      false;
  }
}

void clearFingerDb() {

  for (
    int i = 0;
    i < MAX_FINGER_USERS;
    i++
  ) {

    fingerDb[i].id =
      -1;

    fingerDb[i].name[0] =
      '\0';

    fingerDb[i].allowedDoors[0] =
      '\0';

    fingerDb[i].enabled =
      false;

    fingerDb[i].allowedFrom =
      0;

    fingerDb[i].allowedTo =
      0;

    fingerDb[i].maxUses =
      0;

    fingerDb[i].usedCount =
      0;

    fingerDb[i].used =
      false;
  }
}

String normalizeUid(
  String uid
) {

  uid.trim();
  uid.toUpperCase();

  String output =
    "";

  for (
    int i = 0;
    i < uid.length();
    i++
  ) {

    char character =
      uid[i];

    if (
      (
        character >= '0' &&
        character <= '9'
      ) ||
      (
        character >= 'A' &&
        character <= 'F'
      )
    ) {

      output +=
        character;
    }
  }

  return output;
}

int findRfid(
  const String& uid
) {

  String normalized =
    normalizeUid(
      uid
    );

  for (
    int i = 0;
    i < MAX_RFID_USERS;
    i++
  ) {

    if (
      !rfidDb[i].used
    ) {

      continue;
    }

    if (
      normalizeUid(
        String(
          rfidDb[i].uid
        )
      ) ==
      normalized
    ) {

      return i;
    }
  }

  return -1;
}

int findFinger(
  int id
) {

  for (
    int i = 0;
    i < MAX_FINGER_USERS;
    i++
  ) {

    if (
      !fingerDb[i].used
    ) {

      continue;
    }

    if (
      fingerDb[i].id ==
      id
    ) {

      return i;
    }
  }

  return -1;
}

/* =========================================================
   PORTES AUTORISEES
   ========================================================= */
bool isDoorAllowedForCard(
  int index
) {

  String allowedDoors =
    String(
      rfidDb[index]
        .allowedDoors
    );

  allowedDoors.trim();

  if (
    allowedDoors.length() ==
    0
  ) {

    return true;
  }

  if (
    allowedDoors.indexOf(
      "ALL"
    ) >=
    0
  ) {

    return true;
  }

  return
    allowedDoors.indexOf(
      doorId
    ) >=
    0;
}

bool isDoorAllowedForFinger(
  int index
) {

  String allowedDoors =
    String(
      fingerDb[index]
        .allowedDoors
    );

  allowedDoors.trim();

  if (
    allowedDoors.length() ==
    0
  ) {

    return true;
  }

  if (
    allowedDoors.indexOf(
      "ALL"
    ) >=
    0
  ) {

    return true;
  }

  return
    allowedDoors.indexOf(
      doorId
    ) >=
    0;
}

/* =========================================================
   PUBLICATION DE TOUS LES ETATS
   ========================================================= */
void publishAll() {

  publishMsg(
    topic("state"),
    stateToString(),
    true
  );

  publishMsg(
    topic("door"),
    digitalRead(
      MC38_PIN
    ) ==
      MC38_OPEN
        ? "OPEN"
        : "CLOSED",
    true
  );

  publishMsg(
    topic("pir"),
    digitalRead(
      PIR_PIN
    ) ==
      PIR_ACTIVE
        ? "DETECTED"
        : "CLEAR",
    true
  );

  publishMsg(
    topic(
      "lock_status"
    ),
    relationLocked
      ? "LOCKED"
      : "UNLOCKED",
    true
  );

  publishMsg(
    topic(
      "sd_status"
    ),
    sdReady
      ? "OK"
      : "ERROR",
    true
  );

  publishMsg(
    topic(
      "rtc_status"
    ),
    rtcReady
      ? "OK"
      : "ERROR",
    true
  );

  publishMsg(
    topic(
      "temp_alarm"
    ),
    temperatureAlarmActive
      ? "ALARM"
      : "NORMAL",
    true
  );

  publishMsg(
    "sas/" +
      deviceId +
      "/info",
    doorId,
    true
  );

  publishMsg(
    "sas/" +
      deviceId +
      "/status",
    WiFi.status() ==
      WL_CONNECTED
        ? "online"
        : "offline",
    true
  );

  if (
    !isnan(
      lastTemp
    )
  ) {

    publishMsg(
      topic("temp"),
      String(
        lastTemp,
        1
      ),
      true
    );
  }

  if (
    !isnan(
      lastHum
    )
  ) {

    publishMsg(
      topic("hum"),
      String(
        lastHum,
        1
      ),
      true
    );
  }
}

/* =========================================================
   LOGIQUE PORTE
   ========================================================= */
bool canOpenDoor() {

  if (
    relationLocked
  ) {

    Serial.println(
      "REFUSED CAUSE : relationLocked = true"
    );

    publishMsg(
      topic("event"),
      "refused_relation_locked",
      false
    );

    logSD(
      "REFUSED CAUSE: relationLocked"
    );

    return false;
  }

  if (
    state !=
    READY
  ) {

    Serial.println(
      "REFUSED CAUSE : state = " +
      stateToString()
    );

    logSD(
      "REFUSED CAUSE: state=" +
      stateToString()
    );

    return false;
  }

  if (
    digitalRead(
      MC38_PIN
    ) ==
    MC38_OPEN
  ) {

    Serial.println(
      "REFUSED CAUSE : MC38 indique OPEN"
    );

    publishMsg(
      topic("event"),
      "refused_door_already_open",
      false
    );

    logSD(
      "REFUSED CAUSE: MC38 OPEN"
    );

    return false;
  }

  if (
    USE_GLOBAL_DOOR_STATE_LOCK &&
    otherDoorBusy
  ) {

    Serial.println(
      "REFUSED CAUSE : otherDoorBusy = true"
    );

    publishMsg(
      topic("event"),
      "refused_other_door_busy",
      false
    );

    logSD(
      "REFUSED CAUSE: otherDoorBusy"
    );

    return false;
  }

  return true;
}

/* =========================================================
   ACCES REFUSE
   ========================================================= */
void setRefused(
  const String& reason
) {

  state =
    REFUSED;

  refusedStart =
    millis();

  publishMsg(
    topic("state"),
    "REFUSED",
    true
  );

  publishMsg(
    topic("event"),
    "open_refused_" +
      reason,
    false
  );

  Serial.println(
    "PASSAGE REFUSED : " +
    reason
  );

  logSD(
    "REFUSED: " +
    reason
  );

  updateLeds();

  beepDouble();

  playAudioFile(
    AUDIO_ACCESS_DENIED
  );
}

/* =========================================================
   OUVERTURE PORTE
   ========================================================= */
bool openDoor(
  const String& source
) {

  if (
    !canOpenDoor()
  ) {

    setRefused(
      source
    );

    return false;
  }

  doorWasPhysicallyOpen =
    false;

  state =
    OPEN;

  relayActive =
    true;

  relayStart =
    millis();

  digitalWrite(
    RELAY,
    RELAY_ON
  );

  publishMsg(
    topic("state"),
    "OPEN",
    true
  );

  publishMsg(
    topic("auth"),
    source,
    true
  );

  publishMsg(
    topic("event"),
    "door_opened_by_" +
      source,
    false
  );

  Serial.println(
    "PORTE OUVERTE PAR : " +
    source
  );

  logSD(
    "DOOR OPEN: " +
    source
  );

  updateLeds();

  beepShort();

  playAudioFile(
    AUDIO_DOOR_OPEN
  );

  return true;
}

/* =========================================================
   FERMETURE / WAIT
   ========================================================= */
void startWaitMode() {

  digitalWrite(
    RELAY,
    RELAY_OFF
  );

  relayActive =
    false;

  state =
    WAIT;

  waitStart =
    millis();

  lastWaitRemain =
    -2;

  publishMsg(
    topic("state"),
    "WAIT",
    true
  );

  publishMsg(
    topic("event"),
    "door_closed_wait",
    false
  );

  logSD(
    "DOOR CLOSED -> WAIT"
  );

  updateLeds();

  beepDouble();

  playAudioFile(
    AUDIO_DOOR_CLOSED
  );
}

/* =========================================================
   RESET PORTE
   ========================================================= */
void resetDoor() {

  digitalWrite(
    RELAY,
    RELAY_OFF
  );

  relayActive =
    false;

  doorWasPhysicallyOpen =
    false;

  state =
    READY;

  lastWaitRemain =
    -2;

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
    "reset_done",
    false
  );

  logSD(
    "RESET"
  );

  updateLeds();
}

/* =========================================================
   MACHINE D'ETAT
   ========================================================= */
void handleStateMachine() {

  unsigned long now =
    millis();

  if (
    relayActive &&
    now -
      relayStart >=
    RELAY_TIME
  ) {

    digitalWrite(
      RELAY,
      RELAY_OFF
    );

    relayActive =
      false;
  }

  if (
    state ==
    OPEN
  ) {

    int currentDoorState =
      digitalRead(
        MC38_PIN
      );

    if (
      currentDoorState ==
      MC38_OPEN
    ) {

      doorWasPhysicallyOpen =
        true;
    }

    if (
      doorWasPhysicallyOpen &&
      currentDoorState ==
        MC38_CLOSED
    ) {

      doorWasPhysicallyOpen =
        false;

      startWaitMode();
    }
  }

  if (
    state ==
    WAIT
  ) {

    unsigned long elapsed =
      now -
      waitStart;

    unsigned long limitedElapsed =
      elapsed >
        WAIT_TIME
          ? WAIT_TIME
          : elapsed;

    long remainingSeconds =
      (
        WAIT_TIME -
        limitedElapsed
      ) /
      1000;

    if (
      remainingSeconds <
      0
    ) {

      remainingSeconds =
        0;
    }

    if (
      remainingSeconds !=
      lastWaitRemain
    ) {

      lastWaitRemain =
        remainingSeconds;

      publishMsg(
        topic("wait"),
        String(
          remainingSeconds
        ),
        true
      );
    }

    if (
      elapsed >=
      WAIT_TIME
    ) {

      state =
        READY;

      lastWaitRemain =
        -2;

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
        "state_ready",
        false
      );

      updateLeds();
    }
  }

  if (
    state ==
      REFUSED &&
    now -
      refusedStart >=
    REFUSED_TIME
  ) {

    state =
      READY;

    publishMsg(
      topic("state"),
      "READY",
      true
    );

    updateLeds();
  }
}

/* =========================================================
   BOUTON UNIQUE GPA2
   ========================================================= */
bool buttonPressedEdge() {

  if (
    !mcpReady
  ) {

    return false;
  }

  int reading =
    mcp.digitalRead(
      MCP_BUTTON
    );

  unsigned long now =
    millis();

  if (
    reading !=
    buttonCandidateState
  ) {

    buttonCandidateState =
      reading;

    buttonCandidateTime =
      now;
  }

  if (
    buttonCandidateState !=
      buttonStableState &&
    now -
      buttonCandidateTime >=
    BUTTON_DEBOUNCE
  ) {

    int previousState =
      buttonStableState;

    buttonStableState =
      buttonCandidateState;

    return (
      previousState ==
        HIGH &&
      buttonStableState ==
        LOW
    );
  }

  return false;
}

void handleButtons() {

  if (
    !buttonPressedEdge()
  ) {

    return;
  }

  int physicalDoorState =
    digitalRead(
      MC38_PIN
    );

  Serial.println(
    "===== BOUTON GPA2 ====="
  );

  Serial.print(
    "MC38 : "
  );

  Serial.println(
    physicalDoorState ==
      MC38_CLOSED
        ? "PORTE FERMEE"
        : "PORTE OUVERTE"
  );

  if (
    physicalDoorState ==
    MC38_CLOSED
  ) {

    Serial.println(
      "Bouton : demande OUVERTURE"
    );

    if (
      openDoor(
        "BUTTON_OPEN"
      )
    ) {

      recordAccessEvent(
        "OUVERTURE",
        "BOUTON",
        "BUTTON_OPEN",
        "Ouverture manuelle",
        "AUTHORIZED"
      );
    }

    return;
  }

  Serial.println(
    "Bouton : demande FERMETURE"
  );

  digitalWrite(
    RELAY,
    RELAY_OFF
  );

  relayActive =
    false;

  state =
    OPEN;

  doorWasPhysicallyOpen =
    true;

  publishMsg(
    topic("state"),
    "OPEN",
    true
  );

  publishMsg(
    topic("event"),
    "manual_close_requested",
    false
  );

  logSD(
    "BUTTON: CLOSE REQUEST"
  );

  updateLeds();
}

/* =========================================================
   RFID ENTREE / SORTIE
   ========================================================= */
String readUidString(
  MFRC522& reader
) {

  String uid =
    "";

  for (
    byte i = 0;
    i < reader.uid.size;
    i++
  ) {

    if (
      reader.uid.uidByte[i] <
      0x10
    ) {

      uid +=
        "0";
    }

    uid +=
      String(
        reader.uid.uidByte[i],
        HEX
      );

    if (
      i <
      reader.uid.size - 1
    ) {

      uid +=
        ":";
    }
  }

  uid.toUpperCase();

  return uid;
}

void handleOneRfid(
  MFRC522& reader,
  const String& direction
) {

  if (
    !reader.PICC_IsNewCardPresent()
  ) {

    return;
  }

  if (
    !reader.PICC_ReadCardSerial()
  ) {

    return;
  }

  String uid =
    readUidString(
      reader
    );

  Serial.print(
    "RFID "
  );

  Serial.print(
    direction
  );

  Serial.print(
    " UID : "
  );

  Serial.println(
    uid
  );

  int index =
    findRfid(
      uid
    );

  /* RFID inconnu */
  if (
    index < 0
  ) {

    publishMsg(
      topic("rfid_result"),
      "UNKNOWN",
      false
    );

    String dateText;
    String timeText;

    getDateAndTime(
      dateText,
      timeText
    );

    logAccessHistory(
      direction,
      "RFID",
      uid,
      "RFID inconnu",
      "UNKNOWN",
      dateText,
      timeText
    );

    setRefused(
      "rfid_unknown_" +
      direction
    );
  }

  /* RFID désactivé */
  else if (
    !rfidDb[index].enabled
  ) {

    publishMsg(
      topic("rfid_result"),
      "DISABLED",
      false
    );

    String dateText;
    String timeText;

    getDateAndTime(
      dateText,
      timeText
    );

    logAccessHistory(
      direction,
      "RFID",
      uid,
      String(
        rfidDb[index].name
      ),
      "DISABLED",
      dateText,
      timeText
    );

    setRefused(
      "rfid_disabled_" +
      direction
    );
  }

  /* Horaire */
  else if (
    !checkTimeAccess(
      rfidDb[index].allowedFrom,
      rfidDb[index].allowedTo
    )
  ) {

    publishMsg(
      topic("rfid_result"),
      "TIME_DENIED",
      false
    );

    String dateText;
    String timeText;

    getDateAndTime(
      dateText,
      timeText
    );

    logAccessHistory(
      direction,
      "RFID",
      uid,
      String(
        rfidDb[index].name
      ),
      "TIME_DENIED",
      dateText,
      timeText
    );

    setRefused(
      "rfid_time_denied_" +
      direction
    );
  }

  /* Limite utilisations */
  else if (
    rfidDb[index].maxUses > 0 &&
    rfidDb[index].usedCount >=
      rfidDb[index].maxUses
  ) {

    publishMsg(
      topic("rfid_result"),
      "USE_LIMIT_REACHED",
      false
    );

    String dateText;
    String timeText;

    getDateAndTime(
      dateText,
      timeText
    );

    logAccessHistory(
      direction,
      "RFID",
      uid,
      String(
        rfidDb[index].name
      ),
      "USE_LIMIT_REACHED",
      dateText,
      timeText
    );

    setRefused(
      "rfid_use_limit_" +
      direction
    );
  }

  /* Porte non autorisée */
  else if (
    !isDoorAllowedForCard(
      index
    )
  ) {

    publishMsg(
      topic("rfid_result"),
      "DOOR_NOT_ALLOWED",
      false
    );

    String dateText;
    String timeText;

    getDateAndTime(
      dateText,
      timeText
    );

    logAccessHistory(
      direction,
      "RFID",
      uid,
      String(
        rfidDb[index].name
      ),
      "DOOR_NOT_ALLOWED",
      dateText,
      timeText
    );

    setRefused(
      "door_not_allowed_" +
      direction
    );
  }

  /* Autorisé */
  else {

    String source =
      "RFID_" +
      direction;

    if (
      openDoor(
        source
      )
    ) {

      rfidDb[index]
        .usedCount++;

      publishMsg(
        topic("rfid_result"),
        "AUTHORIZED",
        false
      );

      if (
        direction ==
        "ENTREE"
      ) {

        publishMsg(
          topic("rfid_consume"),
          uid,
          false
        );

        publishMsg(
          topic("rfid_entry"),
          uid,
          false
        );

        publishMsg(
          topic("access_direction"),
          "IN",
          false
        );

        publishMsg(
          topic("event"),
          "rfid_entry_authorized",
          false
        );
      }

      else if (
        direction ==
        "SORTIE"
      ) {

        publishMsg(
          topic("rfid_exit"),
          uid,
          false
        );

        publishMsg(
          topic("access_direction"),
          "OUT",
          false
        );

        publishMsg(
          topic("event"),
          "rfid_exit_authorized",
          false
        );
      }

      recordAccessEvent(
        direction,
        "RFID",
        uid,
        String(
          rfidDb[index].name
        ),
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

  uint8_t result =
    finger.getImage();

  if (
    result ==
    FINGERPRINT_NOFINGER
  ) {

    return;
  }

  if (
    result !=
    FINGERPRINT_OK
  ) {

    return;
  }

  if (
    finger.image2Tz() !=
    FINGERPRINT_OK
  ) {

    return;
  }

  result =
    finger.fingerSearch();

  if (
    result !=
    FINGERPRINT_OK
  ) {

    publishMsg(
      topic("fingerprint_result"),
      "UNKNOWN",
      false
    );

    String dateText;
    String timeText;

    getDateAndTime(
      dateText,
      timeText
    );

    logAccessHistory(
      "ENTREE",
      "EMPREINTE",
      "INCONNU",
      "Empreinte inconnue",
      "UNKNOWN",
      dateText,
      timeText
    );

    setRefused(
      "finger_unknown"
    );

    return;
  }

  int fingerId =
    finger.fingerID;

  Serial.print(
    "Empreinte detectee ID : "
  );

  Serial.println(
    fingerId
  );

  publishMsg(
    topic("fingerprint"),
    String(fingerId),
    false
  );

  publishMsg(
    topic("event"),
    "finger_detected",
    false
  );

  int index =
    findFinger(
      fingerId
    );

  if (
    index < 0
  ) {

    publishMsg(
      topic("fingerprint_result"),
      "UNKNOWN",
      false
    );

    String dateText;
    String timeText;

    getDateAndTime(
      dateText,
      timeText
    );

    logAccessHistory(
      "ENTREE",
      "EMPREINTE",
      String(fingerId),
      "Empreinte absente de la base",
      "UNKNOWN",
      dateText,
      timeText
    );

    setRefused(
      "finger_not_in_db"
    );
  }

  else if (
    !fingerDb[index].enabled
  ) {

    publishMsg(
      topic("fingerprint_result"),
      "DISABLED",
      false
    );

    String dateText;
    String timeText;

    getDateAndTime(
      dateText,
      timeText
    );

    logAccessHistory(
      "ENTREE",
      "EMPREINTE",
      String(fingerId),
      String(
        fingerDb[index].name
      ),
      "DISABLED",
      dateText,
      timeText
    );

    setRefused(
      "finger_disabled"
    );
  }

  else if (
    !checkTimeAccess(
      fingerDb[index].allowedFrom,
      fingerDb[index].allowedTo
    )
  ) {

    publishMsg(
      topic("fingerprint_result"),
      "TIME_DENIED",
      false
    );

    String dateText;
    String timeText;

    getDateAndTime(
      dateText,
      timeText
    );

    logAccessHistory(
      "ENTREE",
      "EMPREINTE",
      String(fingerId),
      String(
        fingerDb[index].name
      ),
      "TIME_DENIED",
      dateText,
      timeText
    );

    setRefused(
      "finger_time_denied"
    );
  }

  else if (
    fingerDb[index].maxUses > 0 &&
    fingerDb[index].usedCount >=
      fingerDb[index].maxUses
  ) {

    publishMsg(
      topic("fingerprint_result"),
      "USE_LIMIT_REACHED",
      false
    );

    String dateText;
    String timeText;

    getDateAndTime(
      dateText,
      timeText
    );

    logAccessHistory(
      "ENTREE",
      "EMPREINTE",
      String(fingerId),
      String(
        fingerDb[index].name
      ),
      "USE_LIMIT_REACHED",
      dateText,
      timeText
    );

    setRefused(
      "finger_use_limit"
    );
  }

  else if (
    !isDoorAllowedForFinger(
      index
    )
  ) {

    publishMsg(
      topic("fingerprint_result"),
      "DOOR_NOT_ALLOWED",
      false
    );

    String dateText;
    String timeText;

    getDateAndTime(
      dateText,
      timeText
    );

    logAccessHistory(
      "ENTREE",
      "EMPREINTE",
      String(fingerId),
      String(
        fingerDb[index].name
      ),
      "DOOR_NOT_ALLOWED",
      dateText,
      timeText
    );

    setRefused(
      "door_not_allowed"
    );
  }

  else {

    if (
      openDoor(
        "FINGER"
      )
    ) {

      fingerDb[index]
        .usedCount++;

      publishMsg(
        topic("fingerprint_result"),
        "AUTHORIZED",
        false
      );

      publishMsg(
        topic("fingerprint_consume"),
        String(fingerId),
        false
      );

      publishMsg(
        topic("access_direction"),
        "IN",
        false
      );

      publishMsg(
        topic("event"),
        "finger_entry_authorized",
        false
      );

      recordAccessEvent(
        "ENTREE",
        "EMPREINTE",
        String(fingerId),
        String(
          fingerDb[index].name
        ),
        "AUTHORIZED"
      );
    }
  }
}

/* =========================================================
   CAPTEURS
   ========================================================= */
void handleSensors() {

  static int lastDoorState =
    -1;

  static int lastPirState =
    -1;

  int currentDoorState =
    digitalRead(
      MC38_PIN
    );

  int currentPirState =
    digitalRead(
      PIR_PIN
    );

  if (
    currentDoorState !=
    lastDoorState
  ) {

    lastDoorState =
      currentDoorState;

    publishMsg(
      topic("door"),
      currentDoorState ==
        MC38_OPEN
          ? "OPEN"
          : "CLOSED",
      true
    );
  }

  if (
    currentPirState !=
    lastPirState
  ) {

    lastPirState =
      currentPirState;

    publishMsg(
      topic("pir"),
      currentPirState ==
        PIR_ACTIVE
          ? "DETECTED"
          : "CLEAR",
      true
    );
  }

  if (
    millis() -
      lastDhtRead >=
    DHT_INTERVAL
  ) {

    lastDhtRead =
      millis();

    float humidity =
      dht.readHumidity();

    float temperature =
      dht.readTemperature();

    if (
      !isnan(
        temperature
      )
    ) {

      lastTemp =
        temperature;

      publishMsg(
        topic("temp"),
        String(
          temperature,
          1
        ),
        true
      );
    }

    if (
      !isnan(
        humidity
      )
    ) {

      lastHum =
        humidity;

      publishMsg(
        topic("hum"),
        String(
          humidity,
          1
        ),
        true
      );
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

  StaticJsonDocument<16384>
    document;

  DeserializationError error =
    deserializeJson(
      document,
      json
    );

  if (
    error
  ) {

    Serial.print(
      "RFID JSON error : "
    );

    Serial.println(
      error.c_str()
    );

    publishMsg(
      topic("event"),
      "rfid_db_parse_error",
      false
    );

    return;
  }

  clearRfidDb();

  JsonArray array =
    document.as<JsonArray>();

  int index =
    0;

  for (
    JsonObject object :
    array
  ) {

    if (
      index >=
      MAX_RFID_USERS
    ) {

      break;
    }

    String uid =
      normalizeUid(
        object["uid"] | ""
      );

    String name =
      object["name"] | "";

    if (
      uid.length() ==
      0
    ) {

      continue;
    }

    String allowedDoors =
      "ALL";

    if (
      object["allowedDoors"]
        .is<JsonArray>()
    ) {

      allowedDoors =
        "";

      JsonArray doorsArray =
        object["allowedDoors"]
          .as<JsonArray>();

      for (
        String allowedDoor :
        doorsArray
      ) {

        if (
          allowedDoors.length() >
          0
        ) {

          allowedDoors +=
            ",";
        }

        allowedDoors +=
          allowedDoor;
      }
    }

    strncpy(
      rfidDb[index].uid,
      uid.c_str(),
      sizeof(
        rfidDb[index].uid
      ) - 1
    );

    rfidDb[index].uid[
      sizeof(
        rfidDb[index].uid
      ) - 1
    ] =
      '\0';

    strncpy(
      rfidDb[index].name,
      name.c_str(),
      sizeof(
        rfidDb[index].name
      ) - 1
    );

    rfidDb[index].name[
      sizeof(
        rfidDb[index].name
      ) - 1
    ] =
      '\0';

    strncpy(
      rfidDb[index]
        .allowedDoors,
      allowedDoors.c_str(),
      sizeof(
        rfidDb[index]
          .allowedDoors
      ) - 1
    );

    rfidDb[index]
      .allowedDoors[
        sizeof(
          rfidDb[index]
            .allowedDoors
        ) - 1
      ] =
        '\0';

    rfidDb[index].enabled =
      object["enabled"] | true;

    rfidDb[index].allowedFrom =
      object["allowedFrom"] | 0UL;

    rfidDb[index].allowedTo =
      object["allowedTo"] | 0UL;

    rfidDb[index].maxUses =
      object["maxUses"] | 0;

    rfidDb[index].usedCount =
      object["usedCount"] | 0;

    rfidDb[index].used =
      true;

    index++;
  }

  Serial.print(
    "RFID users loaded : "
  );

  Serial.println(
    index
  );

  if (
    saveToSd &&
    sdReady
  ) {

    writeFileSD(
      RFID_FILE,
      json
    );
  }

  publishMsg(
    topic("event"),
    "rfid_db_loaded",
    false
  );
}

/* =========================================================
   CHARGEMENT BASE EMPREINTES
   ========================================================= */
void loadFingerDb(
  const String& json,
  bool saveToSd = true
) {

  StaticJsonDocument<16384>
    document;

  DeserializationError error =
    deserializeJson(
      document,
      json
    );

  if (
    error
  ) {

    Serial.print(
      "Finger JSON error : "
    );

    Serial.println(
      error.c_str()
    );

    publishMsg(
      topic("event"),
      "finger_db_parse_error",
      false
    );

    return;
  }

  clearFingerDb();

  JsonArray array =
    document.as<JsonArray>();

  int index =
    0;

  for (
    JsonObject object :
    array
  ) {

    if (
      index >=
      MAX_FINGER_USERS
    ) {

      break;
    }

    int fingerId =
      object["fingerId"] | -1;

    String name =
      object["name"] | "";

    if (
      fingerId < 0
    ) {

      continue;
    }

    String allowedDoors =
      "ALL";

    if (
      object["allowedDoors"]
        .is<JsonArray>()
    ) {

      allowedDoors =
        "";

      JsonArray doorsArray =
        object["allowedDoors"]
          .as<JsonArray>();

      for (
        String allowedDoor :
        doorsArray
      ) {

        if (
          allowedDoors.length() >
          0
        ) {

          allowedDoors +=
            ",";
        }

        allowedDoors +=
          allowedDoor;
      }
    }

    fingerDb[index].id =
      fingerId;

    strncpy(
      fingerDb[index].name,
      name.c_str(),
      sizeof(
        fingerDb[index].name
      ) - 1
    );

    fingerDb[index].name[
      sizeof(
        fingerDb[index].name
      ) - 1
    ] =
      '\0';

    strncpy(
      fingerDb[index]
        .allowedDoors,
      allowedDoors.c_str(),
      sizeof(
        fingerDb[index]
          .allowedDoors
      ) - 1
    );

    fingerDb[index]
      .allowedDoors[
        sizeof(
          fingerDb[index]
            .allowedDoors
        ) - 1
      ] =
        '\0';

    fingerDb[index].enabled =
      object["enabled"] | true;

    fingerDb[index].allowedFrom =
      object["allowedFrom"] | 0UL;

    fingerDb[index].allowedTo =
      object["allowedTo"] | 0UL;

    fingerDb[index].maxUses =
      object["maxUses"] | 0;

    fingerDb[index].usedCount =
      object["usedCount"] | 0;

    fingerDb[index].used =
      true;

    index++;
  }

  Serial.print(
    "Finger users loaded : "
  );

  Serial.println(
    index
  );

  if (
    saveToSd &&
    sdReady
  ) {

    writeFileSD(
      FINGER_FILE,
      json
    );
  }

  publishMsg(
    topic("event"),
    "finger_db_loaded",
    false
  );
}

/* =========================================================
   CHARGEMENT BASES DEPUIS SD
   ========================================================= */
void loadDatabasesFromSD() {

  if (
    !sdReady
  ) {

    Serial.println(
      "SD indisponible : bases non chargees"
    );

    return;
  }

  String rfidJson =
    readFileSD(
      RFID_FILE
    );

  if (
    rfidJson.length() >
    5
  ) {

    loadRfidDb(
      rfidJson,
      false
    );
  }

  else {

    Serial.println(
      "RFID DB absente ou vide"
    );
  }

  String fingerJson =
    readFileSD(
      FINGER_FILE
    );

  if (
    fingerJson.length() >
    5
  ) {

    loadFingerDb(
      fingerJson,
      false
    );
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
void enrollFinger(
  int id
) {

  if (
    id < 1 ||
    id > 127
  ) {

    publishMsg(
      topic(
        "finger_enroll_result"
      ),
      "ERROR:invalid_id",
      true
    );

    return;
  }

  publishMsg(
    topic(
      "finger_enroll_status"
    ),
    "put_finger",
    true
  );

  unsigned long start =
    millis();

  while (
    finger.getImage() !=
    FINGERPRINT_OK
  ) {

    if (
      client.connected()
    ) {

      client.loop();
    }

    if (
      millis() -
        start >
      30000
    ) {

      publishMsg(
        topic(
          "finger_enroll_result"
        ),
        "ERROR:timeout_first_image",
        true
      );

      return;
    }

    delay(
      50
    );
  }

  if (
    finger.image2Tz(1) !=
    FINGERPRINT_OK
  ) {

    publishMsg(
      topic(
        "finger_enroll_result"
      ),
      "ERROR:first_image",
      true
    );

    return;
  }

  publishMsg(
    topic(
      "finger_enroll_status"
    ),
    "remove_finger",
    true
  );

  delay(
    2000
  );

  start =
    millis();

  while (
    finger.getImage() !=
    FINGERPRINT_NOFINGER
  ) {

    if (
      client.connected()
    ) {

      client.loop();
    }

    if (
      millis() -
        start >
      15000
    ) {

      publishMsg(
        topic(
          "finger_enroll_result"
        ),
        "ERROR:remove_timeout",
        true
      );

      return;
    }

    delay(
      50
    );
  }

  publishMsg(
    topic(
      "finger_enroll_status"
    ),
    "put_same_finger",
    true
  );

  start =
    millis();

  while (
    finger.getImage() !=
    FINGERPRINT_OK
  ) {

    if (
      client.connected()
    ) {

      client.loop();
    }

    if (
      millis() -
        start >
      30000
    ) {

      publishMsg(
        topic(
          "finger_enroll_result"
        ),
        "ERROR:timeout_second_image",
        true
      );

      return;
    }

    delay(
      50
    );
  }

  if (
    finger.image2Tz(2) !=
    FINGERPRINT_OK
  ) {

    publishMsg(
      topic(
        "finger_enroll_result"
      ),
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
      topic(
        "finger_enroll_result"
      ),
      "ERROR:create_model",
      true
    );

    return;
  }

  if (
    finger.storeModel(
      id
    ) !=
    FINGERPRINT_OK
  ) {

    publishMsg(
      topic(
        "finger_enroll_result"
      ),
      "ERROR:store_model",
      true
    );

    return;
  }

  publishMsg(
    topic(
      "finger_enroll_status"
    ),
    "finger_saved",
    true
  );

  publishMsg(
    topic(
      "finger_enroll_result"
    ),
    "SUCCESS:" +
      String(id),
    true
  );

  logSD(
    "FINGER ENROLLED: " +
    String(id)
  );
}

void deleteFinger(
  int id
) {

  if (
    finger.deleteModel(
      id
    ) ==
    FINGERPRINT_OK
  ) {

    publishMsg(
      topic(
        "finger_enroll_result"
      ),
      "DELETED:" +
        String(id),
      true
    );

    logSD(
      "FINGER DELETED: " +
      String(id)
    );
  }

  else {

    publishMsg(
      topic(
        "finger_enroll_result"
      ),
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
      topic(
        "finger_enroll_result"
      ),
      "CLEARED_ALL",
      true
    );

    logSD(
      "FINGER DATABASE CLEARED"
    );
  }

  else {

    publishMsg(
      topic(
        "finger_enroll_result"
      ),
      "ERROR:clear_all",
      true
    );
  }
}

/* =========================================================
   VALIDATION NOM
   ========================================================= */
bool validName(
  String name
) {

  name.trim();

  if (
    name.length() ==
    0
  ) {

    return false;
  }

  if (
    name.indexOf("/") >= 0 ||
    name.indexOf("+") >= 0 ||
    name.indexOf("#") >= 0
  ) {

    return false;
  }

  return true;
}

/* =========================================================
   MQTT CALLBACK
   ========================================================= */
void mqttCallback(
  char* receivedTopic,
  byte* payload,
  unsigned int length
) {

  String message =
    "";

  for (
    unsigned int i = 0;
    i < length;
    i++
  ) {

    message +=
      (char)payload[i];
  }

  message.trim();

  String mqttTopic =
    String(
      receivedTopic
    );

  Serial.print(
    "MQTT RECU : "
  );

  Serial.print(
    mqttTopic
  );

  Serial.print(
    " = "
  );

  Serial.println(
    message
  );

  /* ---------- ACK ---------- */

  if (
    mqttTopic ==
    topic("access_ack")
  ) {

    handleAccessAck(
      message
    );

    return;
  }

  /* ---------- Changement nom ---------- */

  if (
    mqttTopic ==
      "sas/" +
      deviceId +
      "/config/name" ||
    mqttTopic ==
      "sas/" +
      doorId +
      "/config/name"
  ) {

    if (
      validName(
        message
      )
    ) {

      prefs.putString(
        "doorId",
        message
      );

      doorId =
        message;

      saveConfigToSD();

      publishMsg(
        "sas/" +
          deviceId +
          "/config/status",
        "name_saved_restart",
        true
      );

      delay(
        500
      );

      ESP.restart();
    }

    else {

      publishMsg(
        "sas/" +
          deviceId +
          "/config/status",
        "invalid_name",
        true
      );
    }
  }

  /* ---------- Commande porte ---------- */

  else if (
    mqttTopic ==
    topic("cmd")
  ) {

    if (
      message ==
      "OPEN"
    ) {

      if (
        openDoor(
          "CMD"
        )
      ) {

        recordAccessEvent(
          "OUVERTURE",
          "MQTT",
          "CMD_OPEN",
          "Commande distante",
          "AUTHORIZED"
        );
      }
    }

    else if (
      message ==
      "CLOSE"
    ) {

      if (
        state ==
        OPEN
      ) {

        startWaitMode();
      }
    }

    else if (
      message ==
      "RESET"
    ) {

      resetDoor();
    }
  }

  /* ---------- Verrouillage Node-RED ---------- */

  else if (
    mqttTopic ==
    topic("lock")
  ) {

    if (
      message ==
      "LOCKED"
    ) {

      relationLocked =
        true;

      Serial.println(
        "RELATION SAS : LOCKED"
      );

      publishMsg(
        topic("event"),
        "relation_locked",
        false
      );

      publishMsg(
        topic("lock_status"),
        "LOCKED",
        true
      );

      logSD(
        "RELATION LOCKED"
      );

      beepDouble();

      updateLeds();
    }

    else if (
      message ==
      "UNLOCKED"
    ) {

      relationLocked =
        false;

      Serial.println(
        "RELATION SAS : UNLOCKED"
      );

      publishMsg(
        topic("event"),
        "relation_unlocked",
        false
      );

      publishMsg(
        topic("lock_status"),
        "UNLOCKED",
        true
      );

      logSD(
        "RELATION UNLOCKED"
      );

      beepShort();

      updateLeds();
    }

    else {

      Serial.println(
        "Valeur /lock ignoree : " +
        message
      );
    }
  }

  /* ---------- Base RFID ---------- */

  else if (
    mqttTopic ==
    "sas/rfid/db"
  ) {

    loadRfidDb(
      message,
      true
    );
  }

  /* ---------- Base empreintes ---------- */

  else if (
    mqttTopic ==
    "sas/finger/db"
  ) {

    loadFingerDb(
      message,
      true
    );
  }

  /* ---------- Commandes empreintes ---------- */

  else if (
    mqttTopic ==
    topic("finger/cmd")
  ) {

    if (
      message.startsWith(
        "ENROLL:"
      )
    ) {

      enrollFinger(
        message
          .substring(7)
          .toInt()
      );
    }

    else if (
      message.startsWith(
        "DELETE:"
      )
    ) {

      deleteFinger(
        message
          .substring(7)
          .toInt()
      );
    }

    else if (
      message ==
      "CLEAR_ALL"
    ) {

      clearFingerSensor();
    }
  }

  /* ---------- Etat global optionnel ---------- */

  else if (
    USE_GLOBAL_DOOR_STATE_LOCK &&
    mqttTopic.startsWith(
      "sas/"
    ) &&
    mqttTopic.endsWith(
      "/state"
    )
  ) {

    String otherDoor =
      mqttTopic.substring(
        4,
        mqttTopic.length() - 6
      );

    if (
      otherDoor !=
        doorId &&
      otherDoor !=
        deviceId
    ) {

      Serial.print(
        "ETAT AUTRE PORTE : "
      );

      Serial.print(
        otherDoor
      );

      Serial.print(
        " = "
      );

      Serial.println(
        message
      );

      if (
        message ==
          "OPEN" ||
        message ==
          "WAIT"
      ) {

        otherDoorBusy =
          true;
      }

      else if (
        message ==
          "READY" ||
        message ==
          "REFUSED"
      ) {

        otherDoorBusy =
          false;
      }
    }
  }
}

/* =========================================================
   WIFI NON BLOQUANT
   ========================================================= */
void startWifi() {

  WiFi.mode(
    WIFI_STA
  );

  WiFi.persistent(
    false
  );

  WiFi.setAutoReconnect(
    true
  );

  WiFi.begin(
    ssid,
    password
  );

  lastWifiReconnect =
    millis();

  Serial.println(
    "Connexion WiFi lancee en arriere-plan"
  );
}

void maintainWifi() {

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    return;
  }

  unsigned long now =
    millis();

  if (
    now -
      lastWifiReconnect <
    WIFI_RECONNECT_TIME
  ) {

    return;
  }

  lastWifiReconnect =
    now;

  wl_status_t wifiStatus =
    WiFi.status();

  if (
    wifiStatus ==
      WL_DISCONNECTED ||
    wifiStatus ==
      WL_CONNECTION_LOST ||
    wifiStatus ==
      WL_NO_SSID_AVAIL
  ) {

    WiFi.reconnect();

    Serial.println(
      "Nouvelle tentative WiFi non bloquante"
    );
  }
}

/* =========================================================
   RECONNEXION MQTT
   ========================================================= */
void reconnectMqtt() {

  if (
    WiFi.status() !=
    WL_CONNECTED
  ) {

    return;
  }

  if (
    client.connected()
  ) {

    return;
  }

  String clientId =
    "SAS_" +
    deviceId +
    "_" +
    String(
      (uint32_t)millis(),
      HEX
    );

  Serial.print(
    "Connexion MQTT..."
  );

  if (
    !client.connect(
      clientId.c_str()
    )
  ) {

    Serial.print(
      "Erreur : "
    );

    Serial.println(
      client.state()
    );

    return;
  }

  Serial.println(
    "OK"
  );

  relationLocked =
    false;

  otherDoorBusy =
    false;

  client.subscribe(
    topic("cmd").c_str()
  );

  client.subscribe(
    topic("finger/cmd").c_str()
  );

  client.subscribe(
    topic("lock").c_str()
  );

  client.subscribe(
    topic("access_ack").c_str()
  );

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

  client.subscribe(
    "sas/rfid/db"
  );

  client.subscribe(
    "sas/finger/db"
  );

  if (
    USE_GLOBAL_DOOR_STATE_LOCK
  ) {

    client.subscribe(
      "sas/+/state"
    );
  }

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
    topic("rtc_status"),
    rtcReady
      ? "OK"
      : "ERROR",
    true
  );

  publishMsg(
    topic("temp_alarm"),
    temperatureAlarmActive
      ? "ALARM"
      : "NORMAL",
    true
  );

  publishMsg(
    "sas/" +
      deviceId +
      "/info",
    doorId,
    true
  );

  publishMsg(
    "sas/" +
      deviceId +
      "/status",
    "online",
    true
  );

  publishAll();

  updateLeds();

  waitingAccessAck =
    false;

  waitingEventId =
    "";

  waitingEventJson =
    "";

  lastSyncPublish =
    0;

  lastSyncCheck =
    0;
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

  pinMode(
    RFID_IN_SS,
    OUTPUT
  );

  pinMode(
    RFID_OUT_SS,
    OUTPUT
  );

  digitalWrite(
    RFID_IN_SS,
    HIGH
  );

  digitalWrite(
    RFID_OUT_SS,
    HIGH
  );

  delay(
    100
  );

  rfidIn.PCD_Init();

  delay(
    100
  );

  byte versionIn =
    rfidIn.PCD_ReadRegister(
      MFRC522::VersionReg
    );

  Serial.print(
    "RFID ENTREE version : 0x"
  );

  Serial.println(
    versionIn,
    HEX
  );

  digitalWrite(
    RFID_IN_SS,
    HIGH
  );

  digitalWrite(
    RFID_OUT_SS,
    HIGH
  );

  rfidOut.PCD_Init();

  delay(
    100
  );

  byte versionOut =
    rfidOut.PCD_ReadRegister(
      MFRC522::VersionReg
    );

  Serial.print(
    "RFID SORTIE version : 0x"
  );

  Serial.println(
    versionOut,
    HEX
  );

  if (
    versionIn ==
      0x00 ||
    versionIn ==
      0xFF
  ) {

    Serial.println(
      "ERREUR RFID ENTREE"
    );
  }

  else {

    Serial.println(
      "RFID ENTREE OK"
    );
  }

  if (
    versionOut ==
      0x00 ||
    versionOut ==
      0xFF
  ) {

    Serial.println(
      "ERREUR RFID SORTIE"
    );
  }

  else {

    Serial.println(
      "RFID SORTIE OK"
    );
  }
}

/* =========================================================
   SETUP
   ========================================================= */
void setup() {

  Serial.begin(
    115200
  );

  delay(
    500
  );

  Serial.println();

  Serial.println(
    "=============================="
  );

  Serial.println(
    "DEMARRAGE SYSTEME SAS"
  );

  Serial.println(
    "=============================="
  );

  prefs.begin(
    "sas_config",
    false
  );

  deviceId =
    makeDeviceId();

  doorId =
    prefs.getString(
      "doorId",
      deviceId
    );

  Serial.println(
    "Device ID : " +
    deviceId
  );

  Serial.println(
    "Door ID : " +
    doorId
  );

  clearRfidDb();

  clearFingerDb();

  /* =======================================================
     GPIO ESP32
     ======================================================= */

  pinMode(
    RELAY,
    OUTPUT
  );

  pinMode(
    MC38_PIN,
    INPUT
  );

  pinMode(
    PIR_PIN,
    INPUT
  );

  digitalWrite(
    RELAY,
    RELAY_OFF
  );

  /* =======================================================
     MCP23017 + I2C
     ======================================================= */

  initMcp23017();

  scanI2CBus();

  /* =======================================================
     RTC DS3231
     ======================================================= */

  initRtc();

  /* =======================================================
     RFID
     ======================================================= */

  initRfidReaders();

  /* =======================================================
     SD
     ======================================================= */

  initSD();

  loadDatabasesFromSD();

  /* =======================================================
     VERIFICATION FICHIERS AUDIO
     ======================================================= */

  if (
    sdReady
  ) {

    Serial.println(
      "Verification fichiers audio :"
    );

    Serial.print(
      AUDIO_DOOR_OPEN
    );

    Serial.println(
      SD.exists(
        AUDIO_DOOR_OPEN
      )
        ? " : OK"
        : " : ABSENT"
    );

    Serial.print(
      AUDIO_DOOR_CLOSED
    );

    Serial.println(
      SD.exists(
        AUDIO_DOOR_CLOSED
      )
        ? " : OK"
        : " : ABSENT"
    );

    Serial.print(
      AUDIO_ACCESS_DENIED
    );

    Serial.println(
      SD.exists(
        AUDIO_ACCESS_DENIED
      )
        ? " : OK"
        : " : ABSENT"
    );
  }

  /* =======================================================
     MAX98357A
     ======================================================= */

  initAudio();

  /* =======================================================
     EMPREINTE
     ======================================================= */

  fingerSerial.begin(
    57600,
    SERIAL_8N1,
    FINGER_RX,
    FINGER_TX
  );

  finger.begin(
    57600
  );

  delay(
    100
  );

  if (
    finger.verifyPassword()
  ) {

    Serial.println(
      "Capteur empreinte OK"
    );
  }

  else {

    Serial.println(
      "Capteur empreinte ERROR"
    );
  }

  /* =======================================================
     DHT11
     ======================================================= */

  dht.begin();

  /* =======================================================
     MQTT
     ======================================================= */

  client.setServer(
    mqtt_server,
    mqtt_port
  );

  client.setCallback(
    mqttCallback
  );

  client.setBufferSize(
    4096
  );

  client.setSocketTimeout(
    2
  );

  /* =======================================================
     NTP
     ======================================================= */

  configTime(
    gmtOffset_sec,
    daylightOffset_sec,
    ntpServer1,
    ntpServer2
  );

  /* =======================================================
     WIFI
     ======================================================= */

  startWifi();

  /* =======================================================
     LEDS
     ======================================================= */

  updateLeds();

  Serial.println();

  Serial.println(
    "Setup termine"
  );

  Serial.println(
    "LED verte/rouge via MCP23017"
  );

  Serial.println(
    "Bouton unique via GPA2"
  );

  Serial.println(
    "Audio via MAX98357A"
  );

  Serial.println(
    rtcReady
      ? "RTC DS3231 : OK"
      : "RTC DS3231 : ERROR"
  );

  Serial.println(
    timeIsValid()
      ? "HEURE SYSTEME : VALIDE"
      : "HEURE SYSTEME : INVALIDE"
  );

  Serial.println(
    "ALARME TEMPERATURE HORS LIGNE : ACTIVE"
  );

  Serial.print(
    "Declenchement : "
  );

  Serial.print(
    TEMP_ALARM_ON,
    1
  );

  Serial.println(
    " C"
  );

  Serial.print(
    "Arret : "
  );

  Serial.print(
    TEMP_ALARM_OFF,
    1
  );

  Serial.println(
    " C ou retour connexion"
  );
}

/* =========================================================
   LOOP
   ========================================================= */
void loop() {

  unsigned long now =
    millis();

  /* =======================================================
     WIFI
     ======================================================= */

  maintainWifi();

  /* =======================================================
     RTC
     ======================================================= */

  maintainRtc();

  /* =======================================================
     MQTT
     ======================================================= */

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    if (
      !client.connected() &&
      now -
        lastMqttReconnect >=
      MQTT_RECONNECT_TIME
    ) {

      lastMqttReconnect =
        now;

      reconnectMqtt();
    }

    if (
      client.connected()
    ) {

      client.loop();
    }
  }

  /* =======================================================
     MACHINE PORTE
     ======================================================= */

  handleStateMachine();

  /* =======================================================
     CAPTEURS
     ======================================================= */

  handleSensors();

  /* =======================================================
     ALARME TEMPERATURE HORS LIGNE
     ======================================================= */

  handleTemperatureAlarm();

  /* =======================================================
     BOUTON
     ======================================================= */

  handleButtons();

  /* =======================================================
     RFID + EMPREINTE
     ======================================================= */

  if (
    state ==
    READY
  ) {

    handleRfid();

    handleFinger();
  }

  /* =======================================================
     EVENEMENTS HORS LIGNE
     ======================================================= */

  processPendingEvents();

  /* =======================================================
     HEARTBEAT
     ======================================================= */

  if (
    now -
      lastHeartbeat >=
    HEARTBEAT_TIME
  ) {

    lastHeartbeat =
      now;

    publishMsg(
      topic("status"),
      WiFi.status() ==
        WL_CONNECTED
          ? "online"
          : "offline",
      true
    );

    publishMsg(
      "sas/" +
        deviceId +
        "/status",
      WiFi.status() ==
        WL_CONNECTED
          ? "online"
          : "offline",
      true
    );

    publishAll();

    updateLeds();
  }

  delay(
    2
  );
}

