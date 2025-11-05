#define BLYNK_TEMPLATE_ID SECRET_TEMPLATE_ID
#define BLYNK_TEMPLATE_NAME SECRET_TEMPLATE_NAME
#define BLYNK_AUTH_TOKEN SECRET_AUTH_TOKEN

#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <SPI.h>
#include "R200.h"
#include <ESP32Servo.h>
#include <time.h>
#include <map>
#include <vector>
#include <EEPROM.h>          
#include "esp_task_wdt.h"     
#include <HX711_ADC.h>        
#include <Preferences.h>     

// Wi-Fi Credentials
const char* ssid = "kibbler-sai";
const char* pass = "kibbler-pass";

// LED Pins
#define RED_LED_PIN 12
#define GREEN_LED_PIN 14
#define YELLOW_LED_PIN 13

// Status Flags
bool isInitializing = true;
bool isInitialized = false;

// Firebase Config
#define FIREBASE_API_KEY SECRET_FIREBASE_API_KEY
#define FIREBASE_URL SECRET_FIREBASE_URL
#define DEFAULT_PET_NAMES_PATH SECRET_DEFAULT_PET_NAMES_PATH
#define PET_REGISTRY_PATH SECRET_PET_REGISTRY_PATH
#define SLEEP_SETTINGS_PATH SECRET_SLEEP_SETTINGS_PATH
#define DEVICE_STATUS_PATH SECRET_DEVICE_STATUS_PATH
#define TAG_REGISTRATION_MODE_PATH SECRET_TAG_REGISTRATION_MODE_PATH
#define LAST_DETECTED_TAG_PATH SECRET_LAST_DETECTED_TAG_PATH
#define TARE_TRIGGER_PATH SECRET_TARE_TRIGGER

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// Firebase Connection Status
bool firebaseConnected = false;
unsigned long lastFirebaseCheck = 0;
const unsigned long firebaseCheckInterval = 30000;
byte firebaseRetryCount = 0;
const byte maxRetries = 3;

// Pet Name Management
std::vector<String> petNames;
bool petNamesLoaded = false;
unsigned long lastPetNamesUpdate = 0;
const unsigned long PET_NAMES_UPDATE_INTERVAL = 3600000; 
bool tagRegistrationMode = false;

// Message tracking
std::map<String, unsigned long> lastInsufficientWriteTimes;
std::map<String, unsigned long> lastIntervalActiveWriteTimes;
const unsigned long MESSAGE_REPEAT_INTERVAL = 7200000; 

// SOLAR/BATTERY CONFIG
#define BATTERY_CAPACITY_WH 153.6f  // 12.8V × 12Ah
#define AVG_POWER_IDLE_W 1.75f
#define AVG_POWER_PEAK_W 4.5f

// Hardware Setup
#define SERVO_PIN 21
#define HX711_DT_PIN 32   // Data pin (DT) - MATCHES COMPONENT_TESTING
#define HX711_SCK_PIN 33  // Clock pin (SCK) - MATCHES COMPONENT_TESTING
#define CONTAINER_TRIG_PIN 27
#define CONTAINER_ECHO_PIN 26
#define RFID_RX_PIN 16
#define RFID_TX_PIN 17
#define BATTERY_PIN 35

Servo foodServo;
R200 rfid;
HX711_ADC LoadCell(HX711_DT_PIN, HX711_SCK_PIN);
Preferences preferences;  

byte feedingIntervalHours = 4;
unsigned long feedingIntervalMs = feedingIntervalHours * 3600000UL;
std::map<String, unsigned long> lastFedTimes;

// Tray Load Cell Variables
const float TRAY_FULL_WEIGHT = 375.0;  
const float TRAY_EMPTY_WEIGHT = 65.0;   
float weightReadings[2] = { 0.0 };   
byte readingIndex = 0;
byte currentTrayLevel = 0;
byte lastValidTrayLevel = 0;
bool hasValidReading = false;
float calibrationFactor = -471.50;
float lastRecordedWeight = 0.0;       

// Container USDS Variables
const byte CONTAINER_MIN_DISTANCE = 14;
const byte CONTAINER_MAX_DISTANCE = 23;
byte containerDistanceReadings[2] = { 0 };  
byte containerReadingIndex = 0;
byte currentContainerLevel = 0;
byte lastValidContainerLevel = 0;
bool hasValidContainerReading = false;

// Food Monitoring
unsigned long lastEmptyTime = 0;
bool wasEmptyRecently = false;
const unsigned long STALE_FOOD_THRESHOLD = 86400000UL; 
bool staleAlertActive = false;

// Historical Data
int todayDispenseCount = 0;
int weekDispenseCount = 0;
byte todayUniquePets = 0;
int totalUniqueUIDs = 0;
String todayFedUIDs;
String allTimeUIDs;
String lastResetDate;
byte batteryPercentage = 100;
unsigned long lastStatusUpdate = 0;
const unsigned long statusUpdateInterval = 60000;

// Battery monitoring constants
const float DIVIDER_RATIO = 5.57;
const float BATTERY_EMPTY = 10.0;
const float BATTERY_FULL = 14.6;
const float ADC_REF_VOLTAGE = 3.3;
const int BATTERY_SAMPLES = 10;

// Solar/Battery Tracking
bool isCharging = false;
int chargeCycles = 0;
unsigned long lastFullChargeTime = 0;
float avgPowerConsumptionW = AVG_POWER_IDLE_W;
unsigned long lastPowerUpdate = 0;
float dailyEnergyUsedWh = 0.0;
float batteryVoltage = 0.0;

// Energy Tracking
String currentEnergyDate = "";
unsigned long lastEnergyReset = 0;

String lastUID;
byte portionLevel = 100; 
const uint8_t blankUid[12] = { 0 };

byte cachedTrayLevel = 0;
byte cachedContainerLevel = 0;
unsigned long lastTrayReadTime = 0;
unsigned long lastContainerReadTime = 0;
const unsigned long SENSOR_CACHE_DURATION = 500; 

void updateLEDs() {
  Serial.print("Updating LEDs - tagRegistrationMode: ");
  Serial.print(tagRegistrationMode);
  Serial.print(", isInitializing: ");
  Serial.print(isInitializing);
  Serial.print(", isInitialized: ");
  Serial.println(isInitialized);
  if (tagRegistrationMode) {
    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(RED_LED_PIN, LOW);
  } else if (isInitializing) {
    digitalWrite(RED_LED_PIN, HIGH);
    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(YELLOW_LED_PIN, LOW);
  } else if (isInitialized) {
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(YELLOW_LED_PIN, HIGH);
  } else {
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(YELLOW_LED_PIN, LOW);
  }
}

void blinkLED(byte ledPin, byte times) {
  for (byte i = 0; i < times; i++) {
    digitalWrite(ledPin, HIGH);
    delay(150); 
    digitalWrite(ledPin, LOW);
    delay(150);
  }
  updateLEDs();
}

void blinkAllLEDs(byte times) {
  for (byte i = 0; i < times; i++) {
    digitalWrite(RED_LED_PIN, HIGH);
    digitalWrite(GREEN_LED_PIN, HIGH);
    digitalWrite(YELLOW_LED_PIN, HIGH);
    delay(200); 
    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(YELLOW_LED_PIN, LOW);
    delay(200); 
  }
  updateLEDs();
}

