/*
  PHOENIX - ESP32 DevKit Car Controller - FINAL v2
  
  FIXES in this version:
  1. PWM frequency changed from 20000Hz to 1000Hz
     20kHz was causing L298N to not respond to reverse direction
     L298N datasheet max switching frequency = ~40kHz but 
     in practice 1kHz is most reliable for direction changes
     
  2. Struct uses __attribute__((packed)) to prevent padding issues
     between ESP32-C3 (remote) and ESP32 DevKit (car)

  3. Direction change now stops motors for 10ms before reversing
     This prevents L298N shoot-through current when switching direction

  WIRING:
    Right motors: OUT1/OUT2 → IN1=G18, IN2=G19, ENA=G14
    Left  motors: OUT3/OUT4 → IN3=G25, IN4=G26, ENB=G13
    TRIG=G27, ECHO=G33, DHT22=G4, MQ5=G34, LED=G2
    Serial2: RX=G16, TX=G17
*/

#include <esp_now.h>
#include <WiFi.h>
#include <DHT.h>

// ===== MOTOR PINS =====
#define IN1  18
#define IN2  19
#define ENA  14   // Right motors speed

#define IN3  25
#define IN4  26
#define ENB  13   // Left motors speed

// ===== SENSOR PINS =====
#define TRIG_PIN  27
#define ECHO_PIN  33
#define DHT_PIN    4
#define MQ5_PIN   34
#define LED_PIN    2

// ===== PWM — 5kHz works reliably while keeping hum higher out of audible range =====
#define PWM_FREQ  5000     
#define PWM_RES   8        // 8-bit: 0-255

// ===== REMOTE MAC =====
uint8_t remoteMac[] = {0xAC, 0xA7, 0x04, 0xBC, 0xB4, 0xBC};

// ===== STRUCTS — packed prevents padding mismatch between C3 and DevKit =====
typedef struct __attribute__((packed)) {
  int  leftSpeed;
  int  rightSpeed;
  bool stopCommand;
  bool spinCommand;
} RemoteCommand;

typedef struct __attribute__((packed)) {
  int   distance;
  float temperature;
  int   gasValue;
  char  dangerLevel[10];
  bool  obstacleDetected;
  bool  mq5Warmed;
} CarData;

RemoteCommand remoteCmd;
CarData       carData;

// ===== SENSOR STATE =====
#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);

float  temperature      = 25.0;
int    distance         = 400;
int    gasValue         = 0;
String dangerLevel      = "LOW";
bool   obstacleDetected = false;
bool   mq5Warmed        = false;

unsigned long mq5Start    = 0;
unsigned long lastDHTRead = 0;
const int     MQ5_THRESHOLD = 2800;

// ===== SPIN STATE =====
bool          spinActive = false;
unsigned long spinStart  = 0;
const unsigned long SPIN_MS = 900UL;

// ===== LAST DIRECTION (for shoot-through protection) =====
int lastL = 0;
int lastR = 0;

// ===== CONNECTION =====
volatile unsigned long lastRemoteMsg   = 0;
volatile bool          remoteConnected = false;
const unsigned long    REMOTE_TIMEOUT  = 2500;

// ===================================================
// MOTOR CONTROL
// ===================================================
void motorSpeed(uint8_t pin, int val) {
  ledcWrite(pin, constrain(abs(val), 0, 255));
}

void stopMotors() {
  motorSpeed(ENA, 0);
  motorSpeed(ENB, 0);
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}

void brakeMotors() {
  digitalWrite(IN1, HIGH); digitalWrite(IN2, HIGH); motorSpeed(ENA, 255);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, HIGH); motorSpeed(ENB, 255);
}

void setRight(int speed) {
  // Brief stop when switching direction to protect L298N
  if ((lastR > 0 && speed < 0) || (lastR < 0 && speed > 0)) {
    motorSpeed(ENA, 0);
    delay(10);
  }
  if      (speed > 0) { digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);  }
  else if (speed < 0) { digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH); }
  else                { digitalWrite(IN1, LOW);  digitalWrite(IN2, LOW);  }
  motorSpeed(ENA, abs(speed));
  lastR = speed;
}

void setLeft(int speed) {
  if ((lastL > 0 && speed < 0) || (lastL < 0 && speed > 0)) {
    motorSpeed(ENB, 0);
    delay(10);
  }
  if      (speed > 0) { digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);  }
  else if (speed < 0) { digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH); }
  else                { digitalWrite(IN3, LOW);  digitalWrite(IN4, LOW);  }
  motorSpeed(ENB, abs(speed));
  lastL = speed;
}

void updateMotors() {
  int L = remoteCmd.leftSpeed;
  int R = remoteCmd.rightSpeed;

  // Active electronic brake forward when obstacle < 30cm
  if (obstacleDetected && L > 0 && R > 0) {
    brakeMotors();
    Serial.println("OBSTACLE: forward blocked & actively braked");
    return;
  }

  setRight(R);
  setLeft(L);

  // Debug every 500ms
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 500) {
    String dir = "IDLE";
    if      (L > 30  && R > 30)  dir = "FORWARD";
    else if (L < -30 && R < -30) dir = "BACKWARD";
    else if (R > L + 30)         dir = "TURN RIGHT";
    else if (L > R + 30)         dir = "TURN LEFT";
    else if (L != 0 || R != 0)   dir = "MOVING";
    Serial.printf("MOTOR> %-12s  L:%5d  R:%5d\n", dir.c_str(), L, R);
    lastPrint = millis();
  }
}

