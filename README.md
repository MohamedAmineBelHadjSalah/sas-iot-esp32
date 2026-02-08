# 🚪 SAS IoT ESP32

## 🎯 Objectif
Système SAS intelligent basé sur ESP32 :
- Contrôle d’accès RFID (RC522)
- Gestion sécurisée double porte (2 servos)
- Température & humidité (DHT11)
- Affichage LCD I2C
- Communication IoT via MQTT
- Supervision via dashboard Web

---

## 🧰 Matériel
- ESP32
- RFID RC522
- DHT11 (4 broches + résistance pull‑up 5k–10k)
- LCD I2C (0x27)
- 2 servomoteurs
- 2 boutons
- 4 LEDs (2 rouges, 2 vertes)
- Buzzer

---

## 🔌 Pinout (câblage actuel)
| Élément | GPIO |
|---|---:|
| Servo Porte 1 | 4 |
| Servo Porte 2 | 14 |
| LED Rouge Porte 1 | 2 |
| LED Verte Porte 1 | 12 |
| LED Rouge Porte 2 | 15 |
| LED Verte Porte 2 | 5 |
| Bouton Porte 1 | 33 |
| Bouton Porte 2 | 27 |
| Buzzer | 32 |
| DHT11 DATA | 17 |
| LCD SDA | 21 |
| LCD SCL | 22 |
| RFID SDA (SS) | 25 |
| RFID RST | 26 |
| RFID SCK | 18 |
| RFID MISO | 19 |
| RFID MOSI | 23 |

---

## 📡 MQTT
**Broker:** `test.mosquitto.org`  
**Port:** `1883`

### Topics publiés
- `sas/door1` (OPEN/CLOSED/BLOCKED)
- `sas/door2` (OPEN/CLOSED/BLOCKED)
- `sas/state` (INIT/P1_OPEN/P2_OPEN/WAIT_AFTER_CLOSE)
- `sas/temp`
- `sas/hum`
- `sas/access` (RFID_OK/RFID_REFUSE)
- `sas/countdown`

---

## ▶️ Installation rapide
1. Arduino IDE + carte ESP32
2. Installer les libs :
   - PubSubClient
   - MFRC522
   - DHT sensor library (+ Adafruit Unified Sensor)
   - ESP32Servo
   - LiquidCrystal_I2C
3. Modifier dans le code :
```cpp
const char* ssid = "TON_WIFI";
const char* password = "TON_MDP";