void loadPetNamesFromFirebase() {
  if (!firebaseConnected) {
    Serial.println("Firebase not connected, cannot load pet names");
    return;
  }

  Serial.println("Loading default pet names from Firebase...");

  if (Firebase.RTDB.getArray(&fbdo, DEFAULT_PET_NAMES_PATH)) {
    FirebaseJsonArray* arr = fbdo.jsonArrayPtr();
    size_t len = arr->size();

    petNames.clear();
    Serial.print("Found ");
    Serial.print(len);
    Serial.println(" pet names");

    for (size_t i = 0; i < len; i++) {
      FirebaseJsonData data;
      if (arr->get(data, i) && data.typeNum == FirebaseJson::JSON_STRING) {
        String name = data.stringValue;
        if (name.length() > 0) {
          petNames.push_back(name);
          Serial.print("Added pet name: ");
          Serial.println(name);
        }
      }
    }

    if (petNames.size() > 0) {
      petNamesLoaded = true;
      lastPetNamesUpdate = millis();
      Serial.println("Successfully loaded default pet names");
    } else {
      Serial.println("No pet names found in array");
    }
  } else {
    Serial.print("Failed to load pet names: ");
    Serial.println(fbdo.errorReason());
    Serial.print("HTTP Code: ");
    Serial.println(fbdo.httpCode());
    Serial.print("Payload: ");
    Serial.println(fbdo.payload());
  }
}

String getAvailablePetName(const String& uid) {
  if (firebaseConnected) {
    String path = String(PET_REGISTRY_PATH) + "/" + uid;
    if (Firebase.RTDB.getString(&fbdo, path)) {
      return fbdo.stringData();
    }
  }

  std::vector<String> usedNames;
  if (firebaseConnected && Firebase.RTDB.getJSON(&fbdo, PET_REGISTRY_PATH)) {
    FirebaseJson* json = fbdo.jsonObjectPtr();
    size_t count = json->iteratorBegin();

    for (size_t i = 0; i < count; i++) {
      FirebaseJsonData data;
      if (json->get(data, i) && data.typeNum == FirebaseJson::JSON_STRING) {
        usedNames.push_back(data.stringValue);
      }
    }
    json->iteratorEnd();
  }

  for (const String& name : petNames) {
    bool nameAvailable = true;
    for (const String& usedName : usedNames) {
      if (name.equalsIgnoreCase(usedName)) {
        nameAvailable = false;
        break;
      }
    }
    if (nameAvailable) {
      Serial.print("Assigning available default name: ");
      Serial.print(name);
      Serial.print(" to UID ");
      Serial.println(uid);
      return name;
    }
  }

  static int petNumber = 1;
  String newName = "Pet_" + String(petNumber++);
  Serial.print("Generated new name: ");
  Serial.print(newName);
  Serial.print(" for UID ");
  Serial.println(uid);
  return newName;
}

void removeAssignedNameFromDefaults(const String& name) {
  for (auto it = petNames.begin(); it != petNames.end();) {
    if (it->equalsIgnoreCase(name)) {
      Serial.print("Removing assigned name from defaults: ");
      Serial.println(*it);
      it = petNames.erase(it);
    } else {
      ++it;
    }
  }

  if (firebaseConnected) {
    FirebaseJsonArray updatedNames;
    for (const String& n : petNames) {
      updatedNames.add(n);
    }

    if (!Firebase.RTDB.setArray(&fbdo, DEFAULT_PET_NAMES_PATH, &updatedNames)) {
      Serial.println("Failed to update default names in Firebase");
    }
  }
}

std::map<String, String> petNameCache;

String getPetNameForUID(const String& uid) {
  auto it = petNameCache.find(uid);
  if (it != petNameCache.end()) {
    return it->second;  
  }
  
  if (firebaseConnected) {
    String path = String(PET_REGISTRY_PATH) + "/" + uid;
    if (Firebase.RTDB.getString(&fbdo, path)) {
      String name = fbdo.stringData();
      if (!name.isEmpty()) {
        petNameCache[uid] = name; 
        return name;
      }
    }
  }

  return "";  
}

String getDateTimeFromMillis(unsigned long millisTime) {
  time_t now = millisTime / 1000;
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}

String getCurrentDate() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char dateStr[11];
    strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &timeinfo);
    return String(dateStr);
  }
  return "";
}

String getCurrentDateTime() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char timeStr[20];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(timeStr);
  }
  return "";
}

String getCurrentTime() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char timeStr[6];
    strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
    return String(timeStr);
  }
  return "";
}

byte getDayOfWeek() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    return timeinfo.tm_wday;
  }
  return 0;
}

void updatePowerMetrics(float currentVoltage) {
  static float lastVoltage = 0.0;
  static float voltageHistory[3] = { 0 };
  static byte historyIndex = 0;

  voltageHistory[historyIndex] = currentVoltage;
  historyIndex = (historyIndex + 1) % 3;

  if (voltageHistory[2] != 0) {
    float trend = (voltageHistory[0] + voltageHistory[1] + voltageHistory[2]) / 3;
    isCharging = (currentVoltage > trend + 0.05);
  }
  lastVoltage = currentVoltage;

  static bool wasFull = false;
  if (batteryPercentage >= 95 && !wasFull) {
    chargeCycles++;
    lastFullChargeTime = millis();
    wasFull = true;
  } else if (batteryPercentage < 90) {
    wasFull = false;
  }

  avgPowerConsumptionW = foodServo.attached() ? AVG_POWER_PEAK_W : AVG_POWER_IDLE_W;
}

float calculateRuntimeHours() {
  if (isCharging) return -1.0;
  float remainingWh = (batteryPercentage / 100.0f) * BATTERY_CAPACITY_WH;
  return remainingWh / avgPowerConsumptionW;
}

void sendPowerDataToFirebase() {
  if (millis() - lastPowerUpdate > 2000) {
    float intervalEnergy = avgPowerConsumptionW * 2.0 / 3600.0;
    dailyEnergyUsedWh += intervalEnergy;

    static byte lastSentBatteryLevel = 255;
    static unsigned long lastPowerFirebaseWrite = 0;
    
    bool batteryChanged = abs((int)batteryPercentage - (int)lastSentBatteryLevel) >= 5;
    bool heartbeatDue = (millis() - lastPowerFirebaseWrite > 60000); 
    
    if (firebaseConnected && (batteryChanged || heartbeatDue)) {
      FirebaseJson powerData;
      powerData.set("timestamp", getCurrentDateTime());
      powerData.set("voltage", batteryVoltage);
      powerData.set("battery_level", batteryPercentage);
      powerData.set("is_charging", isCharging);
      powerData.set("current_power_w", avgPowerConsumptionW);
      powerData.set("daily_energy_wh", dailyEnergyUsedWh);
      powerData.set("runtime_hours", calculateRuntimeHours());
      powerData.set("charge_cycles", chargeCycles);

      if (lastFullChargeTime > 0) {
        powerData.set("last_full_charge", getDateTimeFromMillis(lastFullChargeTime));
      }

      String path = "/devices/kibbler_001/power_status";
      if (!Firebase.RTDB.setJSON(&fbdo, path, &powerData)) {
        Serial.println("Failed to update power data: " + fbdo.errorReason());
        firebaseConnected = false;
      } else {
        lastSentBatteryLevel = batteryPercentage;
        lastPowerFirebaseWrite = millis();
        
        String today = getCurrentDate();
        if (today != currentEnergyDate) {
          String historyPath = "/devices/kibbler_001/energy_history/" + currentEnergyDate;
          Firebase.RTDB.setFloat(&fbdo, historyPath, dailyEnergyUsedWh);
          currentEnergyDate = today;
          dailyEnergyUsedWh = 0;
        }
      }
    }

    lastPowerUpdate = millis();
  }
}

