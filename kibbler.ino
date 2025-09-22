
#include "secrets.h"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Firebase_ESP_Client.h>
#include <SPI.h>
#include "R200.h"
#include <ESP32Servo.h>
#include <time.h>
#include <map>
#include <vector>

// Wi-Fi Credentials
const char *ssid = SECRET_WIFI_SSID;
const char *pass = SECRET_WIFI_PASS;

// Deep Sleep Configuration
#define BUTTON_PIN 15
#define uS_TO_S_FACTOR 1000000
RTC_DATA_ATTR bool bootFromDeepSleep = false;
bool shouldGoToSleep = false;
unsigned long lastActivityTime = 0;

// LED Pins
#define RED_LED_PIN 12
#define GREEN_LED_PIN 13
#define YELLOW_LED_PIN 14

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
const unsigned long PET_NAMES_UPDATE_INTERVAL = 3600000; // 1 hour
bool tagRegistrationMode = false;

// Message tracking
std::map<String, unsigned long> lastInsufficientWriteTimes;
std::map<String, unsigned long> lastIntervalActiveWriteTimes;
const unsigned long MESSAGE_REPEAT_INTERVAL = 7200000; // 2 hours

// BLYNK VIRTUAL PINS
#define VPIN_STATUS V1
#define VPIN_UID V2
#define VPIN_LEVEL V3
#define VPIN_PORTION V5
#define VPIN_FIREBASE_STATUS V6
#define VPIN_STALE_FOOD_ALERT V7
#define VPIN_CONTAINER_LEVEL V8
#define VPIN_BATTERY_LEVEL V9
#define VPIN_FEEDING_INTERVAL V10

// SOLAR/BATTERY CONFIG
#define BATTERY_CAPACITY_WH 153.6f // 12.8V × 12Ah
#define AVG_POWER_IDLE_W 1.75f
#define AVG_POWER_PEAK_W 4.5f

// Hardware Setup
#define SERVO_PIN 21
#define TRIG_PIN 2
#define ECHO_PIN 4
#define CONTAINER_TRIG_PIN 27
#define CONTAINER_ECHO_PIN 26
#define RFID_RX_PIN 16
#define RFID_TX_PIN 17
#define BATTERY_PIN 35

Servo foodServo;
R200 rfid;
BlynkTimer timer;

// Feeding Interval Control
byte feedingIntervalHours = 4;
unsigned long feedingIntervalMs = feedingIntervalHours * 3600000UL;
std::map<String, unsigned long> lastFedTimes;

// Tray USDS Variables
const byte MIN_DISTANCE = 43;
const byte MAX_DISTANCE = 47;
byte distanceReadings[3] = {0};
byte readingIndex = 0;
byte currentTrayLevel = 0;
byte lastValidTrayLevel = 0;
bool hasValidReading = false;

// Container USDS Variables
const byte CONTAINER_MIN_DISTANCE = 14;
const byte CONTAINER_MAX_DISTANCE = 23;
byte containerDistanceReadings[3] = {0};
byte containerReadingIndex = 0;
byte currentContainerLevel = 0;
byte lastValidContainerLevel = 0;
bool hasValidContainerReading = false;

// Food Monitoring
unsigned long lastEmptyTime = 0;
bool wasEmptyRecently = false;
const unsigned long STALE_FOOD_THRESHOLD = 86400000UL; // 24 hours
bool staleAlertActive = false;

// IR Distance Sensor
const int irPin = 32;                                  // GPIO4 (Analog pin)
const int MIN_PRESENCE_DISTANCE = 1;                   // 1cm
const int MAX_PRESENCE_DISTANCE = 50;                  // 50cm
const unsigned long PRESENCE_CONFIRMATION_TIME = 2000; // 2 seconds

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
byte portionLevel;
const uint8_t blankUid[12] = {0};

void applySleepSettings()
{
    if (Firebase.RTDB.getJSON(&fbdo, SLEEP_SETTINGS_PATH))
    {
        FirebaseJson json = fbdo.jsonObject();
        FirebaseJsonData data;

        bool autoWakeEnabled = false;
        int autoWakeHours = 0;

        if (json.get(data, "auto_wake_enabled") && data.typeNum == FirebaseJson::JSON_BOOL)
        {
            autoWakeEnabled = data.boolValue;
        }

        if (json.get(data, "auto_wake_hours") && data.typeNum == FirebaseJson::JSON_INT)
        {
            autoWakeHours = data.intValue;
        }

        if (autoWakeEnabled && autoWakeHours > 0)
        {
            uint64_t sleepTimeUs = autoWakeHours * 3600 * uS_TO_S_FACTOR;
            esp_sleep_enable_timer_wakeup(sleepTimeUs);
            Serial.printf("Auto-wake set for %d hours\n", autoWakeHours);
        }
        else
        {
            esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
            Serial.println("Auto-wake disabled");
        }
    }
}

void goToSleep()
{
    Serial.println("Preparing for deep sleep...");

    if (firebaseConnected)
    {
        String path1 = String(SLEEP_SETTINGS_PATH) + "/is_asleep";
        Firebase.RTDB.setBool(&fbdo, path1.c_str(), true);

        String path2 = String(SLEEP_SETTINGS_PATH) + "/last_sleep_time";
        Firebase.RTDB.setString(&fbdo, path2.c_str(), getCurrentDateTime());

        String path3 = String(DEVICE_STATUS_PATH) + "/status";
        Firebase.RTDB.setString(&fbdo, path3.c_str(), "offline");
    }

    applySleepSettings();

    digitalWrite(RED_LED_PIN, LOW);
    digitalWrite(GREEN_LED_PIN, LOW);
    digitalWrite(YELLOW_LED_PIN, LOW); // Add this line

    if (foodServo.attached())
        foodServo.detach();
    Blynk.disconnect();
    WiFi.disconnect(true);

    esp_sleep_enable_ext0_wakeup(GPIO_NUM_15, LOW);

    Serial.println("Going to sleep...");
    esp_deep_sleep_start();
}

void checkSleepCommand()
{
    String path = String(SLEEP_SETTINGS_PATH) + "/user_request_sleep";
    if (Firebase.RTDB.getBool(&fbdo, path.c_str()))
    {
        if (fbdo.boolData())
        {
            Serial.println("Sleep command received from Firebase");
            Firebase.RTDB.setBool(&fbdo, path.c_str(), false);
            goToSleep();
        }
    }
}

void prepareForSleep()
{
    goToSleep();
}

void checkSleepConditions()
{
    static bool buttonPressed = false;
    static unsigned long buttonPressTime = 0;

    if (digitalRead(BUTTON_PIN) == LOW)
    {
        if (!buttonPressed)
        {
            buttonPressed = true;
            buttonPressTime = millis();
            Serial.println("Sleep button pressed");
        }
    }
    else
    {
        if (buttonPressed && (millis() - buttonPressTime > 50))
        {
            buttonPressed = false;
            if (millis() - buttonPressTime >= 1000)
            {
                shouldGoToSleep = true;
                Serial.println("Sleep command confirmed");
            }
        }
    }

    if (shouldGoToSleep)
    {
        prepareForSleep();
    }
}

