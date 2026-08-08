/*
  PHOENIX - ESP32-C3 SuperMini Remote - AXIS CORRECTED

  YOUR JOYSTICK (from your diagram):
    VRX (G1) = FORWARD/BACKWARD axis  → UP=4095, DOWN=0
    VRY (G2) = LEFT/RIGHT turn axis   → RIGHT=4095, LEFT=0
    Center: VRX=2847, VRY=2828

  THE FIX (only change from previous version):
    speed ← VRX  (was VRY — WRONG)
    turn  ← VRY  (was VRX — WRONG)
    No inversion needed for either axis.
*/

#include <esp_now.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>

// ===== PINS =====
#define JOY_VRX    1   // forward/backward axis
#define JOY_VRY    2   // left/right turn axis
#define JOY_SW     3
#define BUZZER_PIN 4
#define BTN_STOP   5
#define BTN_360    6
#define LED_YELLOW 20
#define LED_GREEN  21
#define OLED_SDA   8
#define OLED_SCL   9

#define OLED_W    128
#define OLED_H     64
#define OLED_ADDR 0x3C

Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);

// ===== MAC =====
uint8_t carMac[] = {0x1C, 0xC3, 0xAB, 0xA2, 0x27, 0x40};

// ===== STRUCTS — must match car exactly =====
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

RemoteCommand cmd = {0, 0, false, false};
CarData       car = {};

// ===== JOYSTICK CALIBRATION =====
// Hardcoded from your diagram — auto-calibration overwrites at boot
int centerX = 2847;  // VRX center (forward/back axis)
int centerY = 2828;  // VRY center (turn axis)
const int   DEADZONE  = 200;
const float TURN_PCT  = 0.50f;  // turn speed = 50% of forward speed max

// ===== STATE =====
volatile bool          connected   = false;
volatile unsigned long lastCarTime = 0;
const unsigned long    CAR_TIMEOUT = 2500;

bool lastStopState = HIGH, lastSpinState = HIGH;
bool stopOn = false, spinOn = false;
bool oledOK = false;

unsigned long lastOled  = 0;
unsigned long lastSend  = 0;
unsigned long lastBlink = 0;
bool          blinkSt   = false;

// ===== FORWARD DECLARATIONS =====
void initOLED();
void initESPNow();
void addCarPeer();
void readJoystick();
void readButtons();
void checkConnection();
void updateLEDs();
void updateOLED();
void beep(int freq, int ms);

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n=== PHOENIX Remote - AXIS CORRECTED ===");

  pinMode(JOY_SW,    INPUT_PULLUP);
  pinMode(BTN_STOP,  INPUT_PULLUP);
  pinMode(BTN_360,   INPUT_PULLUP);
  pinMode(LED_YELLOW, OUTPUT); digitalWrite(LED_YELLOW, LOW);
  pinMode(LED_GREEN,  OUTPUT); digitalWrite(LED_GREEN,  LOW);

  ledcAttach(BUZZER_PIN, 2000, 8);
  ledcWriteTone(BUZZER_PIN, 0);

  initOLED();

  if (oledOK) {
    display.clearDisplay();
    display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
    display.setCursor(0,  0); display.println("PHOENIX Remote");
    display.setCursor(0, 16); display.println("Calibrating...");
    display.setCursor(0, 32); display.println("DON'T TOUCH JOYSTICK");
    display.display();
  }

  // Auto-calibration — overwrites hardcoded center values
  Serial.println("Calibrating joystick (keep centered)...");
  long sX = 0, sY = 0;
  for (int i = 0; i < 50; i++) {
    sX += analogRead(JOY_VRX);
    sY += analogRead(JOY_VRY);
    delay(20);
  }
  centerX = sX / 50;
  centerY = sY / 50;
  Serial.printf("Center: VRX=%d VRY=%d\n", centerX, centerY);
  Serial.println("Push UP    → car goes FORWARD");
  Serial.println("Push DOWN  → car goes BACKWARD");
  Serial.println("Push RIGHT → car turns RIGHT");
  Serial.println("Push LEFT  → car turns LEFT");

  beep(800, 100); delay(100); beep(1200, 100); delay(100); beep(1600, 100);

  initESPNow();

  if (oledOK) {
    display.clearDisplay();
    display.setCursor(0,  0); display.println("Calibrated!");
    display.setCursor(0, 16); display.printf("VRX=%d VRY=%d", centerX, centerY);
    display.setCursor(0, 32); display.println("Searching car...");
    display.display();
  }

  Serial.println("Remote ready.");
  Serial.printf("MAC: %s\n", WiFi.macAddress().c_str());
}