void updateBatteryLevel() {
  float totalVoltage = 0;
  int validSamples = 0;

  for (int i = 0; i < 5; i++) analogRead(BATTERY_PIN);

  for (int i = 0; i < BATTERY_SAMPLES; i++) {
    int adcValue = analogRead(BATTERY_PIN);
    float adcVoltage = adcValue * (ADC_REF_VOLTAGE / 4095.0);
    batteryVoltage = adcVoltage * DIVIDER_RATIO;

    if (batteryVoltage >= 1.0 && batteryVoltage <= 20.0) {
      totalVoltage += batteryVoltage;
      validSamples++;
    }
  }

  if (validSamples == 0) {
    Serial.println("⚠️ BATTERY] Invalid readings - check connection");
    return;
  }

  float avgVoltage = totalVoltage / validSamples;
  batteryVoltage = avgVoltage;

  if (avgVoltage >= 13.9) batteryPercentage = 100;
  else if (avgVoltage >= 13.8) batteryPercentage = 95;
  else if (avgVoltage >= 13.7) batteryPercentage = 90;
  else if (avgVoltage >= 13.6) batteryPercentage = 85;
  else if (avgVoltage >= 13.5) batteryPercentage = 80;
  else if (avgVoltage >= 13.4) batteryPercentage = 75;
  else if (avgVoltage >= 13.2) batteryPercentage = 65;
  else if (avgVoltage >= 13.0) batteryPercentage = 55;
  else if (avgVoltage >= 12.9) batteryPercentage = 45;
  else if (avgVoltage >= 12.8) batteryPercentage = 35;
  else if (avgVoltage >= 12.6) batteryPercentage = 25;
  else if (avgVoltage >= 12.4) batteryPercentage = 15;
  else if (avgVoltage >= 12.2) batteryPercentage = 5;
  else batteryPercentage = 0;

  Serial.print("🔋 [BATTERY] ");
  Serial.print(avgVoltage, 2);
  Serial.print("V (");
  Serial.print(batteryPercentage);
  Serial.println("%)");

  updatePowerMetrics(avgVoltage);

  if (firebaseConnected) {
    if (!Firebase.RTDB.setFloat(&fbdo, "/devices/kibbler_001/device_status/battery_voltage", avgVoltage)) {
      Serial.println("   [⚠️ FIREBASE] Failed to write voltage: " + fbdo.errorReason());
    }

    if (!Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/device_status/battery_level", batteryPercentage)) {
      Serial.println("   [⚠️ FIREBASE] Failed to write percentage: " + fbdo.errorReason());
    }

    Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/device_status/battery_last_updated", getCurrentDateTime());
  }
}

void resetDailyCounters() {
  String currentDate = getCurrentDate();

  if (currentDate != lastResetDate && currentDate != "") {
    if (firebaseConnected && lastResetDate != "") {
      Firebase.RTDB.setFloat(&fbdo,
                             "/devices/kibbler_001/energy_history/" + lastResetDate,
                             dailyEnergyUsedWh);
    }

    todayDispenseCount = 0;
    todayUniquePets = 0;
    todayFedUIDs = "";
    dailyEnergyUsedWh = 0.0;
    lastResetDate = currentDate;

    if (firebaseConnected) {
      Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/stats/today_dispense_count", todayDispenseCount);
      Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/stats/today_unique_pets", todayUniquePets);
      Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/stats/last_reset_date", lastResetDate);
    }
  }
}

void resetWeeklyCounters() {
  static String lastResetWeek;
  static bool needsResetSync = false;

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char currentWeek[10];
    strftime(currentWeek, sizeof(currentWeek), "%G-W%V", &timeinfo);

    if (lastResetWeek != currentWeek) {
      weekDispenseCount = 0;
      lastResetWeek = currentWeek;
      needsResetSync = true;
    }

    if (needsResetSync && firebaseConnected) {
      if (Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/stats/week_dispense_count", weekDispenseCount)) {
        needsResetSync = false;
      }
    }
  }
}

void updateUIDTracking(const String& uid) {
  if (todayFedUIDs.indexOf(uid) == -1) {
    todayFedUIDs += (todayFedUIDs.length() > 0 ? "," : "") + uid;
    todayUniquePets++;
    if (firebaseConnected) {
      Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/stats/today_unique_pets", todayUniquePets);
      Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/stats/today_fed_uids", todayFedUIDs);
    }
  }

  if (allTimeUIDs.indexOf(uid) == -1) {
    allTimeUIDs += (allTimeUIDs.length() > 0 ? "," : "") + uid;
    totalUniqueUIDs++;
    if (firebaseConnected) {
      Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/stats/total_unique_uids", totalUniqueUIDs);
      Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/stats/all_time_uids", allTimeUIDs);
    }
  }
}

void recordFeedingEvent(const String& uid, byte dispenseAmount, byte trayLevel) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to get local time for feeding event");
    return;
  }

  char timestampStr[25];
  strftime(timestampStr, sizeof(timestampStr), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

  String date = getCurrentDate();
  String time = getCurrentTime();
  String petName = getPetNameForUID(uid);

  todayDispenseCount++;
  weekDispenseCount++;

  if (uid != "") {
    updateUIDTracking(uid);
  }

  FirebaseJson feedingRecord;
  feedingRecord.set("timestamp", timestampStr);
  feedingRecord.set("date", date);
  feedingRecord.set("time", time);
  feedingRecord.set("pet_name", petName);
  feedingRecord.set("uid", uid);
  feedingRecord.set("dispense_amount", dispenseAmount);
  feedingRecord.set("tray_level_before", trayLevel);
  feedingRecord.set("container_level", currentContainerLevel);
  feedingRecord.set("battery_level", batteryPercentage);

  if (firebaseConnected) {
    String recordPath = "/devices/kibbler_001/feeding_history/" + String(timestampStr);
    String dailyPath = "/devices/kibbler_001/history/daily/" + date + "/feedings/" + String(timestampStr);

    if (!Firebase.RTDB.setJSON(&fbdo, recordPath.c_str(), &feedingRecord)) {
      Serial.println("Failed to save feeding record: " + fbdo.errorReason());
    }

    if (!Firebase.RTDB.setJSON(&fbdo, dailyPath.c_str(), &feedingRecord)) {
      Serial.println("Failed to save daily record: " + fbdo.errorReason());
    }

    Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/stats/today_dispense_count", todayDispenseCount);
    Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/stats/week_dispense_count", weekDispenseCount);
    Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/stats/last_fed_time", timestampStr);
    Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/stats/last_fed_pet", petName + " (" + uid + ")");

    String activityMessage = "Fed " + petName + " " + String(dispenseAmount) + "% portion";
    recordRecentActivity(activityMessage, uid, petName);  // FIXED: Pass UID and petName
  }
}

void recordRecentActivity(const String& activityMessage, const String& uid, const String& petName) {
  if (!firebaseConnected) return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to get local time for activity recording");
    return;
  }

  char timestampStr[25];
  strftime(timestampStr, sizeof(timestampStr), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

  static String lastActivityMessage;
  static String lastActivityTime;
  static unsigned long lastActivityWriteTime = 0;
  const unsigned long activityDebounceInterval = 5000;

  if (activityMessage == lastActivityMessage && strncmp(timestampStr, lastActivityTime.c_str(), 16) == 0 && (millis() - lastActivityWriteTime < activityDebounceInterval)) {
    return;
  }

  String timestampPath = "/devices/kibbler_001/recent_activities/" + String(timestampStr);

  FirebaseJson activityRecord;
  activityRecord.set("message", activityMessage);
  activityRecord.set("timestamp", timestampStr);
  activityRecord.set("pet_name", petName.isEmpty() ? "Unknown" : petName);  
  if (!uid.isEmpty()) {
    activityRecord.set("uid", uid); 
  }

  if (!Firebase.RTDB.setJSON(&fbdo, timestampPath.c_str(), &activityRecord)) {
    Serial.println("Failed to log activity: " + fbdo.errorReason());
  } else {
    Serial.println("Activity logged: " + activityMessage);
    lastActivityMessage = activityMessage;
    lastActivityTime = timestampStr;
    lastActivityWriteTime = millis();
  }

  static unsigned long lastCleanupTime = 0;
  const unsigned long cleanupInterval = 3600000; 
  
  if (millis() - lastCleanupTime > cleanupInterval) {
    lastCleanupTime = millis();
    
    if (Firebase.RTDB.getJSON(&fbdo, "/devices/kibbler_001/recent_activities")) {
      FirebaseJson* json = fbdo.jsonObjectPtr();
      size_t count = json->iteratorBegin();

      if (count > 50) {
        std::vector<String> keysToDelete;
        keysToDelete.reserve(count - 50);  
        
        for (size_t i = 0; i < count - 50; i++) {
          String key, value;
          int type;
          json->iteratorGet(i, type, key, value);
          keysToDelete.push_back(key);
        }

        Serial.printf("Cleaning up %d old activity entries\n", keysToDelete.size());
        for (const String& key : keysToDelete) {
          String deletePath = "/devices/kibbler_001/recent_activities/" + key;
          Firebase.RTDB.deleteNode(&fbdo, deletePath);
        }
      }
      json->iteratorEnd();
    }
  }
}

