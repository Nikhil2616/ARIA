/*
 * ARIA — Arduino UNO (MUSCLE)
 * Responsibilities:
 *   - Control DC motors via L298N motor driver
 *   - Read 2x HC-SR04 ultrasonic sensors (front + back)
 *   - Read PIR motion sensor
 *   - Send sensor data to ESP32_A via Serial
 *   - Receive motor commands from ESP32_A via Serial
 */

// ================= MOTOR PINS (L298N) =================
// Left Motor
#define ENA   5    // PWM speed control
#define IN1   6
#define IN2   7

// Right Motor
#define ENB   10   // PWM speed control
#define IN3   8
#define IN4   9

// ================= ULTRASONIC PINS =================
// Front HC-SR04
#define TRIG_FRONT  A0
#define ECHO_FRONT  A1

// Back HC-SR04
#define TRIG_BACK   A2
#define ECHO_BACK   A3

// ================= PIR SENSOR =================
#define PIR_PIN  12

// ================= SPEED SETTINGS =================
#define SPEED_SLOW    100  // PWM 0-255
#define SPEED_NORMAL  180
#define SPEED_FAST    255

// ================= STATE =================
String currentCommand = "stop";
unsigned long lastSensorSend = 0;
#define SENSOR_INTERVAL 500  // send sensor data every 500ms

// ================= ULTRASONIC HELPER =================
long readDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, 30000); // 30ms timeout
  if (duration == 0) return 999; // no echo = clear path
  long distance = duration * 0.034 / 2;
  return distance;
}

// ================= MOTOR CONTROL =================
void setMotors(int leftSpeed, int leftDir1, int leftDir2,
               int rightSpeed, int rightDir1, int rightDir2) {
  analogWrite(ENA, leftSpeed);
  digitalWrite(IN1, leftDir1);
  digitalWrite(IN2, leftDir2);

  analogWrite(ENB, rightSpeed);
  digitalWrite(IN3, rightDir1);
  digitalWrite(IN4, rightDir2);
}

void moveForward(int spd) {
  setMotors(spd, HIGH, LOW, spd, HIGH, LOW);
}

void moveBackward(int spd) {
  setMotors(spd, LOW, HIGH, spd, LOW, HIGH);
}

void turnLeft(int spd) {
  setMotors(spd / 2, LOW, HIGH, spd, HIGH, LOW);
}

void turnRight(int spd) {
  setMotors(spd, HIGH, LOW, spd / 2, LOW, HIGH);
}

void stopMotors() {
  setMotors(0, LOW, LOW, 0, LOW, LOW);
}

int parseSpeed(String speedStr) {
  if (speedStr == "slow")   return SPEED_SLOW;
  if (speedStr == "fast")   return SPEED_FAST;
  return SPEED_NORMAL;
}

void executeCommand(String direction, String speed) {
  int spd = parseSpeed(speed);

  if (direction == "forward")  moveForward(spd);
  else if (direction == "backward") moveBackward(spd);
  else if (direction == "left")     turnLeft(spd);
  else if (direction == "right")    turnRight(spd);
  else stopMotors();

  Serial.println("✅ Executing: " + direction + " at speed " + speed);
}

// ================= PARSE SERIAL COMMAND =================
// Format from ESP32_A: "forward:normal\n"
void parseSerialCommand(String line) {
  line.trim();
  int colonIdx = line.indexOf(':');
  if (colonIdx == -1) return;

  String direction = line.substring(0, colonIdx);
  String speed     = line.substring(colonIdx + 1);

  executeCommand(direction, speed);
}

// ================= SETUP =================
void setup() {
  Serial.begin(9600); // Communication with ESP32_A

  // Motor pins
  pinMode(ENA, OUTPUT); pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(ENB, OUTPUT); pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);

  // Ultrasonic pins
  pinMode(TRIG_FRONT, OUTPUT); pinMode(ECHO_FRONT, INPUT);
  pinMode(TRIG_BACK,  OUTPUT); pinMode(ECHO_BACK,  INPUT);

  // PIR
  pinMode(PIR_PIN, INPUT);

  stopMotors();
  Serial.println("ARIA Arduino READY");
}

// ================= LOOP =================
void loop() {
  // Read incoming commands from ESP32_A
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    parseSerialCommand(line);
  }

  // Send sensor data every 500ms
  if (millis() - lastSensorSend > SENSOR_INTERVAL) {
    long frontDist = readDistance(TRIG_FRONT, ECHO_FRONT);
    long backDist  = readDistance(TRIG_BACK,  ECHO_BACK);
    bool pirState  = digitalRead(PIR_PIN);

    // Format: "DIST:F:34:B:80"
    Serial.print("DIST:F:");
    Serial.print(frontDist);
    Serial.print(":B:");
    Serial.println(backDist);

    // If PIR detects motion while moving, stop for safety
    if (pirState) {
      Serial.println("PIR:MOTION");
    }

    lastSensorSend = millis();
  }
}