void updateLEDs()
{
    Serial.print("Updating LEDs - tagRegistrationMode: ");
    Serial.print(tagRegistrationMode);
    Serial.print(", isInitializing: ");
    Serial.print(isInitializing);
    Serial.print(", isInitialized: ");
    Serial.println(isInitialized);
    if (tagRegistrationMode)
    {
        // Registration mode - Yellow LED on, others off
        digitalWrite(GREEN_LED_PIN, LOW);
        digitalWrite(RED_LED_PIN, LOW);
        digitalWrite(YELLOW_LED_PIN, HIGH);
    }
    else if (isInitializing)
    {
        // Initializing - Red LED on, others off
        digitalWrite(RED_LED_PIN, HIGH);
        digitalWrite(GREEN_LED_PIN, LOW);
        digitalWrite(YELLOW_LED_PIN, LOW);
    }
    else if (isInitialized)
    {
        // Normal operation - Green LED on, others off
        digitalWrite(RED_LED_PIN, LOW);
        digitalWrite(GREEN_LED_PIN, HIGH);
        digitalWrite(YELLOW_LED_PIN, LOW);
    }
    else
    {
        // All LEDs off
        digitalWrite(RED_LED_PIN, LOW);
        digitalWrite(GREEN_LED_PIN, LOW);
        digitalWrite(YELLOW_LED_PIN, LOW);
    }
}

void loadPetNamesFromFirebase()
{
    if (!firebaseConnected)
    {
        Serial.println("Firebase not connected, cannot load pet names");
        return;
    }

    Serial.println("Loading default pet names from Firebase...");

    if (Firebase.RTDB.getArray(&fbdo, DEFAULT_PET_NAMES_PATH))
    {
        FirebaseJsonArray *arr = fbdo.jsonArrayPtr();
        size_t len = arr->size();

        petNames.clear();
        Serial.print("Found ");
        Serial.print(len);
        Serial.println(" pet names");

        for (size_t i = 0; i < len; i++)
        {
            FirebaseJsonData data;
            if (arr->get(data, i) && data.typeNum == FirebaseJson::JSON_STRING)
            {
                String name = data.stringValue;
                if (name.length() > 0)
                {
                    petNames.push_back(name);
                    Serial.print("Added pet name: ");
                    Serial.println(name);
                }
            }
        }

        if (petNames.size() > 0)
        {
            petNamesLoaded = true;
            lastPetNamesUpdate = millis();
            Serial.println("Successfully loaded default pet names");
        }
        else
        {
            Serial.println("No pet names found in array");
        }
    }
    else
    {
        Serial.print("Failed to load pet names: ");
        Serial.println(fbdo.errorReason());
        Serial.print("HTTP Code: ");
        Serial.println(fbdo.httpCode());
        Serial.print("Payload: ");
        Serial.println(fbdo.payload());
    }
}

