#include <WiFi.h>
#include <HTTPClient.h>
#include <Ps3Controller.h>
#include <ESP32Servo.h>

// ================= PIN DEFINITIONS =================
#define ENA   4
#define ENB   5
#define IN_1  17
#define IN_2  16
#define IN_3  18
#define IN_4  19
#define LED_TOMBOL 26
#define BTN_TOMBOL 25

// >>> NEW: request log_attack (dikirim via taskNetwork)
volatile bool     reqAttackLog = false;
volatile uint32_t attackTs     = 0;     // millis() saat pukulan dimulai
volatile float    attackG      = 0.0f;  // placeholder; isi dari IMU nanti kalau ada

// ================= WIFI CONFIG =================
const char* ssid     = "bengkel inspirasi";
const char* password = "bengkelinspirasi";

// ================= SERVER CONFIG (robot_b3) =================
const char robot_code[] = "robot_b3";
const char status_host[] = "http://192.168.1.6/tempoer/robot_status.php?robot_code=robot_b3&sig=6c3dfd2b6b64bdfd8717305e78218868b7ddcc5cb3731a118a8ca01071f62d7a";
const char log_host[]    = "http://192.168.1.6/tempoer/log_death.php?robot_code=robot_b3&sig=dea919b3437811c7cca40e361154abffea9051a6664710d667f855cdea7a0b8b";
const char attack_host[] = "http://192.168.1.6/tempoer/log_attack.php";

// ================= RUNTIME TUNING =================
static const uint32_t STATUS_POLL_MS   = 150;  // polling cepat & non-blocking
static const uint32_t HTTP_TIMEOUT_MS  = 200;  // timeout singkat
static const uint8_t  FAIL_MAX         = 3;    // gagal 3x → INACTIVE (fail-safe)
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 10000; // 10s
static const uint32_t WIFI_RETRY_DELAY_MS     = 2000;  // 2s

// ================= VARIABLES =================
int speedCar = 700;            // catatan: analogWrite ESP32 efektif 0–255 (700 ≈ max)
const int speedStep = 100;
const int speedMax  = 1023;    // biarkan sesuai punyamu
const int speedMin  = 400;

Servo myservo, myservo2;
int stick_ly = 0, stick_rx = 0;

// Latch & state pukul (robot_b3)
bool circleLatch = false;
bool isPushing   = false;
unsigned long pushStart = 0;

// Status & tombol fisik
volatile bool robotActive = false; // diupdate di task
volatile bool reqDeathLog = false; // dikirim di task
bool ledState = false;
bool btnWasPressed = false;
unsigned long btnPressStart = 0;
unsigned long lastBlink = 0;

// (opsional) debug mode (skip gating active/inactive)
volatile bool debugMode = false;

// ================= MOTOR CONTROL (analogWrite) =================
void maju()       { digitalWrite(IN_1,HIGH); digitalWrite(IN_2,LOW );  analogWrite(ENA, speedCar);   digitalWrite(IN_3,HIGH); digitalWrite(IN_4,LOW );  analogWrite(ENB, speedCar); }
void mundur()     { digitalWrite(IN_1,LOW ); digitalWrite(IN_2,HIGH);  analogWrite(ENA, speedCar);   digitalWrite(IN_3,LOW );  digitalWrite(IN_4,HIGH); analogWrite(ENB, speedCar); }
void putarKiri()  { digitalWrite(IN_1,LOW ); digitalWrite(IN_2,HIGH);  analogWrite(ENA, speedCar);   digitalWrite(IN_3,HIGH);  digitalWrite(IN_4,LOW );  analogWrite(ENB, speedCar); }
void putarKanan() { digitalWrite(IN_1,HIGH); digitalWrite(IN_2,LOW );  analogWrite(ENA, speedCar);   digitalWrite(IN_3,LOW );  digitalWrite(IN_4,HIGH);  analogWrite(ENB, speedCar); }
void belokKiri()  { digitalWrite(IN_1,HIGH); digitalWrite(IN_2,LOW );  analogWrite(ENA, speedCar/2); digitalWrite(IN_3,HIGH);  digitalWrite(IN_4,LOW );  analogWrite(ENB, speedCar); }
void belokKanan() { digitalWrite(IN_1,HIGH); digitalWrite(IN_2,LOW );  analogWrite(ENA, speedCar);   digitalWrite(IN_3,HIGH);  digitalWrite(IN_4,LOW );  analogWrite(ENB, speedCar/2); }
void mundurKiri() { digitalWrite(IN_1,LOW ); digitalWrite(IN_2,HIGH);  analogWrite(ENA, speedCar/2); digitalWrite(IN_3,LOW );  digitalWrite(IN_4,HIGH);  analogWrite(ENB, speedCar); }
void mundurKanan(){ digitalWrite(IN_1,LOW ); digitalWrite(IN_2,HIGH);  analogWrite(ENA, speedCar);   digitalWrite(IN_3,LOW );  digitalWrite(IN_4,HIGH);  analogWrite(ENB, speedCar/2); }
void stopRobot()  { digitalWrite(IN_1,LOW ); digitalWrite(IN_2,LOW );  analogWrite(ENA, 0);          digitalWrite(IN_3,LOW );  digitalWrite(IN_4,LOW );  analogWrite(ENB, 0); }