// ===== LOOP =====
void loop() {
  readJoystick();
  readButtons();
  checkConnection();
  updateLEDs();

  // Send always — enables auto-reconnect without restart
  if (millis() - lastSend >= 40) {
    cmd.stopCommand = stopOn;
    cmd.spinCommand = spinOn;
    if (!connected) {
      cmd.leftSpeed  = 0;
      cmd.rightSpeed = 0;
    }
    esp_now_send(carMac, (uint8_t*)&cmd, sizeof(cmd));
    lastSend = millis();
  }

  if (millis() - lastOled >= 200) {
    updateOLED();
    lastOled = millis();
  }

  delay(10);
}

// ===== OLED =====
void initOLED() {
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(100000);
  delay(200);

  Serial.print("I2C scan: ");
  int n = 0;
  for (byte a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf("0x%02X ", a); n++; }
  }
  Serial.println();
  if (n == 0) { Serial.println("NO I2C DEVICES"); oledOK = false; return; }

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED FAILED"); oledOK = false; return;
  }
  oledOK = true;
  Serial.println("OLED OK");
  display.clearDisplay(); display.display();
}

// ===== ESP-NOW =====
void initESPNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(); delay(100);
  if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW FAILED"); return; }
  Serial.println("ESP-NOW OK");

  esp_now_register_send_cb([](const wifi_tx_info_t* i, esp_now_send_status_t s) {});

  esp_now_register_recv_cb([](const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len == sizeof(CarData)) {
      memcpy(&car, data, sizeof(car));
      lastCarTime = millis();
      if (!connected) {
        connected = true;
        Serial.println("Car CONNECTED");
      }
    } else {
      Serial.printf("STRUCT SIZE MISMATCH: got=%d expected=%d\n", len, sizeof(CarData));
    }
  });

  addCarPeer();
}

void addCarPeer() {
  esp_now_del_peer(carMac);
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, carMac, 6);
  p.channel = 0; p.encrypt = false;
  Serial.println(esp_now_add_peer(&p) == ESP_OK ? "Car peer OK" : "Car peer FAILED");
}

// ===== JOYSTICK =====
void readJoystick() {
  // Average 5 readings to reduce ADC noise
  long sX = 0, sY = 0;
  for (int i = 0; i < 5; i++) {
    sX += analogRead(JOY_VRX);
    sY += analogRead(JOY_VRY);
  }
  int vrx = sX / 5;  // forward/backward axis
  int vry = sY / 5;  // left/right turn axis

  // Offset from calibrated center
  int rawFB = vrx - centerX;  // FB = Forward/Backward. Positive = forward
  int rawLR = vry - centerY;  // LR = Left/Right.      Positive = right

  // Deadzone
  if (abs(rawFB) < DEADZONE) rawFB = 0;
  if (abs(rawLR) < DEADZONE) rawLR = 0;

  // Map to -255..255
  int halfFB = max(centerX, 4095 - centerX);
  int halfLR = max(centerY, 4095 - centerY);

  // speed: push UP(VRX high, rawFB positive) = positive = FORWARD ✅
  int speed = (rawFB == 0) ? 0 : map(rawFB, -halfFB, halfFB, -255, 255);

  // turn: push RIGHT(VRY high, rawLR positive) = positive = turn RIGHT ✅
  int turn  = (rawLR == 0) ? 0 : map(rawLR, -halfLR, halfLR, -255, 255);

  speed = constrain(speed, -255, 255);
  turn  = constrain(turn,  -255, 255);

  // Differential steering mix (Tank drive pattern)
  int L, R;
  if (speed == 0) {
    // In-place spin (Full Power)
    L = turn;
    R = -turn;
  } else if (speed > 0) {
    // Forward turning
    L = speed + (int)(turn * TURN_PCT);
    R = speed - (int)(turn * TURN_PCT);
  } else {
    // Backward turning
    L = speed - (int)(turn * TURN_PCT);
    R = speed + (int)(turn * TURN_PCT);
  }

  cmd.leftSpeed  = constrain(L, -255, 255);
  cmd.rightSpeed = constrain(R, -255, 255);

  // Joystick button = hold to stop
  if (digitalRead(JOY_SW) == LOW) {
    cmd.leftSpeed = 0; cmd.rightSpeed = 0;
  }

  // Serial debug every 400ms
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 400) {
    String dir = "IDLE";
    if      (cmd.leftSpeed > 30  && cmd.rightSpeed > 30)  dir = "F";
    else if (cmd.leftSpeed < -30 && cmd.rightSpeed < -30) dir = "B";
    else if (cmd.rightSpeed > cmd.leftSpeed  + 30)        dir = "R";
    else if (cmd.leftSpeed  > cmd.rightSpeed + 30)        dir = "L";
    Serial.printf("JOY> %s | spd:%4d turn:%4d | LM:%4d RM:%4d\n",
      dir.c_str(), speed, turn, cmd.leftSpeed, cmd.rightSpeed);
    lastPrint = millis();
  }
}

