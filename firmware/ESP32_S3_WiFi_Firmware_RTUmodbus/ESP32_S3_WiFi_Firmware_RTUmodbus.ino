/*
 * VGU Thesis
 * Project: ESP32-S3 Salinity Sensing (WiFi + Flash Storage + RS485 Modbus)
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <RTClib.h>
#include <ModbusMaster.h>

// =======================
// PIN DEFINITIONS
// =======================
#define SP3485_RX_PIN 11
#define SP3485_TX_PIN 12
#define BAT_ADC_PIN 10
#define BAT_ADC_EN_PIN 9
#define DEBUG_LED_PIN 13
#define PH_POT_PIN 8

// I2C Pins
#define I2C_SDA_PIN 1
#define I2C_SCL_PIN 2

// =======================
// CONFIGURATION
// =======================
const char DEVICE_ID[] = "HoChiMinh_city";

// WiFi & Firebase
const char* ssid = "<SSID>";
const char* password = "<PASSWORD>";
const String firebase_url = "https://esp32-iot-demo-thesis-01.asia-southeast1.firebasedatabase.app/data.json";

#define REG_SALINITY 0x00
#define SLEEP_TIME_US 60 * 1000000ULL  // 1 minute interval
#define MAX_OFFLINE_READINGS 600       // Up to 10 hours of 1-minute offline data

// =======================
// OBJECTS & VARS
// =======================
HardwareSerial SerialRS485(2);
ModbusMaster node;
Preferences preferences;
RTC_DS3231 rtc;

float salinity = 0.0, ph = 0.0, ambient_temperature = 0.0, batt_volt = 0.0;
int bat_perc = 0;
uint32_t current_timestamp = 0;
const float DIVIDER_RATIO = 3.1276;

// RTC variable to track the 5-minute wake-up cycle
RTC_DATA_ATTR int minutes_passed = 0;

struct SensorPacket {
  float sal;
  float ph;
  float amb_temp;
  float v;
  int p;
  uint32_t ts;
};

// =======================
// FUNCTIONS DECLARATION
// =======================

void blinkError(int times) {
  Serial.printf("[LED] Blinking %d times...\n", times);
  for (int i = 0; i < times; i++) {
    digitalWrite(DEBUG_LED_PIN, HIGH);
    delay(300);
    digitalWrite(DEBUG_LED_PIN, LOW);
    delay(300);
  }
}

float readBatteryVoltage() {
  digitalWrite(BAT_ADC_EN_PIN, HIGH);
  delay(50);
  uint32_t mv = 0;
  for (int i = 0; i < 20; i++) {
    mv += analogReadMilliVolts(BAT_ADC_PIN);
    delay(1);
  }
  digitalWrite(BAT_ADC_EN_PIN, LOW);
  return ((mv / 20.0) / 1000.0) * DIVIDER_RATIO;
}

int readBatteryPercentage(float voltage) {
  if (voltage >= 8.4) return 100;
  if (voltage <= 6.0) return 0;
  if (voltage >= 8.0) return map(voltage * 100, 800, 840, 85, 100);
  if (voltage >= 7.6) return map(voltage * 100, 760, 800, 60, 85);
  if (voltage >= 7.2) return map(voltage * 100, 720, 760, 30, 60);
  return map(voltage * 100, 600, 720, 0, 30);
}

float readphFromPot() {
  uint32_t mv = 0;
  for (int i = 0; i < 20; i++) {
    mv += analogReadMilliVolts(PH_POT_PIN);
    delay(1);
  }
  float averageMv = mv / 20.0;
  // Map 0 - 3245mV to 0.00 - 14.00 pH range "tượng trưng"
  float phValue = (averageMv / 3245.0) * 14.0;
  if (phValue < 0) phValue = 0.00;
  if (phValue > 14) phValue = 14.00;
  return phValue;
}

void readSensors() {
  node.begin(1, SerialRS485);
  uint8_t result = node.readHoldingRegisters(REG_SALINITY, 1);
  if (result == node.ku8MBSuccess) {
    salinity = node.getResponseBuffer(0);
  } else {
    salinity = 0.0; // Fail-safe if Modbus disconnected
  }
  ph = readphFromPot();
}

// =======================
// NTP TIME SYNCING (WiFi)
// =======================
void syncTimeViaNTP() {
  Serial.println("--- Starting NTP Sync ---");
  configTime(0, 0, "time.google.com", "pool.ntp.org");

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10000)) {
    // Offset standard unix timestamp to Vietnam time (UTC+7 -> +25200)
    uint32_t vnUnix = mktime(&timeinfo) + 25200;
    rtc.adjust(DateTime(vnUnix));
    Serial.println("[RTC] Successfully Synced via WiFi NTP!");
  } else {
    Serial.println("[NTP] Sync Failed (Timeout)");
  }
}

// =========================================
// FLASH STORAGE & QUEUE & REALTIME DATABASE
// =========================================
void saveToFlash(uint32_t ts, float s, float p, float amb_temp, float v, int pct) {
  preferences.begin("offline_data", false);
  int count = preferences.getInt("count", 0);

  if (count < MAX_OFFLINE_READINGS) {
    SensorPacket pack = { s, p, amb_temp, v, pct, ts };
    char key[15];
    sprintf(key, "pkg_%d", count);
    preferences.putBytes(key, &pack, sizeof(pack));
    preferences.putInt("count", count + 1);
    Serial.printf("[FLASH] Saved packet #%d\n", count + 1);
  } else {
    Serial.println("[FLASH] Memory Full! Overwriting blocked until synced.");
  }
  preferences.end();
}

bool processAndSendQueue() {
  preferences.begin("offline_data", false);
  int count = preferences.getInt("count", 0);
  bool success = false;

  if (count > 0) {
    Serial.printf("[FLASH] Found %d offline packets. Building payload...\n", count);

    // Build JSON Object (Dictionary) instead of an Array for Firebase PATCH
    String json = "{";
    for (int i = 0; i < count; i++) {
      char key[15];
      sprintf(key, "pkg_%d", i);
      SensorPacket pack;

      if (preferences.getBytes(key, &pack, sizeof(pack)) > 0) {
        // Use the Unix Timestamp as the unique Firebase Key
        json += "\"" + String(pack.ts) + "\": {";
        json += "\"device_id\":\"" + String(DEVICE_ID) + "\",";
        json += "\"queue_id\":" + String(i + 1) + ",";
        json += "\"measuredAtSec\":" + String(pack.ts) + ",";
        json += "\"salinity\":" + String(pack.sal, 1) + ",";
        json += "\"ph\":" + String(pack.ph, 2) + ",";
        json += "\"ambient_temp\":" + String(pack.amb_temp, 1) + ",";
        json += "\"bat_v\":" + String(pack.v, 2) + ",";
        json += "\"bat_p\":" + String(pack.p);
        json += "}";

        if (i < count - 1) json += ",";
      }
    }
    json += "}";

    // Send payload via HTTP PATCH
    HTTPClient http;
    http.begin(firebase_url);
    http.addHeader("Content-Type", "application/json");

    Serial.println("--- Sending Batch to Firebase via PATCH ---");
    int httpResponseCode = http.sendRequest("PATCH", json);

    if (httpResponseCode >= 200 && httpResponseCode < 300) {
      Serial.printf("HTTP Success: %d. Clearing Flash Queue.\n", httpResponseCode);
      preferences.putInt("count", 0);  // Clear queue only on success
      success = true;
    } else {
      Serial.printf("HTTP Failed: %d (%s). Queue kept for next try.\n", httpResponseCode, http.errorToString(httpResponseCode).c_str());
      success = false;
    }
    http.end();
  } else {
    Serial.println("[FLASH] Queue empty.");
    success = true;  // Nothing to send, consider it a successful check
  }

  preferences.end();
  return success;
}

// =======================
// SETUP FUNCTION
// =======================
void setup() {
  uint32_t start_time_ms = millis();
  Serial.begin(115200);
  Serial.println("\n--- SYSTEM START ---");
  Serial.println("MCU Name: ESP32S3_N16R8");
  Serial.println("Connection: WIFI");

  // Hardware Init
  pinMode(DEBUG_LED_PIN, OUTPUT);
  digitalWrite(DEBUG_LED_PIN, LOW);
  pinMode(BAT_ADC_EN_PIN, OUTPUT);
  analogSetAttenuation(ADC_11db);

  // RS485 Init
  SerialRS485.begin(9600, SERIAL_8N1, SP3485_RX_PIN, SP3485_TX_PIN);

  // I2C Init & RTC
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    blinkError(5);
  }

  DateTime now = rtc.now();

  // =====================
  // RTC TIME SANITY CHECK 
  // =====================
  if (now.year() < 2024 || now.year() > 2030) {
    Serial.println("[RTC] Invalid Time Detected (New Module?). Forcing immediate WiFi Sync...");
    WiFi.begin(ssid, password);

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
      delay(500);
      Serial.print(".");
      retries++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      syncTimeViaNTP();
      now = rtc.now();  // Refresh variable after successful sync
    } else {
      Serial.println("[RTC] ERROR: WiFi Failed for NTP Sync. Timestamp will be wrong!");
      blinkError(2);
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  // ==========================================

  current_timestamp = now.unixtime() - 25200;  // Timestamp Time correction

  // Read Sensors & Battery
  batt_volt = readBatteryVoltage();
  bat_perc = readBatteryPercentage(batt_volt);
  readSensors();
  ambient_temperature = rtc.getTemperature();

  Serial.printf("[RTC] Date/Time: %02d/%02d/%d %02d:%02d:%02d\n",
                now.day(), now.month(), now.year(),
                now.hour(), now.minute(), now.second());
  Serial.printf("Timestamp: %d\n", current_timestamp);
  Serial.printf("[DATA] Sal: %.1f | pH: %.2f | Ambient: %.1f | Bat: %.2fV (%d%%) \n",
                salinity, ph, ambient_temperature, batt_volt, bat_perc);

  // 1. Always save the immediate reading to Flash
  saveToFlash(current_timestamp, salinity, ph, ambient_temperature, batt_volt, bat_perc);

  // 2. Increment cycle counter
  minutes_passed++;
  Serial.printf(">>> Time until next upload: %d/5 minutes.\n", minutes_passed);

  // 3. Check if 5 minutes have passed to send the queue
  if (minutes_passed >= 5) {
    Serial.println("Interval reached. Connecting to WiFi...");
    WiFi.begin(ssid, password);

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
      delay(500);
      Serial.print(".");
      retries++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WiFi Connected.");

      // Try to send all saved readings
      if (processAndSendQueue()) {
        minutes_passed = 0;  // Reset counter on success
      } else {
        Serial.println(">> REASON: HTTP Failed (3 Blinks)");
        blinkError(3);
        minutes_passed = 0;  // Reset to try again in 5 mins
      }
    } else {
      Serial.println(">> REASON: WiFi Network Not Found (2 Blinks)");
      blinkError(2);
      minutes_passed = 0;  // Reset to try again in 5 mins
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  Serial.println("Deep Sleep ESP32");
  Serial.flush();

  // ==========================================
  // DYNAMIC SLEEP CALCULATION
  // ==========================================
  uint32_t execution_time_ms = millis() - start_time_ms;
  uint64_t adjusted_sleep_time_us = SLEEP_TIME_US;

  // Ensure we don't calculate a negative sleep time if network tasks took > 60s
  if ((execution_time_ms * 1000ULL) < SLEEP_TIME_US) {
    adjusted_sleep_time_us = SLEEP_TIME_US - (execution_time_ms * 1000ULL);
  } else {
    adjusted_sleep_time_us = 1000ULL;  // Wake up immediately if we overslept
  }

  Serial.printf("Execution time: %d ms | Adjusted sleep: %llu us\n", execution_time_ms, adjusted_sleep_time_us);

  // Use the new adjusted sleep time
  esp_sleep_enable_timer_wakeup(adjusted_sleep_time_us);
  esp_deep_sleep_start();
}

void loop() {
  //Will not reach here because of DEEP SLEEP
}