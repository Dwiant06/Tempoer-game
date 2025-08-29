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
const char status_host[] = "http://192.168.1.6/tempoer/robot_status.php?robot_code=robot_o1
                            &sig=70a4c32f6787ba0cee04beb297dee11f0120e31ce8a2e416939c40be9bac7bae";
const char log_host[]    = "http://192.168.1.6/tempoer/log_death.php?robot_code=robot_o1
                            &sig=7f744b2e9f47b1d7a691854ecea0f789fd5e10d15fef88e58873439f34b9139a";

// ================= VARIABLES =================
int speedCar = 700;
const int speedStep = 100;
const int speedMax = 1023;
const int speedMin = 400;

Servo myservo, myservo2;
int stick_ly = 0, stick_rx = 0;
bool circleLatch = false;
bool isPlanting = false;
bool isPicking = false;
bool isAttacking = false;
bool isGripping = false;
bool xLatch = false;
bool debugMode = false;
bool selectLatch = false;
bool armUp = true;
bool flagAttached = false;
bool robotActive = false;
bool ledState = false;
bool btnWasPressed = false;

unsigned long plantingStart = 0;
unsigned long pickingStart = 0;
unsigned long attackStart = 0;
unsigned long lastXPress = 0;
unsigned long btnPressStart = 0;
unsigned long lastStatusCheck = 0;
unsigned long lastBlink = 0;

// ================= WIFI FUNCTIONS =================
void connectWiFi() {
    WiFi.begin(ssid, password);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
        delay(500);
        retry++;
    }
}

bool cekStatus() {
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    http.begin(status_host);
    int httpCode = http.GET();
    if (httpCode == 200) {
        String payload = http.getString();
        payload.trim();
        return payload.equalsIgnoreCase("ACTIVE");
    }
    http.end();
    return false;
}

void sendDeathLog() {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    http.begin(log_host);
    http.GET();
    http.end();
}

// ================= MOTOR CONTROL =================
void maju()       { digitalWrite(IN_1, HIGH); digitalWrite(IN_2, LOW);  analogWrite(ENA, speedCar); digitalWrite(IN_3, HIGH); digitalWrite(IN_4, LOW);  analogWrite(ENB, speedCar); }
void mundur()     { digitalWrite(IN_1, LOW);  digitalWrite(IN_2, HIGH); analogWrite(ENA, speedCar); digitalWrite(IN_3, LOW);  digitalWrite(IN_4, HIGH); analogWrite(ENB, speedCar); }
void putarKiri()  { digitalWrite(IN_1, LOW);  digitalWrite(IN_2, HIGH); analogWrite(ENA, speedCar); digitalWrite(IN_3, HIGH); digitalWrite(IN_4, LOW);  analogWrite(ENB, speedCar); }
void putarKanan() { digitalWrite(IN_1, HIGH); digitalWrite(IN_2, LOW);  analogWrite(ENA, speedCar); digitalWrite(IN_3, LOW);  digitalWrite(IN_4, HIGH); analogWrite(ENB, speedCar); }
void belokKiri()  { digitalWrite(IN_1, HIGH); digitalWrite(IN_2, LOW);  analogWrite(ENA, speedCar / 2); digitalWrite(IN_3, HIGH); digitalWrite(IN_4, LOW);  analogWrite(ENB, speedCar); }
void belokKanan() { digitalWrite(IN_1, HIGH); digitalWrite(IN_2, LOW);  analogWrite(ENA, speedCar);    digitalWrite(IN_3, HIGH); digitalWrite(IN_4, LOW);  analogWrite(ENB, speedCar / 2); }
void mundurKiri() { digitalWrite(IN_1, LOW);  digitalWrite(IN_2, HIGH); analogWrite(ENA, speedCar / 2); digitalWrite(IN_3, LOW);  digitalWrite(IN_4, HIGH); analogWrite(ENB, speedCar); }
void mundurKanan(){ digitalWrite(IN_1, LOW);  digitalWrite(IN_2, HIGH); analogWrite(ENA, speedCar);    digitalWrite(IN_3, LOW);  digitalWrite(IN_4, HIGH); analogWrite(ENB, speedCar / 2); }
void stopRobot()  { digitalWrite(IN_1, LOW); digitalWrite(IN_2, LOW); analogWrite(ENA, 0); digitalWrite(IN_3, LOW); digitalWrite(IN_4, LOW); analogWrite(ENB, 0); }

