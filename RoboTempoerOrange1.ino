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

// ================= WIFI CONFIG =================
const char* ssid     = "bengkel inspirasi";
const char* password = "bengkelinspirasi";

// ================= SERVER CONFIG =================
const char robot_code[] = "robot_o1";
const char status_host[] = "http://192.168.1.6/tempoer/robot_status.php?robot_code=robot_o1";
const char log_host[]    = "http://192.168.1.6/tempoer/log_death.php?robot_code=robot_o1";

// ================= VARIABLES =================
int speedCar = 700;
const int speedStep = 100;
const int speedMax = 1023;
const int speedMin = 400;

Servo myservo, myservo2;
int stick_ly = 0, stick_rx = 0;
bool circlePressed = false, circleLatch = false;
bool isPushing = false;
unsigned long pushStart = 0;
unsigned long lastStatusCheck = 0;
bool robotActive = false;
unsigned long lastBlink = 0;
bool ledState = false;

// BTN_TOMBOL long-press
unsigned long btnPressStart = 0;
bool btnWasPressed = false;

// ================= WIFI FUNCTIONS =================
void connectWiFi() {
    Serial.printf("Connecting to WiFi %s", ssid);
    WiFi.begin(ssid, password);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
        delay(500);
        Serial.print(F("."));
        retry++;
    }
    Serial.println(WiFi.status() == WL_CONNECTED ? F(" Connected!") : F(" Failed to connect!"));
}

bool cekStatus() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[cekStatus] WiFi disconnected!");
        return false;
    }

    HTTPClient http;
    http.begin(status_host);
    int httpCode = http.GET();

    if (httpCode == 200) {
        String payload = http.getString();
        payload.trim();
        Serial.println("[cekStatus] Response: " + payload);

        // Hitung panjang karakter
        int len = payload.length();
        if (len == 6 && payload.equalsIgnoreCase("ACTIVE")) {
            Serial.println("[cekStatus] Status Detected: ACTIVE");
            http.end();
            return true;
        } else if (len == 8 && payload.equalsIgnoreCase("INACTIVE")) {
            Serial.println("[cekStatus] Status Detected: INACTIVE");
            http.end();
            return false;
        } else {
            Serial.println("[cekStatus] Status Unknown: " + payload);
            http.end();
            return false;
        }
    }

    Serial.println("[cekStatus] HTTP Request failed! Code: " + String(httpCode));
    http.end();
    return false;
}



void sendDeathLog() {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    http.begin(log_host);
    int httpCode = http.GET();
    Serial.println(httpCode > 0 ? F("Death log sent") : F("Death log failed"));
    http.end();
}

// ================= MOTOR CONTROL =================
void maju()          { digitalWrite(IN_1, HIGH); digitalWrite(IN_2, LOW);  analogWrite(ENA, speedCar); digitalWrite(IN_3, HIGH); digitalWrite(IN_4, LOW);  analogWrite(ENB, speedCar); }
void mundur()        { digitalWrite(IN_1, LOW);  digitalWrite(IN_2, HIGH); analogWrite(ENA, speedCar); digitalWrite(IN_3, LOW);  digitalWrite(IN_4, HIGH); analogWrite(ENB, speedCar); }
void putarKiri()     { digitalWrite(IN_1, LOW);  digitalWrite(IN_2, HIGH); analogWrite(ENA, speedCar); digitalWrite(IN_3, HIGH); digitalWrite(IN_4, LOW);  analogWrite(ENB, speedCar); }
void putarKanan()    { digitalWrite(IN_1, HIGH); digitalWrite(IN_2, LOW);  analogWrite(ENA, speedCar); digitalWrite(IN_3, LOW);  digitalWrite(IN_4, HIGH); analogWrite(ENB, speedCar); }
void belokKiri()     { digitalWrite(IN_1, HIGH); digitalWrite(IN_2, LOW);  analogWrite(ENA, speedCar / 2); digitalWrite(IN_3, HIGH); digitalWrite(IN_4, LOW);  analogWrite(ENB, speedCar); }
void belokKanan()    { digitalWrite(IN_1, HIGH); digitalWrite(IN_2, LOW);  analogWrite(ENA, speedCar);    digitalWrite(IN_3, HIGH); digitalWrite(IN_4, LOW);  analogWrite(ENB, speedCar / 2); }
void mundurKiri()    { digitalWrite(IN_1, LOW);  digitalWrite(IN_2, HIGH); analogWrite(ENA, speedCar / 2); digitalWrite(IN_3, LOW);  digitalWrite(IN_4, HIGH); analogWrite(ENB, speedCar); }
void mundurKanan()   { digitalWrite(IN_1, LOW);  digitalWrite(IN_2, HIGH); analogWrite(ENA, speedCar);    digitalWrite(IN_3, LOW);  digitalWrite(IN_4, HIGH); analogWrite(ENB, speedCar / 2); }
void stopRobot()     { digitalWrite(IN_1, LOW); digitalWrite(IN_2, LOW); analogWrite(ENA, 0); digitalWrite(IN_3, LOW); digitalWrite(IN_4, LOW); analogWrite(ENB, 0); }

