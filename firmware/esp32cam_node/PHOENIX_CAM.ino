/*
  PHOENIX - ESP32-CAM OPTIMIZED
  - Low latency video streaming
  - Full servo controls in dashboard
  - Access Point mode
*/

#include "esp_camera.h"
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>

// ===== WiFi ACCESS POINT Settings =====
const char* ap_ssid     = "PHOENIX_CAM";
const char* ap_password = "12345678";
const char* ap_hostname = "robotcam";

// ===== CAMERA PINS =====
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

#define LED_FLASH   4
#define PAN_SERVO   2   // Pan servo
#define TILT_SERVO 14   // Tilt servo

// UART pins for communication
#define CAM_UART_RX 13
#define CAM_UART_TX 12

// ===== OBJECTS =====
WebServer        server(80);
WebSocketsServer webSocket = WebSocketsServer(81);
Servo            panServo;
Servo            tiltServo;

// Sensor data removed

int  panAngle     = 90;
int  tiltAngle_v  = 90;
bool flashOn      = false;

TaskHandle_t streamTaskHandle = NULL;
WiFiClient   streamClient;

// ===== OPTIMIZED HTML with Servo Controls =====
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>PHOENIX HUD</title>
  <style>
    :root {
      --bg: #0f172a;
      --glass-bg: rgba(30, 41, 59, 0.7);
      --glass-border: rgba(255, 255, 255, 0.1);
      --primary: #3b82f6;
      --primary-hover: #2563eb;
      --accent: #8b5cf6;
      --danger: #ef4444;
      --text: #f8fafc;
      --text-muted: #94a3b8;
    }
    
    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
      user-select: none;
      -webkit-tap-highlight-color: transparent;
    }

    body {
      font-family: 'Inter', -apple-system, sans-serif;
      background: var(--bg);
      background-image: 
        radial-gradient(at 0% 0%, rgba(59, 130, 246, 0.15) 0px, transparent 50%),
        radial-gradient(at 100% 100%, rgba(139, 92, 246, 0.15) 0px, transparent 50%);
      color: var(--text);
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      align-items: center;
      padding: 20px 15px;
    }

    .container {
      width: 100%;
      max-width: 800px;
      display: flex;
      flex-direction: column;
      gap: 20px;
    }

    .header {
      text-align: center;
      margin-bottom: 5px;
    }

    .header h1 {
      font-size: 1.8rem;
      font-weight: 800;
      background: linear-gradient(to right, #60a5fa, #c084fc);
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
      letter-spacing: 1px;
      text-transform: uppercase;
    }

    .badge {
      display: inline-flex;
      align-items: center;
      padding: 6px 14px;
      border-radius: 999px;
      background: rgba(16, 185, 129, 0.2);
      color: #34d399;
      font-size: 0.85rem;
      font-weight: 600;
      margin-top: 8px;
      border: 1px solid rgba(16, 185, 129, 0.3);
      box-shadow: 0 0 10px rgba(16, 185, 129, 0.1);
    }

    .badge::before {
      content: '';
      width: 8px;
      height: 8px;
      background: #34d399;
      border-radius: 50%;
      margin-right: 8px;
      box-shadow: 0 0 8px #34d399;
      animation: pulse 2s infinite;
    }

    @keyframes pulse {
      0% { opacity: 1; transform: scale(1); }
      50% { opacity: 0.5; transform: scale(1.2); }
      100% { opacity: 1; transform: scale(1); }
    }

    .glass-panel {
      background: var(--glass-bg);
      backdrop-filter: blur(12px);
      -webkit-backdrop-filter: blur(12px);
      border: 1px solid var(--glass-border);
      border-radius: 20px;
      padding: 20px;
      box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
      transition: transform 0.3s ease, box-shadow 0.3s ease;
    }

    .glass-panel:hover {
      box-shadow: 0 8px 32px rgba(0, 0, 0, 0.5);
    }

    .video-container {
      padding: 10px;
      background: linear-gradient(145deg, rgba(30,41,59,0.8), rgba(15,23,42,0.9));
      position: relative;
    }

    #stream {
      width: 100%;
      border-radius: 12px;
      display: block;
      background: #000;
      aspect-ratio: 4/3;
      object-fit: cover;
      box-shadow: inset 0 0 20px rgba(0,0,0,1);
    }

    .section-title {
      font-size: 1.1rem;
      font-weight: 600;
      margin-bottom: 15px;
      color: var(--text-muted);
      display: flex;
      align-items: center;
      gap: 8px;
    }

    .grid-2 {
      display: grid;
      grid-template-columns: repeat(2, 1fr);
      gap: 12px;
    }



    .slider-group {
      margin-bottom: 20px;
    }

    .slider-header {
      display: flex;
      justify-content: space-between;
      margin-bottom: 10px;
      font-size: 0.95rem;
      font-weight: 500;
    }

    .slider-val {
      color: var(--primary);
      font-weight: 700;
    }

    input[type=range] {
      -webkit-appearance: none;
      width: 100%;
      height: 8px;
      background: rgba(0,0,0,0.3);
      border-radius: 4px;
      outline: none;
      box-shadow: inset 0 1px 3px rgba(0,0,0,0.5);
      touch-action: none;
    }

    input[type=range]::-webkit-slider-thumb {
      -webkit-appearance: none;
      width: 24px;
      height: 24px;
      border-radius: 50%;
      background: var(--primary);
      cursor: pointer;
      box-shadow: 0 0 15px rgba(59, 130, 246, 0.6);
      transition: transform 0.1s, background 0.2s;
    }

    input[type=range]::-webkit-slider-thumb:active {
      transform: scale(1.2);
      background: var(--primary-hover);
    }

    .btn {
      width: 100%;
      padding: 16px;
      border: none;
      border-radius: 14px;
      font-size: 1rem;
      font-weight: 600;
      color: white;
      cursor: pointer;
      transition: all 0.2s;
      background: linear-gradient(135deg, var(--primary), var(--accent));
      box-shadow: 0 4px 15px rgba(59, 130, 246, 0.3);
      text-transform: uppercase;
      letter-spacing: 1px;
    }

    .btn:active {
      transform: translateY(2px);
      box-shadow: 0 2px 8px rgba(59, 130, 246, 0.2);
    }

    .btn-secondary {
      background: rgba(255, 255, 255, 0.1);
      border: 1px solid rgba(255, 255, 255, 0.1);
      box-shadow: none;
    }

    .btn-secondary:active {
      background: rgba(255, 255, 255, 0.05);
    }

    .btn.active-flash {
      background: linear-gradient(135deg, #f59e0b, #ea580c);
      box-shadow: 0 4px 20px rgba(245, 158, 11, 0.4);
    }
  </style>
</head>
<body>

<div class="container">
  <div class="header">
    <h1>PHOENIX HUD</h1>
    <div class="badge">Live &bull; HTTP://<span id="ipAddr">...</span></div>
  </div>

  <div class="glass-panel video-container">
    <img id="stream" alt="Camera Feed Offline">
  </div>

  <div class="glass-panel">
    <div class="section-title">
      <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2a10 10 0 1 0 10 10 4 4 0 0 1-5-5 4 4 0 0 1-5-5"/></svg>
      Turret Control
    </div>
    
    <div class="slider-group">
      <div class="slider-header">
        <span>Azimuth (Pan)</span>
        <span class="slider-val"><span id="panValue">90</span>°</span>
      </div>
      <input type="range" id="pan" min="0" max="180" value="90">
    </div>

    <div class="slider-group">
      <div class="slider-header">
        <span>Elevation (Tilt)</span>
        <span class="slider-val" style="color:var(--accent)"><span id="tiltValue">90</span>°</span>
      </div>
      <input type="range" id="tilt" min="0" max="180" value="90">
    </div>

    <div class="grid-2" style="margin-top: 25px;">
      <button class="btn btn-secondary" id="centerBtn">Reset Turret</button>
      <button class="btn" id="flashBtn">Toggle Flash</button>
    </div>
  </div>
</div>

<script>
  // Setup IP and Stream
  const ipAddr = window.location.hostname || "192.168.4.1";
  document.getElementById('ipAddr').innerText = ipAddr;
  
  // MJPEG Stream - NO JAVASCRIPT RELOADING REQUIRED, BROWSER HANDLES IT NATIVELY
  document.getElementById('stream').src = `http://${ipAddr}/stream`;
  
  // WebSockets
  let ws;
  let reconnectAttempts = 0;
  
  function connect() {
    ws = new WebSocket(`ws://${ipAddr}:81`);
    
    ws.onclose = () => setTimeout(connect, 2000);
  }
  
  function sendWS(obj) {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(obj));
    }
  }

  // UI Event Listeners
  const panSlider = document.getElementById('pan');
  const tiltSlider = document.getElementById('tilt');
  const panVal = document.getElementById('panValue');
  const tiltVal = document.getElementById('tiltValue');
  const flashBtn = document.getElementById('flashBtn');
  
  let lastPan = 0;
  panSlider.oninput = function() {
    panVal.innerText = this.value;
    const now = Date.now();
    if (now - lastPan > 40) { // Reduced debounce to 40ms for accurate seamless interaction
      sendWS({type: 'pan', value: parseInt(this.value)});
      lastPan = now;
    }
  };
  panSlider.onchange = function() { // Send exact final position immediately when finger is released
    sendWS({type: 'pan', value: parseInt(this.value)});
  };

  let lastTilt = 0;
  tiltSlider.oninput = function() {
    tiltVal.innerText = this.value;
    const now = Date.now();
    if (now - lastTilt > 40) { // Reduced debounce to 40ms for accurate seamless interaction
      sendWS({type: 'tilt', value: parseInt(this.value)});
      lastTilt = now;
    }
  };
  tiltSlider.onchange = function() { // Send final position on release
    sendWS({type: 'tilt', value: parseInt(this.value)});
  };

  document.getElementById('centerBtn').onclick = () => {
    panSlider.value = 90; tiltSlider.value = 90;
    panVal.innerText = 90; tiltVal.innerText = 90;
    sendWS({type: 'pan', value: 90});
    sendWS({type: 'tilt', value: 90});
  };

  let flashState = false;
  flashBtn.onclick = () => {
    flashState = !flashState;
    flashBtn.classList.toggle('active-flash', flashState);
    flashBtn.innerText = flashState ? 'Flash Enabled' : 'Toggle Flash';
    sendWS({type: 'flash', state: flashState});
  };

  // Init
  connect();