void verifyPetRegistration(const String& uid) {
  if (!firebaseConnected) return;

  String path = String(PET_REGISTRY_PATH) + "/" + uid;
  if (Firebase.RTDB.getString(&fbdo, path)) {
    String name = fbdo.stringData();
    Serial.print("Verification - UID ");
    Serial.print(uid);
    Serial.print(" is registered as: ");
    Serial.println(name);
  } else {
    Serial.print("Verification - UID ");
    Serial.print(uid);
    Serial.println(" is NOT registered!");
  }
}

void updateDeviceStatus() {
  if (!firebaseConnected) return;

  FirebaseJson statusObj;
  statusObj.set("status", "online");
  statusObj.set("last_seen", getCurrentDateTime());
  statusObj.set("battery_level", batteryPercentage);
  statusObj.set("container_level", currentContainerLevel);
  statusObj.set("tray_level", currentTrayLevel);
  statusObj.set("wifi_signal", WiFi.RSSI());
  statusObj.set("uptime", millis() / 1000);
  statusObj.set("feeding_interval_hours", (int)feedingIntervalHours);
  statusObj.set("portion_level", portionLevel);

  if (!Firebase.RTDB.setJSON(&fbdo, "/devices/kibbler_001/device_status", &statusObj)) {
    Serial.println("Failed to update device status: " + fbdo.errorReason());
  }
}

void checkPetNames() {
  static unsigned long lastCheck = 0;
  const unsigned long checkInterval = 3600000; 

  if (millis() - lastCheck > checkInterval) {
    lastCheck = millis();

    if (!petNamesLoaded || petNames.size() == 0) {
      Serial.println("Pet names not loaded, retrying...");
      loadPetNamesFromFirebase();
    }

    Serial.println("Refreshing pet name cache...");
    loadPetNameCache();

    if (firebaseConnected) {
      Serial.println("Current pet registry:");
      if (Firebase.RTDB.getJSON(&fbdo, PET_REGISTRY_PATH)) {
        FirebaseJson* json = fbdo.jsonObjectPtr();
        size_t count = json->iteratorBegin();

        for (size_t i = 0; i < count; i++) {
          String uid, name;
          int type;
          json->iteratorGet(i, type, uid, name);
          Serial.print(uid);
          Serial.print(": ");
          Serial.println(name);
        }
        json->iteratorEnd();
      } else {
        Serial.println("Failed to read pet registry");
      }
    }
  }
}

void loadLastFedTimes() {
  if (Firebase.RTDB.getJSON(&fbdo, "/devices/kibbler_001/pets/last_fed_times")) {
    FirebaseJson& json = fbdo.jsonObject();
    size_t count = json.iteratorBegin();

    for (size_t i = 0; i < count; i++) {
      int type;
      String key, value;
      if (json.iteratorGet(i, type, key, value)) {
        lastFedTimes[key] = value.toInt() * 1000;
      }
    }
    json.iteratorEnd();
  }
}

void loadPetNameCache() {
  Serial.println("Loading pet registry into cache...");
  if (Firebase.RTDB.getJSON(&fbdo, PET_REGISTRY_PATH)) {
    petNameCache.clear();
    
    FirebaseJson* json = fbdo.jsonObjectPtr();
    size_t count = json->iteratorBegin();

    for (size_t i = 0; i < count; i++) {
      String uid, name;
      int type;
      json->iteratorGet(i, type, uid, name);
      
      name.replace("\"", "");
      
      if (uid.length() > 0 && name.length() > 0) {
        petNameCache[uid] = name;
        Serial.print("  Cached: ");
        Serial.print(uid);
        Serial.print(" → ");
        Serial.println(name);
      }
    }
    json->iteratorEnd();
    Serial.print("✓ Loaded ");
    Serial.print(petNameCache.size());
    Serial.println(" pet names into cache");
  } else {
    Serial.println("Failed to load pet registry for cache");
  }
}

void cleanOldFedTimes() {
  unsigned long currentTime = millis();
  std::vector<String> toRemove;

  for (const auto& pair : lastFedTimes) {
    if (currentTime - pair.second > (feedingIntervalMs * 2)) {
      toRemove.push_back(pair.first);
    }
  }

  for (const String& uid : toRemove) {
    lastFedTimes.erase(uid);
  }

  toRemove.clear();
  for (const auto& pair : lastInsufficientWriteTimes) {
    if (currentTime - pair.second > (MESSAGE_REPEAT_INTERVAL * 2)) {
      toRemove.push_back(pair.first);
    }
  }
  for (const String& uid : toRemove) {
    lastInsufficientWriteTimes.erase(uid);
  }

  toRemove.clear();
  for (const auto& pair : lastIntervalActiveWriteTimes) {
    if (currentTime - pair.second > (MESSAGE_REPEAT_INTERVAL * 2)) {
      toRemove.push_back(pair.first);
    }
  }
  for (const String& uid : toRemove) {
    lastIntervalActiveWriteTimes.erase(uid);
  }
}

bool checkFirebaseConnection() {
  if (Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/others/connection_test", "connected")) {
    firebaseConnected = true;
    firebaseRetryCount = 0;
    return true;
  }

  firebaseConnected = false;
  firebaseRetryCount++;
  return false;
}

void initializePetRegistry() {
  if (firebaseConnected) {
    if (!Firebase.RTDB.getJSON(&fbdo, PET_REGISTRY_PATH)) {
      FirebaseJson json;
      if (Firebase.RTDB.setJSON(&fbdo, PET_REGISTRY_PATH, &json)) {
        Serial.println("Created new pet registry");
      } else {
        Serial.println("Failed to create pet registry: " + fbdo.errorReason());
      }
    }
  }
}

bool initializeFirebase() {
  config.api_key = FIREBASE_API_KEY;
  config.database_url = FIREBASE_URL;
  config.signer.tokens.legacy_token = "YfL5gCz4FTovRC6WF3yg1uUgjpTicGomrwTC73SZ";

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  delay(3000);

  if (checkFirebaseConnection()) {
    Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/others/last_connected", getCurrentDateTime());
    initializeStats();
    initializePetRegistry();
    return true;
  }
  return false;
}