// ===== BUTTONS =====
void readButtons() {
  bool s = digitalRead(BTN_STOP);
  if (s == LOW && lastStopState == HIGH) {
    stopOn = !stopOn;
    beep(stopOn ? 1500 : 800, 80);
    Serial.printf("STOP: %s\n", stopOn ? "ON" : "OFF");
  }
  lastStopState = s;

  bool sp = digitalRead(BTN_360);
  if (sp == LOW && lastSpinState == HIGH) beep(1800, 60);
  spinOn = (sp == LOW);
  lastSpinState = sp;
}

// ===== CONNECTION =====
void checkConnection() {
  if (connected && millis() - lastCarTime > CAR_TIMEOUT) {
    connected = false;
    Serial.println("Car LOST — will auto-reconnect natively");
  }
}

// ===== LEDs =====
// GREEN on  = connected to car
// YELLOW blink = searching for car
void updateLEDs() {
  if (connected) {
    digitalWrite(LED_GREEN,  HIGH);
    digitalWrite(LED_YELLOW, LOW);
    blinkSt = false;
  } else {
    digitalWrite(LED_GREEN, LOW);
    if (millis() - lastBlink > 400) {
      blinkSt = !blinkSt;
      digitalWrite(LED_YELLOW, blinkSt);
      lastBlink = millis();
    }
  }
}

// ===== OLED =====
void updateOLED() {
  if (!oledOK) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  if (!connected) {
    display.setCursor(0,  0); display.println("-- NO CONNECTION --");
    display.setCursor(0, 16); display.println("Searching for car...");
    display.setCursor(0, 32); display.println("Yellow LED blinking");
    display.setCursor(0, 48); display.println("Will auto-reconnect");
    display.display();
    return;
  }

  // Row 0: Distance + status
  display.setCursor(0, 0);
  if (car.obstacleDetected)
    display.printf("DIST:%3dcm [BLOCKED]", car.distance);
  else
    display.printf("DIST:%3dcm [%s]", car.distance, car.dangerLevel);

  // Row 1: Temp + Gas
  display.setCursor(0, 12);
  if (!car.mq5Warmed)
    display.printf("T:%.1fC  GAS:WARMUP", car.temperature);
  else
    display.printf("T:%.1fC  G:%4d", car.temperature, car.gasValue);

  // Row 2: Motor speeds (LM=Left Motor, RM=Right Motor)
  display.setCursor(0, 24);
  display.printf("LM:%4d  RM:%4d", cmd.leftSpeed, cmd.rightSpeed);

  // Row 3: Direction
  display.setCursor(0, 36);
  if      (stopOn)                                           display.print("[[   STOP   ]]");
  else if (spinOn)                                           display.print("[[   SPIN   ]]");
  else if (cmd.leftSpeed == 0 && cmd.rightSpeed == 0)        display.print("IDLE");
  else if (cmd.leftSpeed > 30  && cmd.rightSpeed > 30)       display.print(">> FORWARD <<");
  else if (cmd.leftSpeed < -30 && cmd.rightSpeed < -30)      display.print("<< BACKWARD >>");
  else if (cmd.rightSpeed > cmd.leftSpeed  + 30)             display.print("TURN RIGHT");
  else if (cmd.leftSpeed  > cmd.rightSpeed + 30)             display.print("TURN LEFT");
  else                                                       display.print("MOVING");

  // Row 4: Warnings
  display.setCursor(0, 50);
  if (car.obstacleDetected) {
    display.print("!! OBSTACLE AHEAD !!");
    static unsigned long lastObsBeep = 0;
    if (millis() - lastObsBeep > 500) { beep(1500, 80); lastObsBeep = millis(); }
  } else if (strcmp(car.dangerLevel, "HIGH") == 0) {
    display.print("!!! HIGH DANGER !!!");
    static unsigned long lastDBeep = 0;
    if (millis() - lastDBeep > 2000) { beep(2800, 120); lastDBeep = millis(); }
  } else {
    display.print("All systems OK");
  }

  display.display();
}

// ===== BUZZER =====
void beep(int freq, int ms) {
  ledcWriteTone(BUZZER_PIN, freq);
  delay(ms);
  ledcWriteTone(BUZZER_PIN, 0);
}