// ================= PUSH SERVO (robot_b3) =================
void startPush() {
  // >>> NEW: catat & minta kirim log_attack (tanpa HMAC)
  attackTs = millis();
  attackG  = 0.0f;            // kalau nanti ada IMU, isi peak G di sini
  reqAttackLog = true;        // biar taskNetwork yang kirim (non-blocking)

  // lanjut proses pukul seperti biasa
  isPushing = true;
  pushStart = millis();
  myservo2.write(160);  // siapkan
  myservo.write(180);   // pukul
  delay(100);
}

void handlePush() {
  if (!isPushing) return;
  unsigned long now = millis();
  if (now - pushStart > 500 && now - pushStart <= 600) {
    myservo.write(30);    // langkah balik
  }
  if (now - pushStart > 700) {
    myservo2.write(75);   // netral
    isPushing = false;
  }
}

// ================= WIFI/HTTP HELPERS =================
static bool fastStatusGet(const char* url, bool &active_out) {
  HTTPClient http;
  http.setReuse(true);                     // keep-alive untuk latensi rendah
  if (!http.begin(url)) return false;
  http.setTimeout(HTTP_TIMEOUT_MS);       // 150–250 ms cukup

  int code = http.GET();
  if (code == 200) {
    String p = http.getString(); p.trim();
    if (p.length()==1 && (p[0]=='1'||p[0]=='0')) active_out=(p[0]=='1');
    else active_out = p.equalsIgnoreCase("ACTIVE");
  }
  http.end();
  return (code == 200);
}

static void quickGet(const char* url, uint16_t timeout_ms=500) {
  HTTPClient http; http.setReuse(true);
  if (http.begin(url)) { http.setTimeout(timeout_ms); http.GET(); http.end(); }
}