void initializeStats() {
  if (Firebase.RTDB.getString(&fbdo, "/devices/kibbler_001/stats/last_reset_date")) {
    lastResetDate = fbdo.stringData();
  }

  if (Firebase.RTDB.getInt(&fbdo, "/devices/kibbler_001/stats/today_dispense_count")) {
    todayDispenseCount = fbdo.intData();
  }

  if (Firebase.RTDB.getInt(&fbdo, "/devices/kibbler_001/stats/week_dispense_count")) {
    weekDispenseCount = fbdo.intData();
  }

  if (Firebase.RTDB.getInt(&fbdo, "/devices/kibbler_001/stats/today_unique_pets")) {
    todayUniquePets = fbdo.intData();
  }

  if (Firebase.RTDB.getInt(&fbdo, "/devices/kibbler_001/stats/total_unique_uids")) {
    totalUniqueUIDs = fbdo.intData();
  }

  if (Firebase.RTDB.getString(&fbdo, "/devices/kibbler_001/stats/today_fed_uids")) {
    todayFedUIDs = fbdo.stringData();
  }

  if (Firebase.RTDB.getString(&fbdo, "/devices/kibbler_001/stats/all_time_uids")) {
    allTimeUIDs = fbdo.stringData();
  }

  if (Firebase.RTDB.getInt(&fbdo, "/devices/kibbler_001/device_status/feeding_interval_hours")) {
    byte newInterval = fbdo.intData();
    if (newInterval != feedingIntervalHours) {
      feedingIntervalHours = newInterval;
      feedingIntervalMs = feedingIntervalHours * 3600000UL;
      Serial.print("⚙️  Feeding interval updated: ");
      Serial.print(feedingIntervalHours);
      Serial.println(" hours");
      
      blinkAllLEDs(3);
    }
  }

  if (Firebase.RTDB.getInt(&fbdo, "/devices/kibbler_001/device_status/portion_level")) {
    int newPortion = fbdo.intData();
    portionLevel = (newPortion >= 25 && newPortion <= 100) ? newPortion : 100;
  }

  loadLastFedTimes();
  loadPetNameCache();  
  loadPetNamesFromFirebase();
}

void reconnectFirebase() {
  if (firebaseRetryCount < maxRetries) {
    if (initializeFirebase()) {
      return;
    }
  } else {
    firebaseRetryCount = 0;
  }
}

void checkFirebaseStatus() {
  unsigned long now = millis();

  if (now - lastFirebaseCheck >= firebaseCheckInterval) {
    lastFirebaseCheck = now;

    if (!firebaseConnected) {
      reconnectFirebase();
    } else {
      checkFirebaseConnection();
    }
  }

  if (now - lastStatusUpdate >= statusUpdateInterval) {
    lastStatusUpdate = now;
    updateDeviceStatus();
  }

  if (firebaseConnected && (!petNamesLoaded || now - lastPetNamesUpdate > PET_NAMES_UPDATE_INTERVAL)) {
    loadPetNamesFromFirebase();
  }
}

byte getRawDistance(byte trigPin, byte echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH);
  byte distance = duration * 0.034 / 2;

  return distance;
}

bool isValidDistance(byte dist, byte minDist, byte maxDist) {
  return (dist >= minDist && dist <= maxDist);
}

byte distanceToLevel(byte dist, byte maxDist, byte minDist) {
  if (dist >= maxDist) return 0;
  if (dist <= minDist) return 100;
  return map(dist, maxDist, minDist, 0, 100);
}

byte weightToLevel(float weight) {
  if (weight <= TRAY_EMPTY_WEIGHT) return 0;
  if (weight >= TRAY_FULL_WEIGHT) return 100;
  return map(weight * 100, TRAY_EMPTY_WEIGHT * 100, TRAY_FULL_WEIGHT * 100, 0, 100);
}

byte getTrayLevel() {
  if (millis() - lastTrayReadTime < SENSOR_CACHE_DURATION) {
    return cachedTrayLevel;
  }

  if (LoadCell.update()) {
    float currentWeight = LoadCell.getData();
    
    if (currentWeight >= (TRAY_EMPTY_WEIGHT - 20) && currentWeight <= (TRAY_FULL_WEIGHT + 20)) {
      weightReadings[readingIndex] = currentWeight;
      readingIndex = (readingIndex + 1) % 2; 

      bool allValid = true;
      for (byte i = 0; i < 2; i++) { 
        if (weightReadings[i] == 0.0) {
          allValid = false;
          break;
        }
      }

      if (allValid) {
        float avgWeight = (weightReadings[0] + weightReadings[1]) / 2.0;  
        
        if (avgWeight <= TRAY_EMPTY_WEIGHT) {
          currentTrayLevel = 0; 
        } else if (avgWeight >= TRAY_FULL_WEIGHT) {
          currentTrayLevel = 100; 
        } else {
          currentTrayLevel = map(avgWeight * 100, TRAY_EMPTY_WEIGHT * 100, TRAY_FULL_WEIGHT * 100, 0, 100);
        }
        
        lastValidTrayLevel = currentTrayLevel;
        hasValidReading = true;

        cachedTrayLevel = currentTrayLevel;
        lastTrayReadTime = millis();
        lastRecordedWeight = avgWeight; 

        if (currentTrayLevel <= 3) {
          lastEmptyTime = millis();
          if (firebaseConnected) {
            Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/freshness/last_empty_time", getCurrentDateTime());
          }
          if (!wasEmptyRecently) {
            wasEmptyRecently = true;
            staleAlertActive = false;
          }
        } else {
          if (wasEmptyRecently) {
            wasEmptyRecently = false;
            if (firebaseConnected) {
              Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/freshness/last_food_refill_time", getCurrentDateTime());
            }
          }
        }
        return currentTrayLevel;
      }
    } else {
      for (byte i = 0; i < 2; i++) {
        weightReadings[i] = 0.0;
      }
      readingIndex = 0;
    }
  }
  
  cachedTrayLevel = hasValidReading ? lastValidTrayLevel : 0;
  lastTrayReadTime = millis();
  return cachedTrayLevel;
}

byte getContainerLevel() {
  if (millis() - lastContainerReadTime < SENSOR_CACHE_DURATION) {
    return cachedContainerLevel;
  }

  byte rawDist = getRawDistance(CONTAINER_TRIG_PIN, CONTAINER_ECHO_PIN);

  if (isValidDistance(rawDist, CONTAINER_MIN_DISTANCE, CONTAINER_MAX_DISTANCE)) {
    containerDistanceReadings[containerReadingIndex] = rawDist;
    containerReadingIndex = (containerReadingIndex + 1) % 2;  

    bool allValid = true;
    for (byte i = 0; i < 2; i++) {  
      if (containerDistanceReadings[i] == 0) {
        allValid = false;
        break;
      }
    }

    if (allValid) {
      byte avgDistance = (containerDistanceReadings[0] + containerDistanceReadings[1]) / 2;  // REDUCED: Average 2 readings
      currentContainerLevel = distanceToLevel(avgDistance, CONTAINER_MAX_DISTANCE, CONTAINER_MIN_DISTANCE);
      lastValidContainerLevel = currentContainerLevel;
      hasValidContainerReading = true;
      
      // Update cache
      cachedContainerLevel = currentContainerLevel;
      lastContainerReadTime = millis();
      return currentContainerLevel;
    }
  } else if (rawDist > CONTAINER_MAX_DISTANCE) {
    currentContainerLevel = 0;
    for (byte i = 0; i < 2; i++) {  
      containerDistanceReadings[i] = 0;
    }
    containerReadingIndex = 0;
    lastValidContainerLevel = 0;
    hasValidContainerReading = false;
    
    // Update cache
    cachedContainerLevel = 0;
    lastContainerReadTime = millis();
    return 0;
  }
  cachedContainerLevel = hasValidContainerReading ? lastValidContainerLevel : 0;
  lastContainerReadTime = millis();
  return cachedContainerLevel;
}