</script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== PHOENIX ESP32-CAM OPTIMIZED ===");
  
  // Initialize UART
  Serial2.begin(115200, SERIAL_8N1, CAM_UART_RX, CAM_UART_TX);
  Serial.println("UART initialized");
  
  // Initialize pins
  pinMode(LED_FLASH, OUTPUT);
  digitalWrite(LED_FLASH, LOW);
  
  // Initialize components
  initCamera();
  initServos();
  initWiFiAP();
  
  // Setup web server routes
  server.on("/", handleRoot);
  server.on("/stream", handleStream);
  server.on("/flash", handleFlash);
  server.begin();
  
  // Setup WebSocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  
  // Setup mDNS
  MDNS.begin(ap_hostname);
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("ws", "tcp", 81);
  
  Serial.println("\n=== SYSTEM READY ===");
  Serial.print("AP: "); Serial.println(ap_ssid);
  Serial.print("IP: "); Serial.println(WiFi.softAPIP());
  Serial.print("Web: http://"); Serial.println(WiFi.softAPIP());
  Serial.println("===================\n");
}

void loop() {
  server.handleClient();
  webSocket.loop();
  
  delay(5); // Small delay for stability
}

void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_HQVGA;  // 240x176: Ultra low resolution for absolute zero latency
  config.jpeg_quality = 12;              // Restored high JPEG quality since resolution is much smaller
  config.fb_count = 1;                  // Single buffer for lower latency
  
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
  } else {
    Serial.println("Camera OK (low latency mode)");
  }
}

