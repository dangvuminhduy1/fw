/*
 * VGU Thesis
 * Project: ESP32-S3 Salinity Sensing (SIMCOM A7682S 4G + Flash Storage + RS485 Modbus)
*/

#define TINY_GSM_MODEM_SIM7600

#include <ModbusMaster.h>
#include <TinyGsmClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <RTClib.h>

// =======================
// PIN DEFINITIONS
// =======================
#define SP3485_RX_PIN 11
#define SP3485_TX_PIN 12
#define SIMCOM_RX_PIN 4
#define SIMCOM_TX_PIN 5
#define MODEM_BAUD_RATE 115200
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
const char DEVICE_ID[] = "binh_duong";
const char apn[] = "v-internet";
const char gprsUser[] = "";
const char gprsPass[] = "";

const String firebase_host = "esp32-iot-demo-thesis-01.asia-southeast1.firebasedatabase.app";
// ?x-http-method-override=PATCH forces Firebase to merge data flat
const String firebase_url = "https://" + firebase_host + "/data.json?x-http-method-override=PATCH";

#define REG_SALINITY 0x00
#define SLEEP_TIME_US 60 * 1000000ULL  // 1 minute interval
#define MAX_OFFLINE_READINGS 600       // Up to 10 hours of 1-minute offline data

// =======================
// OBJECTS & VARS
// =======================
HardwareSerial SerialModem(1);
HardwareSerial SerialRS485(2);
ModbusMaster node;
TinyGsm modem(SerialModem);
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
// GOOGLE HEADER TIME SYNC
// =======================
void syncTimeViaGoogle() {
  Serial.println("--- Starting Google Time Sync ---");

  // Use TinyGSM's built-in TCP client instead of AT HTTP commands
  TinyGsmClient client(modem);

  if (!client.connect("google.com", 80)) {
    Serial.println("[RTC ERROR] Connection to google.com failed.");
    return;
  }

  // Send a lightweight HTTP HEAD request
  client.print(F("HEAD / HTTP/1.1\r\nHost: google.com\r\nConnection: close\r\n\r\n"));

  unsigned long timeout = millis();
  bool timeFound = false;

  while (client.connected() && millis() - timeout < 10000L) {
    while (client.available()) {
      String line = client.readStringUntil('\n');

      // Look for the Date header, e.g., "Date: Sat, 21 Feb 2026 16:44:45 GMT"
      if (line.startsWith("Date: ")) {
        Serial.print("Google Server Time: ");
        Serial.println(line);

        int d, y, h, m, s;
        char w[4], mon[4], gmt[4];

        // Parse the string into variables
        sscanf(line.c_str(), "Date: %3s, %d %3s %d %d:%d:%d %3s", w, &d, mon, &y, &h, &m, &s, gmt);

        // Convert Month text to Integer
        int month = 1;
        if (strcmp(mon, "Jan") == 0) month = 1;
        else if (strcmp(mon, "Feb") == 0) month = 2;
        else if (strcmp(mon, "Mar") == 0) month = 3;
        else if (strcmp(mon, "Apr") == 0) month = 4;
        else if (strcmp(mon, "May") == 0) month = 5;
        else if (strcmp(mon, "Jun") == 0) month = 6;
        else if (strcmp(mon, "Jul") == 0) month = 7;
        else if (strcmp(mon, "Aug") == 0) month = 8;
        else if (strcmp(mon, "Sep") == 0) month = 9;
        else if (strcmp(mon, "Oct") == 0) month = 10;
        else if (strcmp(mon, "Nov") == 0) month = 11;
        else if (strcmp(mon, "Dec") == 0) month = 12;

        // Convert to Unix and adjust for Vietnam Time (UTC+7)
        DateTime utcTime(y, month, d, h, m, s);
        uint32_t vnUnix = utcTime.unixtime() + 25200;

        rtc.adjust(DateTime(vnUnix));
        Serial.println("[RTC] Successfully Synced via Google!");
        timeFound = true;
        break;
      }
    }
    if (timeFound) break;
  }

  if (!timeFound) {
    Serial.println("[RTC ERROR] Did not find Date header.");
  }

  client.stop();
}

void configureSSL() {
  Serial.println("--- Configuring SSL ---");
  modem.sendAT("+CSSLCFG=\"sslversion\",0,3");
  modem.waitResponse();
  modem.sendAT("+CSSLCFG=\"authmode\",0,0");
  modem.waitResponse();
  modem.sendAT("+CSSLCFG=\"ignorelocaltime\",0,1");
  modem.waitResponse();
  modem.sendAT("+CSSLCFG=\"sni\",0,\"", firebase_host, "\"");
  modem.waitResponse();
}

bool sendToFirebaseNative(String jsonData) {
  Serial.println("--- Starting HTTPS ---");

  modem.sendAT("+HTTPINIT");
  if (modem.waitResponse(5000L) != 1) {
    modem.sendAT("+HTTPTERM");
    delay(200);
    modem.sendAT("+HTTPINIT");
    modem.waitResponse();
  }

  modem.sendAT("+HTTPSSL=1");
  modem.waitResponse();
  modem.sendAT("+HTTPPARA=\"URL\",\"", firebase_url, "\"");
  modem.waitResponse();
  modem.sendAT("+HTTPPARA=\"CONTENT\",\"application/json\"");
  modem.waitResponse();

  modem.sendAT("+HTTPDATA=", String(jsonData.length()), ",10000");
  if (modem.waitResponse("DOWNLOAD") != 1) {
    modem.sendAT("+HTTPTERM");
    return false;
  }
  modem.stream.print(jsonData);
  modem.waitResponse();

  modem.sendAT("+HTTPACTION=1");
  Serial.println("Sending...");

  String response = "";
  int status = 0;
  unsigned long start = millis();
  bool received = false;

  while (millis() - start < 15000) {
    while (modem.stream.available()) {
      response += (char)modem.stream.read();
    }
    if (response.indexOf("+HTTPACTION:") != -1) {
      Serial.println("Resp: " + response);
      int firstComma = response.indexOf(',');
      int secondComma = response.indexOf(',', firstComma + 1);
      status = response.substring(firstComma + 1, secondComma).toInt();
      received = true;
      break;
    }
  }

  modem.sendAT("+HTTPTERM");
  modem.waitResponse();

  if (received && (status >= 200 && status < 300)) {
    Serial.println("Successfully Sent to Firebase!");
    return true;
  } else {
    Serial.printf("Failed. Status: %d\n", status);
    return false;
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
    Serial.println("[FLASH] Memory Full!");
  }
  preferences.end();
}