byte calculateDispenseAmount(byte currentLevel, byte targetPortion) {
  // If tray already meets or exceeds target, no need to dispense
  if (currentLevel >= targetPortion) {
    return 0;
  }

  // SAFETY: Tray too full (>75%) - cannot dispense minimum 25%
  if (currentLevel > 75) {
    Serial.println("⚠️  Tray too full (>75%) - cannot dispense minimum 25%");
    return 0;
  }

  byte neededAmount = targetPortion - currentLevel;

  // Round to nearest standard dispense amount (25%, 50%, 75%)
  byte dispenseAmount;
  if (neededAmount <= 25) {
    dispenseAmount = 25;
  } else if (neededAmount <= 50) {
    dispenseAmount = 50;
  } else {
    dispenseAmount = 75;  
  }

  if (currentLevel + dispenseAmount > 100) {
    // Try smaller amounts
    if (currentLevel + 50 <= 100) {
      Serial.println("⚠️  Adjusting to 50% to prevent overflow");
      dispenseAmount = 50;
    } else if (currentLevel + 25 <= 100) {
      Serial.println("⚠️  Adjusting to 25% to prevent overflow");
      dispenseAmount = 25;
    } else {
      Serial.println("⚠️  Cannot dispense safely - tray would overflow");
      dispenseAmount = 0;
    }
  }

  return dispenseAmount;
}

void checkStaleFood() {
  if (lastEmptyTime > 0 && !staleAlertActive && (millis() - lastEmptyTime >= STALE_FOOD_THRESHOLD)) {
    staleAlertActive = true;
    if (firebaseConnected) {
      Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/alerts/stale_food_alert", "Active");
    }
  }
}

void updatePortionLevelFromFirebase() {
  if (firebaseConnected) {
    static unsigned long lastPortionCheck = 0;
    static int lastFetchedPortion = portionLevel;
    const unsigned long portionCheckInterval = 5000;

    if (millis() - lastPortionCheck > portionCheckInterval) {
      lastPortionCheck = millis();

      if (Firebase.RTDB.getInt(&fbdo, "/devices/kibbler_001/device_status/portion_level")) {
        int newPortion = fbdo.intData();
        if (newPortion != lastFetchedPortion && newPortion >= 25 && newPortion <= 100) {
          portionLevel = newPortion;
          lastFetchedPortion = newPortion;
          Serial.print("⚙️  Portion level updated: ");
          Serial.print(portionLevel);
          Serial.println("%");
          
          // Blink ALL LEDs 3 times to indicate settings updated
          blinkAllLEDs(3);
        }
      }
    }
  }
}

void dispenseFood(byte amount) {
  unsigned long holdTime;
  switch (amount) {
    case 25: holdTime = 500; break;   // 0.5 seconds
    case 50: holdTime = 850; break;  // 0.85 seconds
    case 75: holdTime = 1300; break;  // 1.3 seconds
    default: return;
  }

  Serial.print("🔄 Activating servo - moving to 0° for ");
  Serial.print(holdTime);
  Serial.println("ms");
  
  foodServo.write(0);
  delay(holdTime);
  
  foodServo.write(90);
  Serial.println("✓ Servo returned to 90°");
}

void checkRFID() {
  static unsigned long lastPollTime = 0;
  static unsigned long lastDebugTime = 0;
  const unsigned long pollInterval = 5000;  

  
  if (millis() - lastPollTime >= pollInterval) {
    while (Serial2.available()) {
      Serial2.read();
    }
    
    rfid.poll(); 
    lastPollTime = millis();
    
    if (millis() - lastDebugTime >= 30000) {
      Serial.println("🔍 [RFID] Polling for tags...");
      lastDebugTime = millis();
    }
  }

  if (memcmp(rfid.uid, blankUid, sizeof rfid.uid) != 0) {
    String uidStr;
    for (byte i = 0; i < 12; i++) {
      uidStr += String(rfid.uid[i] < 0x10 ? "0" : "");
      uidStr += String(rfid.uid[i], HEX);
    }
    uidStr.toUpperCase();

    memset(rfid.uid, 0, sizeof(rfid.uid));

    Serial.println("\n╔═══════════════════════════════════════════════╗");
    Serial.println("║           🏷️  RFID TAG DETECTED               ║");
    Serial.println("╠═══════════════════════════════════════════════╣");
    Serial.print("║ UID: ");
    Serial.print(uidStr);
    Serial.println("            ║");
    Serial.println("╚═══════════════════════════════════════════════╝");

    // CHECK REGISTRATION MODE FIRST
    if (tagRegistrationMode) {
      Serial.println("📝 REGISTRATION MODE: Sending tag to web app...");
      if (firebaseConnected) {
        Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/others/last_detected_tag", uidStr);
        Serial.println("   ✅ Tag sent! Complete registration in web app.");
      }
      blinkLED(YELLOW_LED_PIN, 3);  
      Serial.println("═══════════════════════════════════════════════\n");
      return; 
    }

    // Check if pet is registered FIRST
    String petName = getPetNameForUID(uidStr);
    
    if (petName.isEmpty()) {
      // UNREGISTERED PET - REJECT
      Serial.println("❌ REJECTED: Pet not registered!");
      Serial.println("   Please register this pet in the web app first.");
      Serial.println("   Go to Pets > Register New Tag");
      blinkLED(RED_LED_PIN, 3);  // Blink RED 3x - unregistered
      Serial.println("═══════════════════════════════════════════════\n");
      return;  // Exit early - don't dispense
    }
    
    // Pet is registered - continue with normal checks
    Serial.print("🐾 Pet: ");
    Serial.println(petName);

    // ALWAYS check and display tray level
    byte trayLevel = getTrayLevel();
    Serial.print("📊 Tray Level: ");
    Serial.print(trayLevel);
    Serial.print("% (");
    float currentWeight = (weightReadings[0] + weightReadings[1]) / 2.0;
    Serial.print(currentWeight, 1);
    Serial.println("g)");

    // ALWAYS check feeding interval status
    auto it = lastFedTimes.find(uidStr);
    bool intervalActive = (it != lastFedTimes.end() && (millis() - it->second < feedingIntervalMs));
    
    if (intervalActive) {
      unsigned long remainingTime = feedingIntervalMs - (millis() - it->second);
      Serial.print("⏳ Cooldown: ");
      Serial.print(remainingTime / 60000);
      Serial.print(" min ");
      Serial.print((remainingTime % 60000) / 1000);
      Serial.println(" sec remaining");
    } else {
      Serial.println("✅ Cooldown: Ready to dispense");
    }

    // Calculate dispense amount for status display
    byte dispenseAmount = calculateDispenseAmount(trayLevel, portionLevel);
    Serial.print("🎯 Target Portion: ");
    Serial.print(portionLevel);
    Serial.println("%");

    // DECISION LOGIC - Check all conditions and show status
    Serial.println("\n--- DISPENSE CHECK ---");
    
    if (intervalActive) {
      Serial.println("❌ BLOCKED: Feeding interval active (pet needs to wait)");
      blinkLED(RED_LED_PIN, 3);  // Blink RED 3x - blocked by cooldown
    } else if (trayLevel > 75) {
      Serial.println("❌ BLOCKED: Tray too full (>75%) - overflow risk");
      blinkLED(RED_LED_PIN, 3);  // Blink RED 3x - blocked by tray full
    } else if (dispenseAmount == 0) {
      Serial.println("❌ BLOCKED: Tray level already sufficient");
      blinkLED(RED_LED_PIN, 3);  // Blink RED 3x - blocked by sufficient food
    } else {
      // ALL CHECKS PASSED - DISPENSE!
      Serial.println("✅ APPROVED: All checks passed");
      blinkLED(GREEN_LED_PIN, 3);  // Blink GREEN 3x - dispensing!
      
      Serial.println("\n🍽️  ═══ DISPENSING FOOD ═══");
      Serial.print("   Dispense Amount: ");
      Serial.print(dispenseAmount);
      Serial.println("%");

      // Dispense food
      dispenseFood(dispenseAmount);

      // Update last fed time
      lastFedTimes[uidStr] = millis();
      
      // Record to Firebase
      if (firebaseConnected) {
        Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/pets/last_fed_times/" + uidStr, (int)(millis() / 1000));
        recordFeedingEvent(uidStr, dispenseAmount, trayLevel);
        Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/others/last_pet_fed", petName + " (" + uidStr + ")");
      }
      
      Serial.println("   ✅ Food dispensed successfully!");
    }
    
    Serial.println("═══════════════════════════════════════════════\n");
  }
}