void initServos() {
  // Allocate specific timers to PREVENT CONFLICT with camera (XCLK uses LEDC_TIMER_0)
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  
  panServo.setPeriodHertz(50);
  panServo.attach(PAN_SERVO); // Removed custom 500-2400 boundaries which usually stalls specific brackets!
  tiltServo.setPeriodHertz(50);
  tiltServo.attach(TILT_SERVO); // Depend on library safe defaults to fix tilt locking
  
  panServo.write(90);
  tiltServo.write(90);
  Serial.println("Servos initialized without timer conflicts");
}

void initWiFiAP() {
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false); // Crucial! Disable WiFi power saving for max AP speed
  WiFi.softAP(ap_ssid, ap_password);
  WiFi.softAPsetHostname(ap_hostname);
  Serial.println("WiFi AP started");
}

void handleRoot() {
  server.send(200, "text/html", index_html);
}

void handleFlash() {
  if (server.hasArg("state")) {
    flashOn = server.arg("state").toInt();
    digitalWrite(LED_FLASH, flashOn ? HIGH : LOW);
    server.send(200, "text/plain", flashOn ? "ON" : "OFF");
  } else {
    server.send(400, "text/plain", "Missing state");
  }
}

void handleStream() {
  streamClient = server.client();
  streamClient.setNoDelay(true); // CRITICAL: Disable Nagle's algorithm on the TCP socket to instantly send packets without the standard 200ms network delay!
  
  if (streamTaskHandle != NULL) {
    vTaskDelete(streamTaskHandle);
    streamTaskHandle = NULL;
  }
  // Pin to Core 1! Core 0 handles WiFi PHY/MAC. Running this heavy loop on Core 0 starves the WiFi stack!
  xTaskCreatePinnedToCore(streamTask, "stream", 4096, NULL, 2, &streamTaskHandle, 1);
}