// ================= NON-BLOCKING HANDLERS =================
void handleAttack() {
    if (!isAttacking) return;
    if (millis() - attackStart < 300) return;
    myservo2.write(60);
    isAttacking = false;
}

void handlePlanting() {
    if (!isPlanting) return;
    if (millis() - plantingStart < 300) return;
    myservo2.write(180);
    flagAttached = false;
    armUp = false;
    isPlanting = false;
}

void handlePicking() {
    if (!isPicking) return;
    if (millis() - pickingStart < 600) return;
    myservo.write(30);
    flagAttached = true;
    armUp = true;
    isPicking = false;
}

void handleXButton() {
    if (Ps3.data.button.cross) {
        if (!isGripping) {
            myservo2.write(50);
            isGripping = true;
        }
        xLatch = true;
        lastXPress = millis();
    } else {
        if (xLatch && millis() - lastXPress < 300) {
            myservo2.write(180);
            isGripping = false;
        }
        xLatch = false;
    }
}

// ================= PS3 HANDLERS =================
void notify() {
    stick_ly = Ps3.data.analog.stick.ly;
    stick_rx = Ps3.data.analog.stick.rx;

    if (Ps3.data.button.select && !selectLatch) {
        debugMode = !debugMode;
        selectLatch = true;
    }
    if (!Ps3.data.button.select) selectLatch = false;

    if (Ps3.data.button.circle && !circleLatch) {
        myservo.write(180);
        pickingStart = millis();
        myservo2.write(60);
        isPicking = true;
        circleLatch = true;
    }
    if (!Ps3.data.button.circle) circleLatch = false;

    if (Ps3.data.button.triangle) {
        myservo.write(180);
        plantingStart = millis();
        isPlanting = true;
    }

    if (Ps3.data.button.square && flagAttached) {
        myservo2.write(0);
        attackStart = millis();
        isAttacking = true;
    }

    handleXButton();

    if (Ps3.data.button.l2) {
        myservo.write(30);
        armUp = true;
    }
    if (Ps3.data.button.r2) {
        myservo.write(180);
        armUp = false;
    }

    if (Ps3.data.button.l1) speedCar = max(speedCar - speedStep, speedMin);
    if (Ps3.data.button.r1) speedCar = min(speedCar + speedStep, speedMax);
}

void onConnect() {
    Serial.println("PS3 Controller Connected.");
}

void setup() {
    Serial.begin(115200);
    connectWiFi();

    Ps3.attach(notify);
    Ps3.attachOnConnect(onConnect);
    //Ps3.begin("c0:14:3d:85:d6:6f"); //b1
    Ps3.begin("00:26:5c:06:9c:b3"); //o1
    //Ps3.begin("01:02:03:04:05:04"); // b1 

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
}

void loop() {
    if (!Ps3.isConnected()) return;

    bool btnPressed = (digitalRead(BTN_TOMBOL) == LOW);
    if (btnPressed && !btnWasPressed) {
        btnPressStart = millis();
        btnWasPressed = true;
    }
    if (!btnPressed && btnWasPressed) btnWasPressed = false;
    if (btnPressed && btnWasPressed && (millis() - btnPressStart > 50)) {
        sendDeathLog();
        btnWasPressed = false;
    }

    if (WiFi.status() != WL_CONNECTED && !debugMode) connectWiFi();
    if (!debugMode && millis() - lastStatusCheck > 500) {
        lastStatusCheck = millis();
        robotActive = cekStatus();
        if (!robotActive) stopRobot();
    }

    if (!robotActive && !debugMode) {
        if (millis() - lastBlink > 500) {
            lastBlink = millis();
            ledState = !ledState;
            digitalWrite(LED_TOMBOL, ledState);
        }
        stopRobot();
        return;
    } else {
        digitalWrite(LED_TOMBOL, LOW);
    }

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

    handleXButton();
    handleAttack();
    handlePlanting();
    handlePicking();

    delay(1);
}