// ================= TASK JARINGAN =================
// State-machine WiFi agar tidak spam WiFi.begin()
void taskNetwork(void*) {
  enum WifiSm { IDLE, CONNECTING, CONNECTED };
  WifiSm state = IDLE;
  unsigned long stateSince = 0;
  unsigned long lastPoll   = 0;
  uint8_t failCount = 0;

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  for (;;) {
    wl_status_t st = WiFi.status();

    switch (state) {
      case IDLE:
        if (st != WL_CONNECTED) {
          WiFi.begin(ssid, password);
          state = CONNECTING;
          stateSince = millis();
          // Serial.println("[WiFi] begin()");
        } else {
          state = CONNECTED;
          stateSince = millis();
        }
        break;

      case CONNECTING:
        if (st == WL_CONNECTED) {
          state = CONNECTED;
          stateSince = millis();
          // Serial.print("[WiFi] Connected. IP="); Serial.println(WiFi.localIP());
        } else if (millis() - stateSince > WIFI_CONNECT_TIMEOUT_MS) {
          // timeout → reset dan coba lagi setelah jeda
          WiFi.disconnect(true, true);
          vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
          state = IDLE;
          stateSince = millis();
          // Serial.println("[WiFi] Retry connect");
        }
        break;

      case CONNECTED:
        if (st != WL_CONNECTED) {
          state = IDLE;
          stateSince = millis();
          break;
        }

        // Poll status cepat (skip kalau debugMode)
        if (!debugMode && millis() - lastPoll >= STATUS_POLL_MS) {
          lastPoll = millis();
          bool act=false;
          bool ok = fastStatusGet(status_host, act);
          if (ok) { robotActive = act; failCount = 0; }
          else {
            if (++failCount >= FAIL_MAX) { robotActive = false; failCount = 0; } // fail-safe
          }
        }

        // Kirim death log jika diminta
        if (reqDeathLog) { reqDeathLog = false; quickGet(log_host, 500); }

        // >>> NEW: kirim log_attack (tanpa HMAC)
        if (reqAttackLog) {
          reqAttackLog = false;
          String url = String(attack_host)
                      + "?robot_code=" + robot_code
                      + "&ts=" + String(attackTs)
                      + "&g="  + String(attackG, 2);
          quickGet(url.c_str(), 300);   // fire-and-forget, timeout singkat
        }
        break;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ================= PS3 HANDLERS =================
void notify() {
  stick_ly = Ps3.data.analog.stick.ly;
  stick_rx = Ps3.data.analog.stick.rx;

  // Circle → pukul
  if (Ps3.data.button.circle && !circleLatch) {
    circleLatch = true;
    if (!isPushing) startPush();
  } else if (!Ps3.data.button.circle) {
    circleLatch = false;
  }

  // (opsional) Select → toggle debug
  static bool selectLatch=false;
  if (Ps3.data.button.select && !selectLatch) { debugMode = !debugMode; selectLatch = true; }
  if (!Ps3.data.button.select) selectLatch=false;

  // Speed adjust
  if (Ps3.data.button.l1) speedCar = max(speedCar - speedStep, speedMin);
  if (Ps3.data.button.r1) speedCar = min(speedCar + speedStep, speedMax);
}
void onConnect() { Serial.println(F("PS3 Controller Connected.")); }

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  pinMode(ENA, OUTPUT);
  pinMode(ENB, OUTPUT);
  pinMode(IN_1, OUTPUT);
  pinMode(IN_2, OUTPUT);
  pinMode(IN_3, OUTPUT);
  pinMode(IN_4, OUTPUT);

  pinMode(LED_TOMBOL, OUTPUT);
  pinMode(BTN_TOMBOL, INPUT_PULLUP);

  myservo.attach(32, 500, 2400);
  myservo2.attach(33, 500, 2400);

  // Mulai task jaringan (WiFi + HTTP)
  xTaskCreatePinnedToCore(taskNetwork, "taskNetwork", 6144, NULL, 1, NULL, 0);

  Ps3.attach(notify);
  Ps3.attachOnConnect(onConnect);
  Ps3.begin("01:02:03:04:05:04"); // stik b2 dipakai untuk robot_b3

  Serial.println(F("System Ready (robot_b3, non-blocking WiFi)."));
}

// ================= LOOP =================
void loop() {
  if (!Ps3.isConnected()) return;

  // Tombol fisik: long-press 1s → request death log (dikirim task)
  bool btnPressed = (digitalRead(BTN_TOMBOL) == LOW);
  if (btnPressed && !btnWasPressed) { btnPressStart = millis(); btnWasPressed = true; }
  if (!btnPressed && btnWasPressed) btnWasPressed = false;
  if (btnPressed && btnWasPressed && (millis() - btnPressStart > 50)) {
    reqDeathLog = true; btnWasPressed = false;
  }

  // Jika INACTIVE & bukan debug → blink & stop
  if (!robotActive && !debugMode) {
    if (millis() - lastBlink > 500) {
      lastBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_TOMBOL, ledState);
    }
    stopRobot();
    handlePush(); // selesaikan siklus servo kalau sedang jalan
    return;
  } else {
    digitalWrite(LED_TOMBOL, LOW);
  }

  // Kontrol gerak (deadzone ±25)
  if (stick_ly > 25) {
    if      (stick_rx > 25) belokKanan();
    else if (stick_rx < -25) belokKiri();
    else maju();
  } else if (stick_ly < -25) {
    if      (stick_rx > 25) mundurKanan();
    else if (stick_rx < -25) mundurKiri();
    else mundur();
  } else if (stick_rx > 25) putarKanan();
  else if (stick_rx < -25) putarKiri();
  else stopRobot();

  handlePush();
  delay(1);
}
