#include <Servo.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

Servo myServo;
int usds_TrigPin = 4;
int usds_EchoPin = 3;

#define WHITE 1
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

Adafruit_SH1106G oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Button pins
const int selectButtonPin = 11; // Select or confirm
const int incrementButtonPin = 9; // Increment minutes
const int decrementButtonPin = 10; // Decrement minutes

unsigned long lastDebounceTime = 0; // Last debounce time
unsigned long debounceDelay = 200; // Debounce delay in milliseconds

int currentMode = 0; // 0: Main menu, 1: Detection mode, 2: Schedule mode, 3: Schedule set
int feedHour = 0; // Feeding schedule hour
int feedMinute = 0; // Feeding schedule minute

unsigned long lastFeedTime = 0; // Track the last feeding time

void setup() {
  myServo.attach(2);
  Serial.begin(9600);
  Wire.begin();
  myServo.write(0);

  pinMode(usds_TrigPin, OUTPUT);
  pinMode(usds_EchoPin, INPUT);

  pinMode(selectButtonPin, INPUT_PULLUP);
  pinMode(incrementButtonPin, INPUT_PULLUP);
  pinMode(decrementButtonPin, INPUT_PULLUP);

  oled.begin(0x3C);
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(WHITE);

  showMainMenu(); // Display the main menu
}

void showMainMenu() {
  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.println("\n");
  oled.println("---------------------");
  oled.println("Select Mode:");
  oled.println("> Detect Pets");
  oled.println("> Set Time");
  oled.println("---------------------");
  oled.display(); // Display the main menu
}

void showDetectionMode() {
  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.println("\n");
  oled.println("---------------------");
  oled.println("DETECTING PETS");
  oled.println("---------------------");
  oled.display(); // Display detection mode
}

void showTimeSetting() {
  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.println("---------------------");
  oled.println("TIME INTERVAL:");
  oled.setCursor(0, 20);
  oled.print(feedHour);
  oled.print(":");
  if (feedMinute < 10) {
    oled.print("0");
  }
  oled.print(feedMinute);
  oled.setCursor(0, 40);
  oled.println("---------------------");
  oled.display(); // Display time setting
}

void showScheduleSet() {
  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.println("\n");
  oled.println("---------------------");
  oled.println("TIME SCHEDULE IS SET");
  oled.println("---------------------");
  oled.display(); // Display schedule set message
}

long measureDistance() {
  digitalWrite(usds_TrigPin, LOW);
  delayMicroseconds(2);

  digitalWrite(usds_TrigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(usds_TrigPin, LOW);

  long duration = pulseIn(usds_EchoPin, HIGH); // Duration of echo
  long distance = (duration / 2) * 0.0343; // Convert to centimeters
  
  return distance;
}

void loop() {
  unsigned long currentTime = millis(); // Current loop time
  long distance = measureDistance();
  Serial.println(distance);
  Serial.println("");
  // Debouncing logic
  if ((currentTime - lastDebounceTime) > debounceDelay) {
    if (digitalRead(incrementButtonPin) == HIGH) {
      if (currentMode == 0) { // Switch to detection mode
        currentMode = 1;
      } else if (currentMode == 2) { // Increment minutes
        feedMinute = (feedMinute + 1) % 60;
        if (feedMinute == 0) {
          feedHour = (feedHour + 1) % 24; // Increment hour if minutes wrap around
        }
      }
      lastDebounceTime = currentTime;
    }

    if (digitalRead(decrementButtonPin) == HIGH) {
      if (currentMode == 0) { // Switch to schedule mode
        currentMode = 2;
      } else if (currentMode == 2) { // Decrement minutes
        feedMinute = (feedMinute - 1 + 60) % 60;
        if (feedMinute == 59) {
          feedHour = (feedHour - 1 + 24) % 24; // Decrement hour if minutes wrap backward
        }
      }
      lastDebounceTime = currentTime;
    }

    if (digitalRead(selectButtonPin) == HIGH) {
      if (currentMode == 2) { // Confirm the schedule
        currentMode = 3; // Schedule set
      } else { // If in any mode other than the main menu, return to main menu
        currentMode = 0; // Return to the main menu
      }
      lastDebounceTime = currentTime;
    }
  }
// Auto detect mode
  if (currentMode == 0) { // Main menu
    showMainMenu(); 
  } else if (currentMode == 1) { 
    showDetectionMode(); 
    long distance = measureDistance(); 
    Serial.print(distance);
    if (distance < 5) { 
      myServo.write(90); 
      delay(1000); 
      myServo.write(0);
      delay(10000);
    }
  } else if (currentMode == 2) { 
    showTimeSetting(); 
  } else if (currentMode == 3) { 
    showScheduleSet(); 
  }
  
  if (currentMode == 3) { 
    unsigned long scheduledTime = (unsigned)(feedHour * 3600 + feedMinute * 60) * 1000; // Convert scheduled time to milliseconds
    unsigned long elapsedTime = currentTime - lastFeedTime;
    if (elapsedTime >= scheduledTime) {
      myServo.write(90); 
      oled.print(distance);
      delay(1000); 
      myServo.write(0); // Close feeder
      delay(10000);
      lastFeedTime = currentTime; // Update last feeding time
    }
  }
}
