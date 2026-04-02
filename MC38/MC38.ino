#define DOOR_SENSOR 14   // GPIO connecté au capteur

void setup() {
  Serial.begin(115200);

  pinMode(DOOR_SENSOR, INPUT_PULLUP); // activation pull-up interne

  Serial.println("Systeme de surveillance de porte demarre...");
}

void loop() {

  int state = digitalRead(DOOR_SENSOR);

  if (state == LOW) {
    Serial.println("Porte Fermee");
  } 
  else {
    Serial.println("Porte Ouverte");
  }

  delay(2000); // lecture chaque 2 secondes
}