void checkRegistrationMode() {
  static unsigned long lastCheck = 0;
  static unsigned long lastBlink = 0;
  static bool yellowBlinkState = false;
  const unsigned long checkInterval = 1000;
  const unsigned long blinkInterval = 500;  // Blink every 500ms

  // Check Firebase for registration mode status
  if (millis() - lastCheck > checkInterval) {
    lastCheck = millis();

    if (firebaseConnected) {
      if (Firebase.RTDB.getBool(&fbdo, TAG_REGISTRATION_MODE_PATH)) {
        bool newMode = fbdo.boolData();
        if (newMode != tagRegistrationMode) {
          tagRegistrationMode = newMode;
          Serial.print("📝 Tag registration mode: ");
          Serial.println(tagRegistrationMode ? "ON" : "OFF");
          
          if (!tagRegistrationMode) {
            // Mode turned off - restore normal state
            updateLEDs();
          }
        }
      }
    }
  }

  // Handle continuous yellow LED blinking when in registration mode
  if (tagRegistrationMode) {
    if (millis() - lastBlink > blinkInterval) {
      lastBlink = millis();
      yellowBlinkState = !yellowBlinkState;
      
      // Keep red and green off, blink yellow
      digitalWrite(RED_LED_PIN, LOW);
      digitalWrite(GREEN_LED_PIN, LOW);
      digitalWrite(YELLOW_LED_PIN, yellowBlinkState ? HIGH : LOW);
    }
  }
}

void sendTrayLevel() {
  byte level = getTrayLevel();
  checkStaleFood();

  // Write to Firebase ONLY on actual changes
  static byte lastSentLevel = 255;  
  static float lastSentWeight = -999.0; 
  
  float currentWeight = (weightReadings[0] + weightReadings[1]) / 2.0;
  bool significantWeightChange = abs(currentWeight - lastSentWeight) > 5.0;
  
  bool levelChanged = (level != lastSentLevel);
  
  if (firebaseConnected && (levelChanged || significantWeightChange)) {
    Serial.print("📤 [FIREBASE] Updating tray level: ");
    Serial.print(level);
    Serial.print("% (");
    Serial.print(currentWeight, 1);
    Serial.println("g)");
    
    if (!Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/device_status/tray_level", level)) {
      Serial.println("   ❌ [ERROR] Failed: " + fbdo.errorReason());
    } else {
      Serial.println("   ✅ [SUCCESS] Tray level updated");
      lastSentLevel = level;
      lastSentWeight = currentWeight;
    }
  }
}

void checkTareTrigger() {
  if (!firebaseConnected) return;
  
  static unsigned long lastCheck = 0;
  const unsigned long checkInterval = 2000; 
  
  if (millis() - lastCheck > checkInterval) {
    lastCheck = millis();
    
    if (Firebase.RTDB.getBool(&fbdo, TARE_TRIGGER_PATH)) {
      bool tareTrigger = fbdo.boolData();
      
      if (tareTrigger) {
        Serial.println("\n🔧 [TARE] Remote tare triggered from Firebase!");
        Serial.println("   IMPORTANT: Make sure tray is EMPTY!");
        delay(2000);
        
        // Perform tare
        LoadCell.tareNoDelay();
        while (!LoadCell.getTareStatus()) {
          LoadCell.update();
        }
        
        preferences.begin("kibbler", false);
        long newOffset = LoadCell.getTareOffset();
        preferences.putLong("tareOffset", newOffset);
        preferences.end();
        
        Serial.print("   ✓ Tare complete! New offset saved: ");
        Serial.println(newOffset);
        
        // Reset trigger in Firebase
        Firebase.RTDB.setBool(&fbdo, TARE_TRIGGER_PATH, false);
        Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/others/last_tare_time", getCurrentDateTime());
      }
    }
  }
}

void sendContainerLevel() {
  byte containerLevel = getContainerLevel();

  // Write to Firebase ONLY on actual changes
  static byte lastSentContainerLevel = 255;  
  
  bool levelChanged = (containerLevel != lastSentContainerLevel);
  
  if (firebaseConnected && levelChanged) {
    Serial.print("📤 [FIREBASE] Updating container level: ");
    Serial.print(containerLevel);
    Serial.println("%");
    
    if (!Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/device_status/container_level", containerLevel)) {
      Serial.println("   ❌ [ERROR] Failed: " + fbdo.errorReason());
    } else {
      Serial.println("   ✅ [SUCCESS] Container level updated");
      lastSentContainerLevel = containerLevel;
    }

    // Report low container level to Firebase but don't prevent dispensing
    if (containerLevel < 10) {
      Serial.println("   ⚠️ [ALERT] Container level low!");
      Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/alerts/container_alert", "Low Level: " + String(containerLevel) + "%");
    }
  }
}

void updateStatus() {
  updateBatteryLevel();
  resetDailyCounters();
  resetWeeklyCounters();
}