void streamTask(void* param) {
  streamClient.print(
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
    "Cache-Control: no-cache, no-store, must-revalidate\r\n"
    "Pragma: no-cache\r\n"
    "Expires: 0\r\n\r\n"
  );
  
  while (streamClient.connected()) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      vTaskDelay(5 / portTICK_PERIOD_MS);
      continue;
    }
    
    // Bundle headers into a single packet to prevent TCP/IP fragmentation and extreme overhead
    char headerMsg[128];
    snprintf(headerMsg, sizeof(headerMsg), 
      "--frame\r\n"
      "Content-Type: image/jpeg\r\n"
      "Content-Length: %u\r\n\r\n", fb->len);
      
    streamClient.write((uint8_t*)headerMsg, strlen(headerMsg));
    streamClient.write(fb->buf, fb->len);
    streamClient.write((uint8_t*)"\r\n", 2);
    
    esp_camera_fb_return(fb);
    
    vTaskDelay(5 / portTICK_PERIOD_MS); // Lowered minimum loop delay completely to 5ms for max framerate
  }
  
  streamClient.stop(); // Release the socket correctly
  streamTaskHandle = NULL;
  vTaskDelete(NULL);
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type != WStype_TEXT) return;
  
  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) return;
  
  String t = doc["type"].as<String>();
  
  if (t == "pan") {
    panAngle = constrain((int)doc["value"], 0, 180);
    panServo.write(panAngle);
  } 
  else if (t == "tilt") {
    tiltAngle_v = constrain((int)doc["value"], 0, 180);
    tiltServo.write(tiltAngle_v);
  } 
  else if (t == "flash") {
    flashOn = (bool)doc["state"];
    digitalWrite(LED_FLASH, flashOn ? HIGH : LOW);
  }
}