bool processAndSendQueue() {
  preferences.begin("offline_data", false);
  int count = preferences.getInt("count", 0);
  bool success = false;

  if (count > 0) {
    Serial.printf("[FLASH] Found %d offline packets. Building payload...\n", count);

    String json = "{";
    for (int i = 0; i < count; i++) {
      char key[15];
      sprintf(key, "pkg_%d", i);
      SensorPacket pack;

      if (preferences.getBytes(key, &pack, sizeof(pack)) > 0) {
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

    if (sendToFirebaseNative(json)) {
      Serial.println("HTTP Success. Clearing Flash Queue.");
      preferences.putInt("count", 0);
      success = true;
    } else {
      Serial.println("HTTP Failed. Queue kept for next try.");
      success = false;
    }
  } else {
    Serial.println("[FLASH] Queue empty.");
    success = true;
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
  Serial.println("MCU Name: ESP32_N16R8");
  Serial.println("Modem Name: SIMCOM A7682S (4G)");

  // Hardware Init
  pinMode(DEBUG_LED_PIN, OUTPUT);
  digitalWrite(DEBUG_LED_PIN, LOW);
  pinMode(BAT_ADC_EN_PIN, OUTPUT);
  analogSetAttenuation(ADC_11db);

  // MUST BEGIN SERIAL BEFORE SENDING AT COMMANDS
  SerialModem.begin(MODEM_BAUD_RATE, SERIAL_8N1, SIMCOM_RX_PIN, SIMCOM_TX_PIN);

  // Ensure Modem is asleep initially
  //SerialModem.println("AT+CSCLK=2");  ERROR FIX

  // I2C Init & RTC
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    blinkError(5);
  }

  // =======================
  // RTC TIME RELIABLE CHECK 
  // =======================
  DateTime now = rtc.now();
  bool needs_time_sync = (now.year() < 2024 || now.year() > 2030);
  bool modem_is_awake = false;

  // If time is wrong, wake up SIMCOM and fix it IMMEDIATELY before reading sensors
  if (needs_time_sync) {
    Serial.println("[RTC] Invalid Time Detected! Forcing immediate 4G Sync...");

    // Wake up sequence
    SerialModem.println("AT");
    delay(100);
    SerialModem.println("AT+CPIN?");
    delay(2000);
    modem_is_awake = true;

    // INCREASED TIMEOUT: 4G modems need more time to attach to the network
    if (modem.init() && modem.waitForNetwork(15000L) && modem.gprsConnect(apn, gprsUser, gprsPass)) {
      syncTimeViaGoogle();  // Now using the Google Header Method!
      now = rtc.now();      // Refresh the time variable with the newly downloaded true time
    } else {
      Serial.println("[RTC] Failed to attach to 4G. Using default fallback.");
      current_timestamp = 1700000000;
    }
  }

  // Set timestamp using the updated time
  if (!needs_time_sync || now.year() >= 2024) {
    current_timestamp = now.unixtime() - 25200;  // Timestamp Time correction
  }

  batt_volt = readBatteryVoltage();
  bat_perc = readBatteryPercentage(batt_volt);
  SerialRS485.begin(9600, SERIAL_8N1, SP3485_RX_PIN, SP3485_TX_PIN);
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

  // 3. Only process network upload if 5 minutes passed
  if (minutes_passed >= 5) {

    // If we didn't wake the modem earlier for a time sync, wake it up now
    if (!modem_is_awake) {
      Serial.println("Interval reached. Waking up SIMCOM via AT Command...");
      SerialModem.println("AT");
      delay(100);
      SerialModem.println("AT+CPIN?");
      delay(2000);
      modem_is_awake = true;  //ERROR FIXED
    }

    if (modem.init()) {
      if (modem.waitForNetwork(15000L)) {
        if (modem.gprsConnect(apn, gprsUser, gprsPass)) {
          configureSSL();
          // Try to send all saved readings
          if (processAndSendQueue()) {
            minutes_passed = 0;  // Reset counter on success
          } else {
            Serial.println(">> REASON: Firebase Transfer Failed (3 Blinks)");
            blinkError(3);
            minutes_passed = 0;  // Reset to try again in 5 mins
          }
        } else {
          Serial.println(">> REASON: 4G APN Failed (3 Blinks)");
          blinkError(3);
          minutes_passed = 0;  // Reset to try again in 5 mins
        }
      } else {
        Serial.println(">> REASON: 4G Network Not Found (2 Blinks)");
        blinkError(2);
        minutes_passed = 0;  // Reset to try again in 5 mins
      }
    } else {
      Serial.println(">> REASON: Modem Not Responding (2 Blinks). Skipping network tasks.");
      blinkError(2);
      minutes_passed = 0;
    }
  }

  // Put modem back to deep sleep
  if (modem_is_awake) {  //ERROR FIX
    Serial.println("Putting SIMCOM back to sleep...");
    SerialModem.println("AT+CSCLK=2");
    modem_is_awake = true;
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