void printSensorReadings() {
  static unsigned long lastPrintTime = 0;
  const unsigned long printInterval = 5000;

  if (millis() - lastPrintTime >= printInterval) {
    lastPrintTime = millis();

    byte trayLevel = cachedTrayLevel;
    byte containerLevel = cachedContainerLevel;
    
    float currentWeight = 0.0;
    if (LoadCell.update()) {
      currentWeight = LoadCell.getData();
    }
    
    byte containerDistance = getRawDistance(CONTAINER_TRIG_PIN, CONTAINER_ECHO_PIN);
    
    Serial.print("📊 Tray: ");
    Serial.print(currentWeight, 1);
    Serial.print("g (");
    Serial.print(trayLevel);
    Serial.print("%) | Container: ");
    Serial.print(containerLevel);
    Serial.print("% (");
    Serial.print(containerDistance);
    Serial.print("cm) | Battery: ");
    Serial.print(batteryPercentage);
    Serial.print("% (");
    Serial.print(batteryVoltage, 2);
    Serial.println("V)");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Initializing Kibbler...");

  esp_task_wdt_deinit();
  
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 30000,     
    .idle_core_mask = 0,     
    .trigger_panic = true    
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);      
  Serial.println("Watchdog timer enabled - 30s timeout");

  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(YELLOW_LED_PIN, OUTPUT);
  updateLEDs();

  foodServo.setPeriodHertz(50);
  foodServo.attach(SERVO_PIN, 500, 2400);
  foodServo.write(90);
  Serial.println("Servo initialized to 90 degrees");

  Serial.println("Initializing HX711 Load Cell...");
  
  Serial.println("🔧 Clearing EEPROM...");
  EEPROM.begin(512); 
  for (int i = 0; i < 512; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
  Serial.println("✓ EEPROM cleared (fresh start)");
  
  LoadCell.begin();
  unsigned long stabilizingtime = 2000;
  boolean _tare = true;
  LoadCell.start(stabilizingtime, _tare);
  
  if (LoadCell.getTareTimeoutFlag()) {
    Serial.println("ERROR: HX711 timeout during initialization!");
    Serial.println("Check HX711 connections (DT=GPIO4, SCK=GPIO2)");
  } else {
    LoadCell.setCalFactor(calibrationFactor);
    Serial.println("HX711 Load Cell initialized successfully!");
    
    preferences.begin("kibbler", false);
    
    if (preferences.isKey("tareOffset")) {
      long savedOffset = preferences.getLong("tareOffset", 0);
      LoadCell.setTareOffset(savedOffset);
      Serial.print("✓ Loaded saved tare offset: ");
      Serial.println(savedOffset);
    } else {
      Serial.println("⚠️  No saved tare offset found - performing initial tare...");
      Serial.println("   IMPORTANT: Make sure tray is EMPTY!");
      delay(2000);
      
      LoadCell.tareNoDelay();
      while (!LoadCell.getTareStatus()) {
        LoadCell.update();
      }
      
      long newOffset = LoadCell.getTareOffset();
      preferences.putLong("tareOffset", newOffset);
      Serial.print("✓ Initial tare complete! Saved offset: ");
      Serial.println(newOffset);
    }
    
    preferences.end();  
  }

  pinMode(CONTAINER_TRIG_PIN, OUTPUT);
  pinMode(CONTAINER_ECHO_PIN, INPUT);
  pinMode(BATTERY_PIN, INPUT); 

  // Initialize RFID module
  rfid.begin(&Serial2, 115200, RFID_RX_PIN, RFID_TX_PIN);
  Serial.println("RFID R200 module initialized");
  Serial.printf("RFID using Serial2 - RX: %d, TX: %d at 115200 baud\n", RFID_RX_PIN, RFID_TX_PIN);
  
  // Set RFID to manual polling mode (prevents continuous beeping)
  rfid.setMultiplePollingMode(false);
  Serial.println("✓ RFID set to manual polling mode (5-second intervals)");
  
  esp_task_wdt_reset();
  esp_task_wdt_reset();

  Serial.print("Connecting to WiFi...");
  WiFi.begin(ssid, pass);
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 60) { 
    delay(500);
    Serial.print(".");
    wifiAttempts++;
    if (wifiAttempts % 10 == 0) {
      esp_task_wdt_reset();  
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
  } else {
    Serial.println("\nWiFi connection failed!");
  }
  
  esp_task_wdt_reset();  

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", "PHT-8", 1);
  tzset();
  Serial.print("Waiting for time sync...");
  int timeAttempts = 0;
  while (time(nullptr) < 1000000000 && timeAttempts < 40) {  // 20 second timeout
    delay(500);
    Serial.print(".");
    timeAttempts++;
    if (timeAttempts % 10 == 0) {
      esp_task_wdt_reset();  
    }
  }
  
  if (time(nullptr) >= 1000000000) {
    Serial.println(" Time synced!");
  } else {
    Serial.println(" Time sync timeout!");
  }
  
  esp_task_wdt_reset();  // Reset watchdog before Firebase

  if (initializeFirebase()) {
    Serial.println("Firebase connected!");

    // Set device status to online
    String path = String(DEVICE_STATUS_PATH) + "/status";
    Firebase.RTDB.setString(&fbdo, path.c_str(), "online");
    if (fbdo.httpCode() == 200) {
      Serial.println("Successfully set device status to online");
    } else {
      Serial.println("Failed to set device status");
    }

    loadPetNamesFromFirebase();
  } else {
    Serial.println("Firebase connection failed");
  }

  currentEnergyDate = getCurrentDate();
  lastEnergyReset = millis();
  dailyEnergyUsedWh = 0.0;

  // Set up timer intervals
  const unsigned long RFID_INTERVAL = 5000;  // 5 seconds - prevents continuous beeping
  const unsigned long TRAY_LEVEL_INTERVAL = 2000;
  const unsigned long CONTAINER_LEVEL_INTERVAL = 3000;
  const unsigned long SLEEP_CHECK_INTERVAL = 5000;
  const unsigned long POWER_DATA_INTERVAL = 10000;
  const unsigned long FIREBASE_CHECK_INTERVAL = 10000;
  const unsigned long STATUS_UPDATE_INTERVAL = 30000;
  const unsigned long CLEANUP_INTERVAL = 3600000;
  const unsigned long PET_NAMES_CHECK_INTERVAL = 60000;
  const unsigned long PORTION_UPDATE_INTERVAL = 5000;
  const unsigned long REGISTRATION_CHECK_INTERVAL = 1000;
  const unsigned long USDS_PRINT_INTERVAL = 1000;

  isInitializing = false;
  isInitialized = true;
  updateLEDs();

  Serial.println("Kibbler initialization complete!");
}

void loop() {
  esp_task_wdt_reset();
  
  static unsigned long lastRFIDLoop = 0;
  if (millis() - lastRFIDLoop >= 50) {
    lastRFIDLoop = millis();
    rfid.loop();
  }
  
  static unsigned long lastRFIDCheck = 0;
  static unsigned long lastTrayLevelSend = 0;
  static unsigned long lastContainerLevelSend = 0;
  static unsigned long lastPowerDataSend = 0;
  static unsigned long lastFirebaseStatusCheck = 0;
  static unsigned long lastStatusUpdate = 0;
  static unsigned long lastCleanup = 0;
  static unsigned long lastPetNamesCheck = 0;
  static unsigned long lastPortionUpdate = 0;
  static unsigned long lastRegistrationCheck = 0;
  static unsigned long lastUSDSPrint = 0;

  unsigned long currentMillis = millis();

  // RFID Check - 5 seconds (prevents continuous beeping)
  if (currentMillis - lastRFIDCheck >= 5000) {
    lastRFIDCheck = currentMillis;
    checkRFID();
  }

  // Tray Level - 500ms (OPTIMIZED: was 2 seconds - 4x faster updates!)
  if (currentMillis - lastTrayLevelSend >= 500) {
    lastTrayLevelSend = currentMillis;
    sendTrayLevel();
  }

  // Container Level - 500ms (OPTIMIZED: was 3 seconds - 6x faster updates!)
  if (currentMillis - lastContainerLevelSend >= 500) {
    lastContainerLevelSend = currentMillis;
    sendContainerLevel();
  }

  // Power Data - 10 seconds
  if (currentMillis - lastPowerDataSend >= 10000) {
    lastPowerDataSend = currentMillis;
    sendPowerDataToFirebase();
  }

  // Firebase Status - 10 seconds
  if (currentMillis - lastFirebaseStatusCheck >= 10000) {
    lastFirebaseStatusCheck = currentMillis;
    checkFirebaseStatus();
  }

  // Status Update - 30 seconds
  if (currentMillis - lastStatusUpdate >= 30000) {
    lastStatusUpdate = currentMillis;
    updateStatus();
  }

  // Cleanup - 1 hour
  if (currentMillis - lastCleanup >= 3600000) {
    lastCleanup = currentMillis;
    cleanOldFedTimes();
  }

  // Pet Names Check - 1 minute
  if (currentMillis - lastPetNamesCheck >= 60000) {
    lastPetNamesCheck = currentMillis;
    checkPetNames();
  }

  // Portion Update - 5 seconds
  if (currentMillis - lastPortionUpdate >= 5000) {
    lastPortionUpdate = currentMillis;
    updatePortionLevelFromFirebase();
  }

  // Registration Check - 100ms (faster for continuous blinking)
  if (currentMillis - lastRegistrationCheck >= 100) {
    lastRegistrationCheck = currentMillis;
    checkRegistrationMode();
  }

  // Sensor Print - 1 second
  if (currentMillis - lastUSDSPrint >= 1000) {
    lastUSDSPrint = currentMillis;
    printSensorReadings();
  }
}
