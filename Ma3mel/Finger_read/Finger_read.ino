#include <Adafruit_Fingerprint.h>

HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

uint8_t id;

uint8_t enrollFingerprint(uint8_t id);

void setup() {
  Serial.begin(115200);
  delay(1000);

  // RX ESP32 = 16, TX ESP32 = 17
  fingerSerial.begin(57600, SERIAL_8N1, 16, 17);
  finger.begin(57600);

  if (finger.verifyPassword()) {
    Serial.println("Capteur empreinte OK");
  } else {
    Serial.println("Capteur non detecte");
    while (true) {
      delay(10);
    }
  }

  Serial.println("Tape un ID dans le Serial Monitor puis entree");
  Serial.println("Exemple: 1");
}

void loop() {
  if (Serial.available()) {
    id = Serial.parseInt();

    while (Serial.available()) {
      Serial.read();
    }

    if (id > 0) {
      Serial.print("Enregistrement de l'empreinte ID #");
      Serial.println(id);

      if (enrollFingerprint(id) == 1) {
        Serial.println("Empreinte enregistree avec succes");
      } else {
        Serial.println("Echec enregistrement");
      }

      Serial.println("Tape un autre ID si besoin");
    }
  }
}

uint8_t enrollFingerprint(uint8_t id) {
  int p = -1;

  Serial.println("Pose le doigt...");
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();

    if (p == FINGERPRINT_OK) {
      Serial.println("Image 1 prise");
    } else if (p == FINGERPRINT_NOFINGER) {
      delay(50);
    } else {
      Serial.println("Erreur lecture image 1");
      return 0;
    }
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    Serial.println("Erreur conversion image 1");
    return 0;
  }

  Serial.println("Retire le doigt...");
  delay(2000);

  while (finger.getImage() != FINGERPRINT_NOFINGER) {
    delay(50);
  }

  Serial.println("Repose le meme doigt...");
  p = -1;

  while (p != FINGERPRINT_OK) {
    p = finger.getImage();

    if (p == FINGERPRINT_OK) {
      Serial.println("Image 2 prise");
    } else if (p == FINGERPRINT_NOFINGER) {
      delay(50);
    } else {
      Serial.println("Erreur lecture image 2");
      return 0;
    }
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    Serial.println("Erreur conversion image 2");
    return 0;
  }

  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    Serial.println("Les deux empreintes ne correspondent pas");
    return 0;
  }

  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.println("Modele stocke");
    return 1;
  } else {
    Serial.println("Erreur stockage");
    return 0;
  }
}