String getAvailablePetName(const String &uid)
{
    if (firebaseConnected)
    {
        String path = String(PET_REGISTRY_PATH) + "/" + uid;
        if (Firebase.RTDB.getString(&fbdo, path))
        {
            return fbdo.stringData();
        }
    }

    std::vector<String> usedNames;
    if (firebaseConnected && Firebase.RTDB.getJSON(&fbdo, PET_REGISTRY_PATH))
    {
        FirebaseJson *json = fbdo.jsonObjectPtr();
        size_t count = json->iteratorBegin();

        for (size_t i = 0; i < count; i++)
        {
            FirebaseJsonData data;
            if (json->get(data, i) && data.typeNum == FirebaseJson::JSON_STRING)
            {
                usedNames.push_back(data.stringValue);
            }
        }
        json->iteratorEnd();
    }

    for (const String &name : petNames)
    {
        bool nameAvailable = true;
        for (const String &usedName : usedNames)
        {
            if (name.equalsIgnoreCase(usedName))
            {
                nameAvailable = false;
                break;
            }
        }
        if (nameAvailable)
        {
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

void removeAssignedNameFromDefaults(const String &name)
{
    for (auto it = petNames.begin(); it != petNames.end();)
    {
        if (it->equalsIgnoreCase(name))
        {
            Serial.print("Removing assigned name from defaults: ");
            Serial.println(*it);
            it = petNames.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (firebaseConnected)
    {
        FirebaseJsonArray updatedNames;
        for (const String &n : petNames)
        {
            updatedNames.add(n);
        }

        if (!Firebase.RTDB.setArray(&fbdo, DEFAULT_PET_NAMES_PATH, &updatedNames))
        {
            Serial.println("Failed to update default names in Firebase");
        }
    }
}

String getPetNameForUID(const String &uid)
{
    if (firebaseConnected)
    {
        String path = String(PET_REGISTRY_PATH) + "/" + uid;
        if (Firebase.RTDB.getString(&fbdo, path))
        {
            String name = fbdo.stringData();
            if (!name.isEmpty())
            {
                return name;
            }
        }
    }

    String newName = getAvailablePetName(uid);
    removeAssignedNameFromDefaults(newName);

    if (firebaseConnected)
    {
        String path = String(PET_REGISTRY_PATH) + "/" + uid;
        if (Firebase.RTDB.setString(&fbdo, path, newName))
        {
            Serial.print("Registered ");
            Serial.print(uid);
            Serial.print(" as ");
            Serial.println(newName);
        }
        else
        {
            Serial.print("Failed to register pet: ");
            Serial.println(fbdo.errorReason());
        }
    }

    return newName;
}

String getDateTimeFromMillis(unsigned long millisTime)
{
    time_t now = millisTime / 1000;
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(buffer);
}

String getCurrentDate()
{
    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
    {
        char dateStr[11];
        strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &timeinfo);
        return String(dateStr);
    }
    return "";
}

String getCurrentDateTime()
{
    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
    {
        char timeStr[20];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);
        return String(timeStr);
    }
    return "";
}

String getCurrentTime()
{
    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
    {
        char timeStr[6];
        strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
        return String(timeStr);
    }
    return "";
}

byte getDayOfWeek()
{
    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
    {
        return timeinfo.tm_wday;
    }
    return 0;
}

void updatePowerMetrics(float currentVoltage)
{
    static float lastVoltage = 0.0;
    static float voltageHistory[3] = {0};
    static byte historyIndex = 0;

    voltageHistory[historyIndex] = currentVoltage;
    historyIndex = (historyIndex + 1) % 3;

    if (voltageHistory[2] != 0)
    {
        float trend = (voltageHistory[0] + voltageHistory[1] + voltageHistory[2]) / 3;
        isCharging = (currentVoltage > trend + 0.05);
    }
    lastVoltage = currentVoltage;

    static bool wasFull = false;
    if (batteryPercentage >= 95 && !wasFull)
    {
        chargeCycles++;
        lastFullChargeTime = millis();
        wasFull = true;
    }
    else if (batteryPercentage < 90)
    {
        wasFull = false;
    }

    avgPowerConsumptionW = foodServo.attached() ? AVG_POWER_PEAK_W : AVG_POWER_IDLE_W;
}

float calculateRuntimeHours()
{
    if (isCharging)
        return -1.0;
    float remainingWh = (batteryPercentage / 100.0f) * BATTERY_CAPACITY_WH;
    return remainingWh / avgPowerConsumptionW;
}

void sendPowerDataToFirebase()
{
    if (millis() - lastPowerUpdate > 2000)
    {
        float intervalEnergy = avgPowerConsumptionW * 2.0 / 3600.0;
        dailyEnergyUsedWh += intervalEnergy;

        FirebaseJson powerData;
        powerData.set("timestamp", getCurrentDateTime());
        powerData.set("voltage", batteryVoltage);
        powerData.set("battery_level", batteryPercentage);
        powerData.set("is_charging", isCharging);
        powerData.set("current_power_w", avgPowerConsumptionW);
        powerData.set("daily_energy_wh", dailyEnergyUsedWh);
        powerData.set("runtime_hours", calculateRuntimeHours());
        powerData.set("charge_cycles", chargeCycles);

        if (lastFullChargeTime > 0)
        {
            powerData.set("last_full_charge", getDateTimeFromMillis(lastFullChargeTime));
        }

        String path = "/devices/kibbler_001/power_status";
        if (!Firebase.RTDB.setJSON(&fbdo, path, &powerData))
        {
            Serial.println("Failed to update power data: " + fbdo.errorReason());
            firebaseConnected = false;
        }
        else
        {
            String today = getCurrentDate();
            if (today != currentEnergyDate)
            {
                String historyPath = "/devices/kibbler_001/energy_history/" + currentEnergyDate;
                Firebase.RTDB.setFloat(&fbdo, historyPath, dailyEnergyUsedWh);
                currentEnergyDate = today;
                dailyEnergyUsedWh = 0;
            }
        }

        lastPowerUpdate = millis();
    }
}

void updateBatteryLevel()
{
    float totalVoltage = 0;
    int validSamples = 0;

    for (int i = 0; i < 5; i++)
        analogRead(BATTERY_PIN);

    for (int i = 0; i < BATTERY_SAMPLES; i++)
    {
        int adcValue = analogRead(BATTERY_PIN);
        float adcVoltage = adcValue * (ADC_REF_VOLTAGE / 4095.0);
        batteryVoltage = adcVoltage * DIVIDER_RATIO;

        if (batteryVoltage >= 1.0 && batteryVoltage <= 20.0)
        {
            totalVoltage += batteryVoltage;
            validSamples++;
        }
        delay(10);
    }

    if (validSamples == 0)
    {
        Serial.println("[Battery] Invalid readings - check connection");
        return;
    }

    float avgVoltage = totalVoltage / validSamples;
    batteryVoltage = avgVoltage;

    if (avgVoltage >= 13.9)
        batteryPercentage = 100;
    else if (avgVoltage >= 13.8)
        batteryPercentage = 95;
    else if (avgVoltage >= 13.7)
        batteryPercentage = 90;
    else if (avgVoltage >= 13.6)
        batteryPercentage = 85;
    else if (avgVoltage >= 13.5)
        batteryPercentage = 80;
    else if (avgVoltage >= 13.4)
        batteryPercentage = 75;
    else if (avgVoltage >= 13.2)
        batteryPercentage = 65;
    else if (avgVoltage >= 13.0)
        batteryPercentage = 55;
    else if (avgVoltage >= 12.9)
        batteryPercentage = 45;
    else if (avgVoltage >= 12.8)
        batteryPercentage = 35;
    else if (avgVoltage >= 12.6)
        batteryPercentage = 25;
    else if (avgVoltage >= 12.4)
        batteryPercentage = 15;
    else if (avgVoltage >= 12.2)
        batteryPercentage = 5;
    else
        batteryPercentage = 0;

    Serial.print("[Battery] ");
    Serial.print(avgVoltage, 2);
    Serial.print("V (");
    Serial.print(batteryPercentage);
    Serial.println("%)");

    Blynk.virtualWrite(VPIN_BATTERY_LEVEL, batteryPercentage);
    updatePowerMetrics(avgVoltage);

    if (firebaseConnected)
    {
        if (!Firebase.RTDB.setFloat(&fbdo, "/devices/kibbler_001/battery_voltage", avgVoltage))
        {
            Serial.println("Failed to write voltage: " + fbdo.errorReason());
        }

        if (!Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/battery_level", batteryPercentage))
        {
            Serial.println("Failed to write percentage: " + fbdo.errorReason());
        }

        Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/battery_last_updated", getCurrentDateTime());
    }
}

void resetDailyCounters()
{
    String currentDate = getCurrentDate();

    if (currentDate != lastResetDate && currentDate != "")
    {
        if (firebaseConnected && lastResetDate != "")
        {
            Firebase.RTDB.setFloat(&fbdo,
                                   "/devices/kibbler_001/energy_history/" + lastResetDate,
                                   dailyEnergyUsedWh);
        }

        todayDispenseCount = 0;
        todayUniquePets = 0;
        todayFedUIDs = "";
        dailyEnergyUsedWh = 0.0;
        lastResetDate = currentDate;

        if (firebaseConnected)
        {
            Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/stats/today_dispense_count", todayDispenseCount);
            Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/stats/today_unique_pets", todayUniquePets);
            Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/stats/last_reset_date", lastResetDate);
        }
    }
}

void resetWeeklyCounters()
{
    static String lastResetWeek;
    static bool needsResetSync = false;

    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
    {
        char currentWeek[10];
        strftime(currentWeek, sizeof(currentWeek), "%G-W%V", &timeinfo);

        if (lastResetWeek != currentWeek)
        {
            weekDispenseCount = 0;
            lastResetWeek = currentWeek;
            needsResetSync = true;
        }

        if (needsResetSync && firebaseConnected)
        {
            if (Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/stats/week_dispense_count", weekDispenseCount))
            {
                needsResetSync = false;
            }
        }
    }
}

void updateUIDTracking(const String &uid)
{
    if (todayFedUIDs.indexOf(uid) == -1)
    {
        todayFedUIDs += (todayFedUIDs.length() > 0 ? "," : "") + uid;
        todayUniquePets++;
        if (firebaseConnected)
        {
            Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/stats/today_unique_pets", todayUniquePets);
            Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/stats/today_fed_uids", todayFedUIDs);
        }
    }

    if (allTimeUIDs.indexOf(uid) == -1)
    {
        allTimeUIDs += (allTimeUIDs.length() > 0 ? "," : "") + uid;
        totalUniqueUIDs++;
        if (firebaseConnected)
        {
            Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/stats/total_unique_uids", totalUniqueUIDs);
            Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/stats/all_time_uids", allTimeUIDs);
        }
    }
}

void recordFeedingEvent(const String &uid, byte dispenseAmount, byte trayLevel)
{
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo))
    {
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

    if (uid != "")
    {
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

    if (firebaseConnected)
    {
        String recordPath = "/devices/kibbler_001/feeding_history/" + String(timestampStr);
        String dailyPath = "/devices/kibbler_001/history/daily/" + date + "/feedings/" + String(timestampStr);

        if (!Firebase.RTDB.setJSON(&fbdo, recordPath.c_str(), &feedingRecord))
        {
            Serial.println("Failed to save feeding record: " + fbdo.errorReason());
        }

        if (!Firebase.RTDB.setJSON(&fbdo, dailyPath.c_str(), &feedingRecord))
        {
            Serial.println("Failed to save daily record: " + fbdo.errorReason());
        }

        Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/stats/today_dispense_count", todayDispenseCount);
        Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/stats/week_dispense_count", weekDispenseCount);
        Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/stats/last_fed_time", timestampStr);
        Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/stats/last_fed_pet", petName + " (" + uid + ")");

        String activityMessage = "Fed " + petName + " " + String(dispenseAmount) + "% portion";
        recordRecentActivity(activityMessage);
    }
}

void recordRecentActivity(const String &activityMessage)
{
    if (!firebaseConnected)
        return;

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo))
    {
        Serial.println("Failed to get local time for activity recording");
        return;
    }

    char timestampStr[25];
    strftime(timestampStr, sizeof(timestampStr), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);

    static String lastActivityMessage;
    static String lastActivityTime;
    static unsigned long lastActivityWriteTime = 0;
    const unsigned long activityDebounceInterval = 5000; // 5 seconds debounce

    // Skip if same message and within debounce interval
    if (activityMessage == lastActivityMessage && strncmp(timestampStr, lastActivityTime.c_str(), 16) == 0 && (millis() - lastActivityWriteTime < activityDebounceInterval))
    {
        return;
    }

    String timestampPath = "/devices/kibbler_001/recent_activities/" + String(timestampStr);

    FirebaseJson activityRecord;
    activityRecord.set("message", activityMessage);
    activityRecord.set("timestamp", timestampStr);
    activityRecord.set("pet_name", lastUID.isEmpty() ? "Unknown" : getPetNameForUID(lastUID));
    if (!lastUID.isEmpty())
    {
        activityRecord.set("uid", lastUID);
    }

    if (!Firebase.RTDB.setJSON(&fbdo, timestampPath.c_str(), &activityRecord))
    {
        Serial.println("Failed to log activity: " + fbdo.errorReason());
    }
    else
    {
        Serial.println("Activity logged: " + activityMessage);
        lastActivityMessage = activityMessage;
        lastActivityTime = timestampStr;
        lastActivityWriteTime = millis();
    }

    if (Firebase.RTDB.getJSON(&fbdo, "/devices/kibbler_001/recent_activities"))
    {
        FirebaseJson *json = fbdo.jsonObjectPtr();
        size_t count = json->iteratorBegin();

        if (count > 50)
        {
            std::vector<String> keysToDelete;
            for (size_t i = 0; i < count - 50; i++)
            {
                String key, value;
                int type;
                json->iteratorGet(i, type, key, value);
                keysToDelete.push_back(key);
            }

            for (const String &key : keysToDelete)
            {
                String deletePath = "/devices/kibbler_001/recent_activities/" + key;
                Firebase.RTDB.deleteNode(&fbdo, deletePath);
            }
        }
        json->iteratorEnd();
    }
}

void verifyPetRegistration(const String &uid)
{
    if (!firebaseConnected)
        return;

    String path = String(PET_REGISTRY_PATH) + "/" + uid;
    if (Firebase.RTDB.getString(&fbdo, path))
    {
        String name = fbdo.stringData();
        Serial.print("Verification - UID ");
        Serial.print(uid);
        Serial.print(" is registered as: ");
        Serial.println(name);
    }
    else
    {
        Serial.print("Verification - UID ");
        Serial.print(uid);
        Serial.println(" is NOT registered!");
    }
}

void updateDeviceStatus()
{
    if (!firebaseConnected)
        return;

    FirebaseJson statusObj;
    statusObj.set("status", "online");
    statusObj.set("last_seen", getCurrentDateTime());
    statusObj.set("battery_level", batteryPercentage);
    statusObj.set("container_level", currentContainerLevel);
    statusObj.set("tray_level", currentTrayLevel);
    statusObj.set("wifi_signal", WiFi.RSSI());
    statusObj.set("uptime", millis() / 1000);
    statusObj.set("firebase_connected", firebaseConnected);
    statusObj.set("feeding_interval_hours", (int)feedingIntervalHours);

    if (!Firebase.RTDB.setJSON(&fbdo, DEVICE_STATUS_PATH, &statusObj))
    {
        Serial.println("Failed to update device status: " + fbdo.errorReason());
    }
}

void checkPetNames()
{
    static unsigned long lastCheck = 0;
    const unsigned long checkInterval = 3600000;

    if (millis() - lastCheck > checkInterval)
    {
        lastCheck = millis();

        if (!petNamesLoaded || petNames.size() == 0)
        {
            Serial.println("Pet names not loaded, retrying...");
            loadPetNamesFromFirebase();
        }

        if (firebaseConnected)
        {
            Serial.println("Current pet registry:");
            if (Firebase.RTDB.getJSON(&fbdo, PET_REGISTRY_PATH))
            {
                FirebaseJson *json = fbdo.jsonObjectPtr();
                size_t count = json->iteratorBegin();

                for (size_t i = 0; i < count; i++)
                {
                    String uid, name;
                    int type;
                    json->iteratorGet(i, type, uid, name);
                    Serial.print(uid);
                    Serial.print(": ");
                    Serial.println(name);
                }
                json->iteratorEnd();
            }
            else
            {
                Serial.println("Failed to read pet registry");
            }
        }
    }
}

void loadLastFedTimes()
{
    if (Firebase.RTDB.getJSON(&fbdo, "/devices/kibbler_001/last_fed_times"))
    {
        FirebaseJson &json = fbdo.jsonObject();
        size_t count = json.iteratorBegin();

        for (size_t i = 0; i < count; i++)
        {
            int type;
            String key, value;
            if (json.iteratorGet(i, type, key, value))
            {
                lastFedTimes[key] = value.toInt() * 1000;
            }
        }
        json.iteratorEnd();
    }
}

void cleanOldFedTimes()
{
    unsigned long currentTime = millis();
    std::vector<String> toRemove;

    // Clean lastFedTimes
    for (const auto &pair : lastFedTimes)
    {
        if (currentTime - pair.second > (feedingIntervalMs * 2))
        {
            toRemove.push_back(pair.first);
        }
    }

    for (const String &uid : toRemove)
    {
        lastFedTimes.erase(uid);
    }

    // Clean message tracking maps
    toRemove.clear();
    for (const auto &pair : lastInsufficientWriteTimes)
    {
        if (currentTime - pair.second > (MESSAGE_REPEAT_INTERVAL * 2))
        {
            toRemove.push_back(pair.first);
        }
    }
    for (const String &uid : toRemove)
    {
        lastInsufficientWriteTimes.erase(uid);
    }

    toRemove.clear();
    for (const auto &pair : lastIntervalActiveWriteTimes)
    {
        if (currentTime - pair.second > (MESSAGE_REPEAT_INTERVAL * 2))
        {
            toRemove.push_back(pair.first);
        }
    }
    for (const String &uid : toRemove)
    {
        lastIntervalActiveWriteTimes.erase(uid);
    }
}

bool checkFirebaseConnection()
{
    if (Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/connection_test", "connected"))
    {
        firebaseConnected = true;
        firebaseRetryCount = 0;
        Blynk.virtualWrite(VPIN_FIREBASE_STATUS, "Connected");
        return true;
    }

    firebaseConnected = false;
    firebaseRetryCount++;
    Blynk.virtualWrite(VPIN_FIREBASE_STATUS, "Disconnected");
    return false;
}

void initializePetRegistry()
{
    if (firebaseConnected)
    {
        if (!Firebase.RTDB.getJSON(&fbdo, PET_REGISTRY_PATH))
        {
            FirebaseJson json;
            if (Firebase.RTDB.setJSON(&fbdo, PET_REGISTRY_PATH, &json))
            {
                Serial.println("Created new pet registry");
            }
            else
            {
                Serial.println("Failed to create pet registry: " + fbdo.errorReason());
            }
        }
    }
}

bool initializeFirebase()
{
    config.api_key = FIREBASE_API_KEY;
    config.database_url = FIREBASE_URL;
    config.signer.tokens.legacy_token = SECRET_FIREBASE_LEGACY_TOKEN;

    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
    delay(3000);

    if (checkFirebaseConnection())
    {
        Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/last_connected", getCurrentDateTime());
        initializeStats();
        initializePetRegistry();
        return true;
    }
    return false;
}

void initializeStats()
{
    if (Firebase.RTDB.getString(&fbdo, "/devices/kibbler_001/stats/last_reset_date"))
    {
        lastResetDate = fbdo.stringData();
    }

    if (Firebase.RTDB.getInt(&fbdo, "/devices/kibbler_001/stats/today_dispense_count"))
    {
        todayDispenseCount = fbdo.intData();
    }

    if (Firebase.RTDB.getInt(&fbdo, "/devices/kibbler_001/stats/week_dispense_count"))
    {
        weekDispenseCount = fbdo.intData();
    }

    if (Firebase.RTDB.getInt(&fbdo, "/devices/kibbler_001/stats/today_unique_pets"))
    {
        todayUniquePets = fbdo.intData();
    }

    if (Firebase.RTDB.getInt(&fbdo, "/devices/kibbler_001/stats/total_unique_uids"))
    {
        totalUniqueUIDs = fbdo.intData();
    }

    if (Firebase.RTDB.getString(&fbdo, "/devices/kibbler_001/stats/today_fed_uids"))
    {
        todayFedUIDs = fbdo.stringData();
    }

    if (Firebase.RTDB.getString(&fbdo, "/devices/kibbler_001/stats/all_time_uids"))
    {
        allTimeUIDs = fbdo.stringData();
    }

    if (Firebase.RTDB.getInt(&fbdo, "/devices/kibbler_001/feeding_interval_hours"))
    {
        feedingIntervalHours = fbdo.intData();
        feedingIntervalMs = feedingIntervalHours * 3600000UL;
    }

    if (Firebase.RTDB.getInt(&fbdo, "/devices/kibbler_001/portion_level"))
    {
        int newPortion = fbdo.intData();
        portionLevel = (newPortion >= 25 && newPortion <= 100) ? newPortion : 100;
    }

    loadLastFedTimes();
    loadPetNamesFromFirebase();
}

void reconnectFirebase()
{
    if (firebaseRetryCount < maxRetries)
    {
        if (initializeFirebase())
        {
            return;
        }
    }
    else
    {
        firebaseRetryCount = 0;
    }
}

void checkFirebaseStatus()
{
    unsigned long now = millis();

    if (now - lastFirebaseCheck >= firebaseCheckInterval)
    {
        lastFirebaseCheck = now;

        if (!firebaseConnected)
        {
            reconnectFirebase();
        }
        else
        {
            checkFirebaseConnection();
        }
    }

    if (now - lastStatusUpdate >= statusUpdateInterval)
    {
        lastStatusUpdate = now;
        updateDeviceStatus();
    }

    if (firebaseConnected && (!petNamesLoaded || now - lastPetNamesUpdate > PET_NAMES_UPDATE_INTERVAL))
    {
        loadPetNamesFromFirebase();
    }
}

byte getRawDistance(byte trigPin, byte echoPin)
{
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);
    long duration = pulseIn(echoPin, HIGH);
    byte distance = duration * 0.034 / 2;

    // Print the raw distance measurement
    Serial.print("USDS Raw - Pin ");
    Serial.print(trigPin);
    Serial.print(": ");
    Serial.print(distance);
    Serial.println(" cm");

    return distance;
}

bool isValidDistance(byte dist, byte minDist, byte maxDist)
{
    return (dist >= minDist && dist <= maxDist);
}

byte distanceToLevel(byte dist, byte maxDist, byte minDist)
{
    if (dist >= maxDist)
        return 0;
    if (dist <= minDist)
        return 100;
    return map(dist, maxDist, minDist, 0, 100);
}

byte getTrayLevel()
{
    byte rawDist = getRawDistance(TRIG_PIN, ECHO_PIN);

    if (isValidDistance(rawDist, MIN_DISTANCE, MAX_DISTANCE))
    {
        distanceReadings[readingIndex] = rawDist;
        readingIndex = (readingIndex + 1) % 3;

        bool allValid = true;
        for (byte i = 0; i < 3; i++)
        {
            if (distanceReadings[i] == 0)
            {
                allValid = false;
                break;
            }
        }

        if (allValid)
        {
            byte avgDistance = (distanceReadings[0] + distanceReadings[1] + distanceReadings[2]) / 3;
            currentTrayLevel = distanceToLevel(avgDistance, MAX_DISTANCE, MIN_DISTANCE);
            lastValidTrayLevel = currentTrayLevel;
            hasValidReading = true;

            if (currentTrayLevel <= 3)
            {
                lastEmptyTime = millis();
                if (firebaseConnected)
                {
                    Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/last_empty_time", getCurrentDateTime());
                }
                if (!wasEmptyRecently)
                {
                    wasEmptyRecently = true;
                    staleAlertActive = false;
                    Blynk.virtualWrite(VPIN_STALE_FOOD_ALERT, "Clear");
                }
            }
            else
            {
                if (wasEmptyRecently)
                {
                    wasEmptyRecently = false;
                    if (firebaseConnected)
                    {
                        Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/last_food_refill_time", getCurrentDateTime());
                    }
                }
            }
            Serial.print("Tray Level Updated: ");
            Serial.print(currentTrayLevel);
            Serial.println("%");
            return currentTrayLevel;
        }
    }
    else if (rawDist > MAX_DISTANCE)
    {
        // Immediately return 0% if latest reading exceeds MAX_DISTANCE
        currentTrayLevel = 0;
        // Optionally clear readings to require new valid data
        for (byte i = 0; i < 3; i++)
        {
            distanceReadings[i] = 0;
        }
        readingIndex = 0;
        lastValidTrayLevel = 0;
        hasValidReading = false;
        Serial.println("Tray Level: 0% (latest reading exceeds MAX_DISTANCE)");
        return 0;
    }
    // Handle other invalid readings (e.g., < MIN_DISTANCE or no valid readings yet)
    Serial.print("Tray Level: Returning last valid level: ");
    Serial.print(hasValidReading ? lastValidTrayLevel : 0);
    Serial.println("%");
    return hasValidReading ? lastValidTrayLevel : 0;
}

byte getContainerLevel()
{
    byte rawDist = getRawDistance(CONTAINER_TRIG_PIN, CONTAINER_ECHO_PIN);

    if (isValidDistance(rawDist, CONTAINER_MIN_DISTANCE, CONTAINER_MAX_DISTANCE))
    {
        containerDistanceReadings[containerReadingIndex] = rawDist;
        containerReadingIndex = (containerReadingIndex + 1) % 3;

        bool allValid = true;
        for (byte i = 0; i < 3; i++)
        {
            if (containerDistanceReadings[i] == 0)
            {
                allValid = false;
                break;
            }
        }

        if (allValid)
        {
            byte avgDistance = (containerDistanceReadings[0] + containerDistanceReadings[1] + containerDistanceReadings[2]) / 3;
            currentContainerLevel = distanceToLevel(avgDistance, CONTAINER_MAX_DISTANCE, CONTAINER_MIN_DISTANCE);
            lastValidContainerLevel = currentContainerLevel;
            hasValidContainerReading = true;
            Serial.print("Container Level Updated: ");
            Serial.print(currentContainerLevel);
            Serial.println("%");
            return currentContainerLevel;
        }
    }
    else if (rawDist > CONTAINER_MAX_DISTANCE)
    {
        // Immediately return 0% if latest reading exceeds CONTAINER_MAX_DISTANCE
        currentContainerLevel = 0;
        for (byte i = 0; i < 3; i++)
        {
            containerDistanceReadings[i] = 0;
        }
        containerReadingIndex = 0;
        lastValidContainerLevel = 0;
        hasValidContainerReading = false;
        Serial.println("Container Level: 0% (latest reading exceeds CONTAINER_MAX_DISTANCE)");
        return 0;
    }
    Serial.print("Container Level: Returning last valid level: ");
    Serial.print(hasValidContainerReading ? lastValidContainerLevel : 0);
    Serial.println("%");
    return hasValidContainerReading ? lastValidContainerLevel : 0;
}

byte calculateDispenseAmount(byte currentLevel, byte targetPortion)
{
    if (currentLevel >= targetPortion)
    {
        return 0;
    }

    byte neededAmount = targetPortion - currentLevel;

    if (currentContainerLevel < 10)
    {
        return 0;
    }

    byte dispenseAmount = neededAmount;
    if (dispenseAmount > 75)
    {
        dispenseAmount = 75;
    }

    if (dispenseAmount <= 25)
    {
        dispenseAmount = 25;
    }
    else if (dispenseAmount <= 50)
    {
        dispenseAmount = 50;
    }
    else
    {
        dispenseAmount = 75;
    }

    return dispenseAmount;
}

void checkStaleFood()
{
    if (lastEmptyTime > 0 && !staleAlertActive && (millis() - lastEmptyTime >= STALE_FOOD_THRESHOLD))
    {
        staleAlertActive = true;
        Blynk.virtualWrite(VPIN_STALE_FOOD_ALERT, "STALE FOOD DETECTED! Please empty tray.");
        if (firebaseConnected)
        {
            Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/stale_food_alert", "Active");
        }
    }
}

BLYNK_WRITE(VPIN_FEEDING_INTERVAL)
{
    feedingIntervalHours = param.asInt();
    feedingIntervalMs = feedingIntervalHours * 3600000UL;
    Serial.print("Feeding interval set to: ");
    Serial.print(feedingIntervalHours);
    Serial.println(" hours");
    if (firebaseConnected)
    {
        Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/feeding_interval_hours", (int)feedingIntervalHours);
    }
    Blynk.virtualWrite(VPIN_STATUS, "Feeding interval set to " + String(feedingIntervalHours) + " hours");
}

void updatePortionLevelFromFirebase()
{
    if (firebaseConnected)
    {
        static unsigned long lastPortionCheck = 0;
        static int lastFetchedPortion = portionLevel;
        const unsigned long portionCheckInterval = 5000;

        if (millis() - lastPortionCheck > portionCheckInterval)
        {
            lastPortionCheck = millis();

            if (Firebase.RTDB.getInt(&fbdo, "/devices/kibbler_001/portion_level"))
            {
                int newPortion = fbdo.intData();
                if (newPortion != lastFetchedPortion && newPortion >= 25 && newPortion <= 100)
                {
                    portionLevel = newPortion;
                    lastFetchedPortion = newPortion;
                    Serial.print("Portion level updated: ");
                    Serial.print(portionLevel);
                    Serial.println("%");
                }
            }
        }
    }
}

void dispenseFood(byte amount)
{
    if (getContainerLevel() < 10)
    {
        String msg = "ERROR: Container almost empty (" + String(currentContainerLevel) + "%)";
        Serial.println(msg);
        Blynk.virtualWrite(VPIN_STATUS, msg);
        return;
    }

    unsigned long holdTime;
    switch (amount)
    {
    case 25:
        holdTime = 2000;
        break;
    case 50:
        holdTime = 3500;
        break;
    case 75:
        holdTime = 5000;
        break;
    case 100:
        holdTime = 6000;
        break;
    default:
        return;
    }

    Serial.print("Activating servo for ");
    Serial.print(holdTime);
    Serial.println("ms");
    foodServo.write(90);
    delay(holdTime);
    foodServo.write(0);

    lastActivityTime = millis();
}

void checkRFID()
{
    static unsigned long lastPollTime = 0;
    static unsigned long presenceStartTime = 0;
    static bool waitingForConfirmation = false;
    static String detectedUID = "";
    const unsigned long pollInterval = 500;
    const unsigned long confirmationTime = 2000; // 2 seconds

    rfid.loop();

    // Periodically send poll command
    if (millis() - lastPollTime >= pollInterval)
    {
        while (Serial2.available())
        {
            Serial2.read();
        }
        rfid.poll();
        lastPollTime = millis();
    }

    unsigned long currentTime = millis();

    // === PHASE 1: Detect Tag and Start Timer ===
    if (!waitingForConfirmation && memcmp(rfid.uid, blankUid, sizeof rfid.uid) != 0)
    {
        // Convert UID to string
        String uidStr;
        for (byte i = 0; i < 12; i++)
        {
            uidStr += String(rfid.uid[i] < 0x10 ? "0" : "");
            uidStr += String(rfid.uid[i], HEX);
        }
        uidStr.toUpperCase();

        // Read IR distance
        int sensorValue = analogRead(irPin);
        float distanceCm = 4800.0 / (sensorValue - 20);

        Serial.println("\n--- RFID Detected ---");
        Serial.print("Tag UID: ");
        Serial.println(uidStr);
        Serial.print("IR Distance: ");
        Serial.print(distanceCm);
        Serial.println(" cm");

        // Valid distance?
        if (distanceCm >= MIN_PRESENCE_DISTANCE && distanceCm <= MAX_PRESENCE_DISTANCE)
        {
            waitingForConfirmation = true;
            presenceStartTime = currentTime;
            detectedUID = uidStr;
            Serial.println("Initial presence detected. Waiting 2s for confirmation without tag...");
            return;
        }
        else
        {
            Serial.println("Initial IR check failed: Out of range.");
        }
    }

    // === PHASE 2: Wait 2 Seconds Without Tag ===
    if (waitingForConfirmation)
    {
        // If tag still detected — cancel
        if (memcmp(rfid.uid, blankUid, sizeof rfid.uid) != 0)
        {
            Serial.println("Tag still detected during 2s wait. Cancelling confirmation.");
            waitingForConfirmation = false;
            presenceStartTime = 0;
            detectedUID = "";
            return;
        }

        // Wait for 2 seconds
        if (currentTime - presenceStartTime >= confirmationTime)
        {
            // Recheck IR
            int sensorValue = analogRead(irPin);
            float distanceCm = 4800.0 / (sensorValue - 20);

            Serial.println("\n--- 2s Confirmation Check ---");
            Serial.print("Final IR Distance: ");
            Serial.print(distanceCm);
            Serial.println(" cm");

            if (distanceCm >= MIN_PRESENCE_DISTANCE && distanceCm <= MAX_PRESENCE_DISTANCE)
            {
                Serial.println("Presence confirmed after 2s. Proceeding with conditions...");

                // Get tray level and pet info
                byte trayLevel = getTrayLevel();
                String petName = getPetNameForUID(detectedUID);

                Serial.print("Tray Level: ");
                Serial.print(trayLevel);
                Serial.println("%");
                Serial.print("Pet Name: ");
                Serial.println(petName);

                // Feeding interval check
                auto it = lastFedTimes.find(detectedUID);
                if (it != lastFedTimes.end() && (currentTime - it->second < feedingIntervalMs))
                {
                    unsigned long remainingTime = feedingIntervalMs - (currentTime - it->second);
                    Serial.print("Feeding interval active. Remaining: ");
                    Serial.print(remainingTime / 1000);
                    Serial.println(" seconds");

                    String msg = "Detected: " + petName + ". Feeding Interval still active (" + String(remainingTime / 3600000UL) + "h, " + String((remainingTime % 3600000UL) / 60000UL) + "m).";
                    Blynk.virtualWrite(VPIN_STATUS, msg);
                }
                else
                {
                    byte containerLevel = getContainerLevel();
                    if (containerLevel < 10)
                    {
                        String msg = "ERROR: Container almost empty (" + String(containerLevel) + "%)";
                        Serial.println(msg);
                        Blynk.virtualWrite(VPIN_STATUS, msg);
                    }
                    else
                    {
                        byte dispenseAmount = calculateDispenseAmount(trayLevel, portionLevel);
                        if (dispenseAmount > 0)
                        {
                            Serial.print("Dispensing ");
                            Serial.print(dispenseAmount);
                            Serial.println("% portion");

                            Blynk.virtualWrite(VPIN_STATUS, "Pet Detected: Dispensing " + String(dispenseAmount) + "%...");
                            dispenseFood(dispenseAmount);

                            lastFedTimes[detectedUID] = currentTime;
                            if (firebaseConnected)
                            {
                                Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/last_fed_times/" + detectedUID, (int)(currentTime / 1000));
                                recordFeedingEvent(detectedUID, dispenseAmount, trayLevel);
                                Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/last_pet_fed", petName + " (" + detectedUID + ")");
                            }
                        }
                        else
                        {
                            String statusMsg = petName + ": Tray level sufficient (" + String(trayLevel) + "%)";
                            Serial.println(statusMsg);
                            Blynk.virtualWrite(VPIN_STATUS, statusMsg);
                        }
                    }
                }
            }
            else
            {
                Serial.println("Final IR check failed. Presence not confirmed.");
            }

            // Reset state
            waitingForConfirmation = false;
            presenceStartTime = 0;
            detectedUID = "";
        }
    }
}

void checkRegistrationMode()
{
    static unsigned long lastCheck = 0;
    const unsigned long checkInterval = 1000;

    if (millis() - lastCheck > checkInterval)
    {
        lastCheck = millis();

        if (firebaseConnected)
        {
            if (Firebase.RTDB.getBool(&fbdo, TAG_REGISTRATION_MODE_PATH))
            {
                bool newMode = fbdo.boolData();
                if (newMode != tagRegistrationMode)
                {
                    tagRegistrationMode = newMode;
                    Serial.print("Tag registration mode: ");
                    Serial.println(tagRegistrationMode ? "ON" : "OFF");
                    updateLEDs();

                    // Visual feedback
                    if (tagRegistrationMode)
                    {
                        digitalWrite(GREEN_LED_PIN, HIGH);
                        digitalWrite(RED_LED_PIN, HIGH); // Both LEDs on = registration mode
                    }
                    else
                    {
                        digitalWrite(GREEN_LED_PIN, isInitialized ? HIGH : LOW);
                        digitalWrite(RED_LED_PIN, isInitializing ? HIGH : LOW);
                    }
                }
            }
        }
    }
}

void sendTrayLevel()
{
    byte level = getTrayLevel();
    Blynk.virtualWrite(VPIN_LEVEL, level);
    checkStaleFood();

    static unsigned long lastFirebaseUpdate = 0;
    if (firebaseConnected && (millis() - lastFirebaseUpdate > 5000))
    {
        if (!Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/tray_level", level))
        {
            Serial.println("Failed to update tray level: " + fbdo.errorReason());
        }
        lastFirebaseUpdate = millis();
    }
}

void sendContainerLevel()
{
    byte containerLevel = getContainerLevel();
    Blynk.virtualWrite(VPIN_CONTAINER_LEVEL, containerLevel);

    static unsigned long lastContainerFirebaseUpdate = 0;
    if (firebaseConnected && (millis() - lastContainerFirebaseUpdate > 5000))
    {
        if (!Firebase.RTDB.setInt(&fbdo, "/devices/kibbler_001/container_level", containerLevel))
        {
            Serial.println("Failed to update container level: " + fbdo.errorReason());
        }
        lastContainerFirebaseUpdate = millis();

        if (containerLevel < 20)
        {
            Firebase.RTDB.setString(&fbdo, "/devices/kibbler_001/container_alert", "Low Level: " + String(containerLevel) + "%");
        }
    }
}

void updateStatus()
{
    updateBatteryLevel();
    resetDailyCounters();
    resetWeeklyCounters();
}

void printUSDSReadings()
{
    static unsigned long lastPrintTime = 0;
    const unsigned long printInterval = 1000; // 1 second

    if (millis() - lastPrintTime >= printInterval)
    {
        lastPrintTime = millis();

        // Print tray USDS reading
        byte trayDist = getRawDistance(TRIG_PIN, ECHO_PIN);
        Serial.print("Tray USDS: ");
        Serial.print(trayDist);
        Serial.print(" cm (");
        Serial.print(isValidDistance(trayDist, MIN_DISTANCE, MAX_DISTANCE) ? "Valid" : "Invalid");
        Serial.println(")");

        // Print container USDS reading
        byte containerDist = getRawDistance(CONTAINER_TRIG_PIN, CONTAINER_ECHO_PIN);
        Serial.print("Container USDS: ");
        Serial.print(containerDist);
        Serial.print(" cm (");
        Serial.print(isValidDistance(containerDist, CONTAINER_MIN_DISTANCE, CONTAINER_MAX_DISTANCE) ? "Valid" : "Invalid");
        Serial.println(")");

        // Add separator for readability
        Serial.println("---------------------");
    }
}

void setup()
{
    Serial.begin(115200);
    Serial.println("Initializing Kibbler...");

    pinMode(RED_LED_PIN, OUTPUT);
    pinMode(GREEN_LED_PIN, OUTPUT);
    pinMode(YELLOW_LED_PIN, OUTPUT);
    updateLEDs();

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0)
    {
        bootFromDeepSleep = true;
        Serial.println("Woke from deep sleep");
    }

    foodServo.setPeriodHertz(50);
    foodServo.attach(SERVO_PIN, 500, 2400);
    foodServo.write(0);

    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);
    pinMode(CONTAINER_TRIG_PIN, OUTPUT);
    pinMode(CONTAINER_ECHO_PIN, INPUT);
    pinMode(BATTERY_PIN, INPUT_PULLDOWN);
    pinMode(irPin, INPUT);

    rfid.begin(&Serial2, 115200, RFID_RX_PIN, RFID_TX_PIN);

    Serial.print("Connecting to WiFi...");
    WiFi.begin(ssid, pass);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected!");

    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", "PHT-8", 1);
    tzset();
    Serial.print("Waiting for time sync...");
    while (time(nullptr) < 1000000000)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println(" Time synced!");

    Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);
    Serial.println("Blynk connected!");

    if (initializeFirebase())
    {
        Serial.println("Firebase connected!");

        if (!bootFromDeepSleep)
        {
            Serial.println("Initializing Firebase sleep settings...");

            String path1 = String(SLEEP_SETTINGS_PATH) + "/is_asleep";
            Firebase.RTDB.setBool(&fbdo, path1.c_str(), false);
            if (fbdo.httpCode() == 200)
            {
                Serial.println("Successfully set is_asleep");
            }
            else
            {
                Serial.println("Failed to set is_asleep");
            }

            String path2 = String(SLEEP_SETTINGS_PATH) + "/auto_wake_enabled";
            Firebase.RTDB.setBool(&fbdo, path2.c_str(), true);
            if (fbdo.httpCode() == 200)
            {
                Serial.println("Successfully set auto_wake_enabled");
            }
            else
            {
                Serial.println("Failed to set auto_wake_enabled");
            }

            String path3 = String(SLEEP_SETTINGS_PATH) + "/auto_wake_hours";
            Firebase.RTDB.setInt(&fbdo, path3.c_str(), 4);
            if (fbdo.httpCode() == 200)
            {
                Serial.println("Successfully set auto_wake_hours");
            }
            else
            {
                Serial.println("Failed to set auto_wake_hours");
            }

            String path4 = String(DEVICE_STATUS_PATH) + "/status";
            Firebase.RTDB.setString(&fbdo, path4.c_str(), "online");
            if (fbdo.httpCode() == 200)
            {
                Serial.println("Successfully set status");
            }
            else
            {
                Serial.println("Failed to set status");
            }
        }
        else
        {
            String path = String(DEVICE_STATUS_PATH) + "/status";
            Firebase.RTDB.setString(&fbdo, path.c_str(), "online");
        }

        loadPetNamesFromFirebase();
    }
    else
    {
        Serial.println("Firebase connection failed");
    }

    currentEnergyDate = getCurrentDate();
    lastEnergyReset = millis();
    dailyEnergyUsedWh = 0.0;

    Blynk.virtualWrite(VPIN_STALE_FOOD_ALERT, "Clear");
    Blynk.virtualWrite(VPIN_CONTAINER_LEVEL, 0);
    Blynk.virtualWrite(VPIN_BATTERY_LEVEL, batteryPercentage);
    Blynk.virtualWrite(VPIN_FEEDING_INTERVAL, (int)feedingIntervalHours);

    timer.setInterval(500L, checkRFID);
    timer.setInterval(2000L, sendTrayLevel);
    timer.setInterval(3000L, sendContainerLevel);
    timer.setInterval(5000L, checkSleepCommand);
    timer.setInterval(10000L, sendPowerDataToFirebase);
    timer.setInterval(10000L, checkFirebaseStatus);
    timer.setInterval(30000L, updateStatus);
    timer.setInterval(3600000L, cleanOldFedTimes);
    timer.setInterval(60000L, checkPetNames);
    timer.setInterval(5000L, updatePortionLevelFromFirebase);
    timer.setInterval(1000L, checkSleepConditions);
    timer.setInterval(1000L, checkRegistrationMode);
    timer.setInterval(1000L, printUSDSReadings); // Added this line for USDS readings

    isInitializing = false;
    isInitialized = true;
    updateLEDs();

    Serial.println("Kibbler initialization complete!");
}

void loop()
{
    Blynk.run();
    timer.run();
}