// ===================================================
// SENSORS
// ===================================================
int getDistance() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long dur = pulseIn(ECHO_PIN, HIGH, 30000UL);
  if (dur == 0) return 400;
  return constrain((int)(dur * 0.0343f / 2.0f), 2, 400);
}

void readSensors() {
  distance = getDistance();
  if (millis() - lastDHTRead >= 2000) {
    float t = dht.readTemperature();
    if (!isnan(t)) temperature = t;
    lastDHTRead = millis();
  }
  gasValue = analogRead(MQ5_PIN);
}

void updateDangerLevel() {
  bool gasHigh  = mq5Warmed && (gasValue > MQ5_THRESHOLD);
  bool tempHigh = (temperature > 55.0);
  bool tempWarm = (temperature > 38.0);
  bool closeObj = (distance < 30 && distance > 2);
  obstacleDetected = closeObj;
  if      (gasHigh || tempHigh)  dangerLevel = "HIGH";
  else if (tempWarm || closeObj) dangerLevel = "MED";
  else                           dangerLevel = "LOW";
}

void sendDataToCAM() {
  Serial2.printf("DIST:%03d,TEMP:%05.1f,GAS:%04d,DANGER:%s\n",
    distance, temperature, gasValue, dangerLevel.c_str());
}

// ===================================================
// ESP-NOW
// ===================================================
void addRemotePeer() {
  esp_now_del_peer(remoteMac);
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, remoteMac, 6);
  p.channel = 0; p.encrypt = false;
  Serial.println(esp_now_add_peer(&p) == ESP_OK ? "Peer OK" : "Peer FAILED");
}

void initESPNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(); delay(100);
  if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW FAILED"); return; }
  Serial.println("ESP-NOW OK");

  esp_now_register_send_cb(
    [](const wifi_tx_info_t* i, esp_now_send_status_t s) {}
  );

  esp_now_register_recv_cb(
    [](const esp_now_recv_info_t* info, const uint8_t* data, int len) {
      if (len == sizeof(RemoteCommand)) {
        memcpy(&remoteCmd, data, sizeof(remoteCmd));
        lastRemoteMsg   = millis();
        remoteConnected = true;
      } else {
        Serial.printf("SIZE MISMATCH: got=%d expected=%d\n",
          len, sizeof(RemoteCommand));
      }
    }
  );

  addRemotePeer();
}

// ===================================================
// SETUP
// ===================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== PHOENIX Car FINAL v2 ===");
  Serial.printf("RemoteCommand size: %d bytes\n", sizeof(RemoteCommand));
  Serial.printf("CarData size:       %d bytes\n", sizeof(CarData));

  Serial2.begin(115200, SERIAL_8N1, 16, 17);

  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, HIGH);
  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // SDK 3.x PWM — 1kHz reliable for L298N forward AND backward
  ledcAttach(ENA, PWM_FREQ, PWM_RES);
  ledcAttach(ENB, PWM_FREQ, PWM_RES);
  stopMotors();

  dht.begin();
  mq5Start = millis();
  Serial.println("MQ-5 warming up (60s)...");

  initESPNow();

  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, LOW);  delay(120);
    digitalWrite(LED_PIN, HIGH); delay(120);
  }

  Serial.println("Car ready — waiting for remote...");
  Serial.printf("MAC: %s\n", WiFi.macAddress().c_str());
}

// ===================================================
// LOOP
// ===================================================
void loop() {
  if (!mq5Warmed && millis() - mq5Start >= 60000UL) {
    mq5Warmed = true;
    Serial.println("MQ-5 ready");
  }

  if (remoteConnected && (millis() - lastRemoteMsg > REMOTE_TIMEOUT)) {
    remoteConnected = false;
    stopMotors();
    lastL = 0; lastR = 0;
    Serial.println("Remote LOST - Auto resume when back in range");
  }

  readSensors();
  updateDangerLevel();

  if (!remoteConnected) {
    stopMotors();
    spinActive = false;
  } else if (remoteCmd.stopCommand) {
    stopMotors(); 
    spinActive = false;
  } else if (remoteCmd.spinCommand && !spinActive) {
    // Start Spin macro (one-tap to activate full spin)
    spinActive = true;
    spinStart = millis();
    setLeft(255);
    setRight(-255);
    Serial.println("SPIN started");
  } else if (spinActive) {
    if (millis() - spinStart >= SPIN_MS) {
      spinActive = false;
      stopMotors();
      Serial.println("SPIN done");
    } else {
      setLeft(255);
      setRight(-255);
    }
  } else {
    updateMotors();
  }

  carData.distance         = distance;
  carData.temperature      = temperature;
  carData.gasValue         = gasValue;
  carData.obstacleDetected = obstacleDetected;
  carData.mq5Warmed        = mq5Warmed;
  strcpy(carData.dangerLevel, dangerLevel.c_str());
  esp_now_send(remoteMac, (uint8_t*)&carData, sizeof(carData));

  sendDataToCAM();

  static unsigned long lastDbg = 0;
  if (millis() - lastDbg > 2000) {
    Serial.printf("DIST:%3dcm  TEMP:%.1fC  GAS:%4d  %s  Remote:%s\n",
      distance, temperature, gasValue, dangerLevel.c_str(),
      remoteConnected ? "OK" : "LOST");
    lastDbg = millis();
  }

  delay(30);
}