// ================= PUSH SERVO =================
void startPush() {
    isPushing = true;
    pushStart = millis();
    myservo2.write(160);
    myservo.write(180);
    Serial.println(F("Push Start"));
}

void handlePush() {
    if (!isPushing) return;
    unsigned long now = millis();
    if (now - pushStart > 500 && now - pushStart <= 600) {
        myservo.write(30);
        Serial.println(F("Push Step 2"));
    }
    if (now - pushStart > 700) {
        myservo2.write(75);
        isPushing = false;
        Serial.println(F("Push Done"));
    }
}

// ================= PS3 HANDLERS =================
void notify() {
    stick_ly = Ps3.data.analog.stick.ly;
    stick_rx = Ps3.data.analog.stick.rx;

    if (Ps3.data.button.circle && !circleLatch) {
        circlePressed = true;
        circleLatch = true;
        Serial.println(F("Circle pressed"));
    } else if (!Ps3.data.button.circle) {
        circleLatch = false;
    }

    if (Ps3.data.button.l1) {
        speedCar = max(speedCar - speedStep, speedMin);
        Serial.printf("Speed Down: %d\n", speedCar);
    }
    if (Ps3.data.button.r1) {
        speedCar = min(speedCar + speedStep, speedMax);
        Serial.printf("Speed Up: %d\n", speedCar);
    }
}

void onConnect() {
    Serial.println(F("PS3 Controller Connected."));
}

// ================= SETUP =================
void setup() {
    Serial.begin(115200);
    connectWiFi();

    Ps3.attach(notify);
    Ps3.attachOnConnect(onConnect);
    //Ps3.begin("c0:14:3d:85:d6:6f"); //o1
    Ps3.begin("90:34:fc:7c:a9:03"); //o2
    //Ps3.begin("01:02:03:04:05:04"); // b1 
    //Ps3.begin("01:02:03:04:05:06"); // b2 
    //Ps3.begin("01:02:03:04:05:03"); // o2

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

    Serial.println(F("System Ready."));
}

// ================= LOOP =================
void loop() {
    if (!Ps3.isConnected()) return;

    // Tombol fisik death log
    bool btnPressed = (digitalRead(BTN_TOMBOL) == LOW);
    if (btnPressed && !btnWasPressed) {
        btnPressStart = millis();
        btnWasPressed = true;
    }
    if (!btnPressed && btnWasPressed) btnWasPressed = false;
    if (btnPressed && btnWasPressed && (millis() - btnPressStart > 50)) {
        Serial.println(F("BTN_TOMBOL ditekan 1 detik -> Kirim Death Log"));
        sendDeathLog();
        btnWasPressed = false;
    }

    // WiFi reconnect
    if (WiFi.status() != WL_CONNECTED) connectWiFi();

    // Cek status robot tiap 5 detik
    if (millis() - lastStatusCheck > 5000) {
        lastStatusCheck = millis();
        robotActive = cekStatus();
        Serial.println(robotActive ? F("Robot ACTIVE") : F("Robot INACTIVE"));
        if (!robotActive) stopRobot();
    }

    // Jika INACTIVE -> LED Blink
    if (!robotActive) {
        if (millis() - lastBlink > 500) {
            lastBlink = millis();
            ledState = !ledState;
            digitalWrite(LED_TOMBOL, ledState);
        }
        stopRobot();
        return;  // skip kontrol
    } else {
        digitalWrite(LED_TOMBOL, LOW);  // LED mati jika ACTIVE
    }

    // Kontrol motor saat ACTIVE
    if (stick_ly > 25) {
        if (stick_rx > 25) belokKanan();
        else if (stick_rx < -25) belokKiri();
        else maju();
    } else if (stick_ly < -25) {
        if (stick_rx > 25) mundurKanan();
        else if (stick_rx < -25) mundurKiri();
        else mundur();
    } else if (stick_rx > 25) putarKanan();
    else if (stick_rx < -25) putarKiri();
    else stopRobot();

    // Push (Circle)
    if (circlePressed && !isPushing) {
        startPush();
        circlePressed = false;
    }
    handlePush();

    delay(1);
}
