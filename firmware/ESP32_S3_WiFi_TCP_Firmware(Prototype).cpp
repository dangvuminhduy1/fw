#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_sleep.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <ModbusIP_ESP8266.h>  // emelianov/modbus-esp8266
#include <math.h>  

// ===================== Cloud Function endpoint  =====================
const char* FUNCTION_URL = "https://asia-southeast1-esp32-iot-demo-temphumi.cloudfunctions.net/ingestBatch";

// ===================== Device identity  =====================
const char* DEVICE_ID  = "demo_wifi_1";
const char* DEVICE_KEY = "abc123";

// ===================== WiFi =====================
const char* WIFI_SSID = "DUY";
const char* WIFI_PASS = "123456789";
static const uint32_t WIFI_TIMEOUT_MS = 15000;

// ===================== Sampling / Sleep =====================
static const uint32_t MEASURE_INTERVAL_SEC   = 60;   // 1min updload
static const uint32_t SEND_EVERY_N_SAMPLES   = 3;    // test nhanh: 3min 
static const uint8_t  RECORDS_PER_BATCH      = 10;   // số record mỗi batch
static const uint8_t  MAX_BATCH_PER_WAKE     = 5;    // tối đa batch mỗi lần wake sau khi kết nối lại

// ===================== LittleFS buffer =====================
static const char* BUFFER_FILE = "/buffer.txt";
static const char* META_FILE   = "/meta.json";
static const uint32_t MAX_RECORDS = 200; // giới hạn backlog

// ===================== FS fail handling  =====================
static const uint8_t FS_MOUNT_FAIL_LIMIT = 3;
RTC_DATA_ATTR uint8_t fsFailStreak = 0;
RTC_DATA_ATTR bool    fsDisabled   = false;

// ===================== RTC time anchor  =====================
RTC_DATA_ATTR uint32_t currentEpoch = 0;
RTC_DATA_ATTR bool     timeValid    = false;
RTC_DATA_ATTR uint32_t sampleCount  = 0;

// ===================== RTC clamp state =====================
RTC_DATA_ATTR bool  clampHasPrev = false;
RTC_DATA_ATTR float clampPrevSal = 0.0f;   // ppt 
RTC_DATA_ATTR float clampPrevTmp = 0.0f;   // °C  
RTC_DATA_ATTR float clampPrevPh  = 0.0f;   // pH 

// ===================== RTC battery simulation =====================
RTC_DATA_ATTR float batteryPct = 85.0f;   // 0..100
RTC_DATA_ATTR bool  batteryCharging = false;

// ===================== Buffer meta in RAM =====================
static uint32_t bufHeadLine = 0;
static uint32_t bufCount    = 0;

// ===================== Modbus TCP (pyModSlave) =====================
// Windows hotspot 192.168.137.1 
IPAddress SERVER_IP(192, 168, 137, 1);
const uint16_t SERVER_PORT = 1502;
const uint8_t  UNIT_ID = 1; // Modbus TCP: 0 or 1

ModbusIP mb;
static uint16_t hregBuf[3] = {0, 0, 0}; // 3 registers: sal(0), temp(1), ph(2)
static bool readDone = false;
static bool readOk = false;

// ===================== helpers =====================
static inline float clampf(float x, float lo, float hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

static inline float roundTo(float x, int decimals) { 
  float p = 1.0f;
  for (int i = 0; i < decimals; i++) p *= 10.0f; // Làm tròn đến phần thập phân
  return roundf(x * p) / p;
}

// Convert battery % (0..100) -> voltage for 2S2P pack (7.4V trung bình)
// Demo-friendly: V_MIN ~6.60V (3.30/cell), V_MAX 8.40V (4.20/cell)
float batteryPctToVolt_2S(float pct) {
  pct = clampf(pct, 0.0f, 100.0f);
  const float V_MIN = 6.60f;
  const float V_MAX = 8.40f;

  // Piecewise curve (more realistic than linear)
  // 0-10%: 6.60 -> 7.00
  // 10-90%: 7.00 -> 8.10
  // 90-100%: 8.10 -> 8.40
  if (pct <= 10.0f) {
    return V_MIN + (pct / 10.0f) * (7.00f - V_MIN);
  } else if (pct <= 90.0f) {
    return 7.00f + ((pct - 10.0f) / 80.0f) * (8.10f - 7.00f);
  } else {
    return 8.10f + ((pct - 90.0f) / 10.0f) * (V_MAX - 8.10f);
  }
}

bool connectWiFi(uint8_t retries = 3) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);           // tránh ghi flash config
  WiFi.setSleep(false);             // ổn định hơn khi STA

  for (uint8_t attempt = 1; attempt <= retries; attempt++) {
    // Clean state trước mỗi attempt (tránh kẹt sau khi deep sleep)
    WiFi.disconnect(true, true);    
    WiFi.mode(WIFI_OFF);
    delay(200);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    Serial.printf("Connecting WiFi (attempt %u/%u)", attempt, retries);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
      delay(250);
      Serial.print(".");
      if (millis() - start > WIFI_TIMEOUT_MS) {
        Serial.println("\nWiFi connect timeout");
        break;
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi connected!");
      Serial.print("IP: "); Serial.println(WiFi.localIP());
      Serial.print("GW: "); Serial.println(WiFi.gatewayIP());
      Serial.print("RSSI: "); Serial.println(WiFi.RSSI());

      
      delay(1200) //delay 1s để hostpot ổn định

      return true;
    }

    delay(600);
  }

  return false;
}


void disconnectWiFi() {
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

// ===================== deep sleep =====================
void goToSleep(uint32_t seconds) {
  Serial.printf("Going to sleep for %u sec...\n", seconds);
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}

// ===================== LittleFS meta =====================
bool saveBufferMeta() {
  StaticJsonDocument<128> doc;
  doc["headLine"] = bufHeadLine;
  doc["count"]    = bufCount;

  File f = LittleFS.open(META_FILE, FILE_WRITE);
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

bool loadBufferMeta() {
  if (!LittleFS.exists(META_FILE)) return false;
  File f = LittleFS.open(META_FILE, FILE_READ);
  if (!f) return false;

  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  bufHeadLine = doc["headLine"] | 0;
  bufCount    = doc["count"]    | 0;
  return true;
}

void rebuildMetaFromFile() {
  bufHeadLine = 0;
  bufCount = 0;

  if (!LittleFS.exists(BUFFER_FILE)) {
    saveBufferMeta();
    return;
  }

  File f = LittleFS.open(BUFFER_FILE, FILE_READ);
  if (!f) {
    saveBufferMeta();
    return;
  }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    bufCount++;
  }
  f.close();
  saveBufferMeta();
}

void initMetaFromFileIfNeeded() {
  if (!loadBufferMeta()) {
    Serial.println("Meta missing/bad -> rebuild from buffer file");
    rebuildMetaFromFile();
  } else {
    Serial.printf("Meta loaded: headLine=%u count=%u\n", bufHeadLine, bufCount);
  }
}

// Bỏ phần headLine đã gửi để file nhỏ lại
bool compactBufferNow() {
  if (bufCount == 0) {
    bufHeadLine = 0;
    if (LittleFS.exists(BUFFER_FILE)) LittleFS.remove(BUFFER_FILE);
    return saveBufferMeta();
  }

  if (!LittleFS.exists(BUFFER_FILE)) {
    bufHeadLine = 0;
    return saveBufferMeta();
  }

  File in = LittleFS.open(BUFFER_FILE, FILE_READ);
  if (!in) return false;

  File out = LittleFS.open("/tmp.txt", FILE_WRITE);
  if (!out) { in.close(); return false; }

  // skip headLine lines (đã gửi)
  uint32_t skipped = 0;
  while (in.available() && skipped < bufHeadLine) {
    in.readStringUntil('\n');
    skipped++;
  }

  // copy remaining (bufCount lines)
  uint32_t copied = 0;
  while (in.available() && copied < bufCount) {
    String line = in.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    out.println(line);
    copied++;
  }

  in.close();
  out.close();

  LittleFS.remove(BUFFER_FILE);
  LittleFS.rename("/tmp.txt", BUFFER_FILE);

  // sau compact: mọi thứ còn lại là "chưa gửi"
  bufHeadLine = 0;
  bufCount = copied;
  return saveBufferMeta();
}


// ===================== NTP sync =====================
bool syncNtpOnce() {
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");

  uint32_t start = millis();
  time_t now;

  while (true) {
    time(&now);
    if ((uint32_t)now > 1700000000UL) break;
    if (millis() - start > 8000) {
      Serial.println("NTP sync timeout");
      return false;
    }
    delay(200);
  }

  currentEpoch = (uint32_t)now;
  timeValid = true;

  Serial.printf("NTP synced. currentEpoch=%u\n", currentEpoch);
  return true;
}

// ===================== Battery simulation =====================
// Mô phỏng NLMT đơn giản:
// => Nếu có timeValid: ban ngày (07:00-17:00) thì sạc, ban đêm thì xả
// => Nếu chưa có time: cứ xả dần cho tới min, rồi sạc lên max (dao động như thật)
float updateBatteryAndGet(uint32_t measuredAtEpoch) {
  const float MIN_PCT = 20.0f;
  const float MAX_PCT = 98.0f;

  // tốc độ thay đổi mỗi phút (tuỳ chỉnh cho “thật”) 2S2P nên sẽ tụt chậm hơn và tăng nhanh hơn
  const float DISCHARGE_PER_MIN = 0.15f; // tụt 0.15%/phút
  const float CHARGE_PER_MIN    = 0.40f; // tăng 0.40%/phút

  bool shouldCharge = false;

  if (timeValid && measuredAtEpoch > 0) {
    time_t t = (time_t)measuredAtEpoch;
    struct tm tmLocal;
    localtime_r(&t, &tmLocal);
    int h = tmLocal.tm_hour;
    shouldCharge = (h >= 7 && h < 17);
  } else {
    if (!batteryCharging && batteryPct <= MIN_PCT) batteryCharging = true;
    if (batteryCharging && batteryPct >= MAX_PCT) batteryCharging = false;
    shouldCharge = batteryCharging;
  }

  batteryPct += shouldCharge ? CHARGE_PER_MIN : -DISCHARGE_PER_MIN;
  batteryPct = clampf(batteryPct, MIN_PCT, MAX_PCT);
  batteryCharging = shouldCharge;
  return batteryPct;
}

// ===================== Save record (sal/temp/pH + rawSal/rawTemp/rawpH/battery) =====================
void saveRecord(
  float sal, float temp, float ph,
  float rawSal, float rawTemp, float rawPh,
  float battPct, float battVolt,
  uint32_t measuredAtEpoch
) {
  StaticJsonDocument<384> doc;
  doc["sal"] = sal; //Correct Salinity
  doc["temp"] = temp; //Correct Tempurature
  doc["ph"] = ph; //Conrrect pH

  doc["rawSal"] = rawSal; 
  doc["rawTemp"] = rawTemp;
  doc["rawPh"] = rawPh;

  doc["batteryPct"] = battPct;
  doc["batteryVolt"] = battVolt;
  doc["measuredAt"] = measuredAtEpoch;

  File f = LittleFS.open(BUFFER_FILE, FILE_APPEND);
  if (!f) {
    Serial.println("ERROR: Failed to open buffer file for append");
    return;
  }
  serializeJson(doc, f);
  f.println();
  f.close();

  bufCount++;
  if (bufCount > MAX_RECORDS) {
    uint32_t overflow = bufCount - MAX_RECORDS;
    bufHeadLine += overflow;
    bufCount = MAX_RECORDS;
    Serial.printf("Backlog overflow -> drop %u oldest (headLine=%u, count=%u)\n", overflow, bufHeadLine, bufCount);
  }

  saveBufferMeta();
  Serial.println("Record saved to LittleFS");
}

// ===================== Build batch payload =====================
bool buildBatchPayload(String &outPayload, int maxItems, int &itemsBuilt) {
  itemsBuilt = 0;
  outPayload = "";

  if (bufCount == 0) return false;
  if (!LittleFS.exists(BUFFER_FILE)) return false;

  File f = LittleFS.open(BUFFER_FILE, FILE_READ);
  if (!f) return false;

  uint32_t skipped = 0;
  while (f.available() && skipped < bufHeadLine) {
    f.readStringUntil('\n');
    skipped++;
  }

  DynamicJsonDocument doc(16384);
  doc["deviceId"] = DEVICE_ID;
  doc["key"] = DEVICE_KEY;
  JsonArray arr = doc.createNestedArray("records");

  while (f.available() && itemsBuilt < maxItems && (uint32_t)itemsBuilt < bufCount) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    StaticJsonDocument<384> row;
    DeserializationError err = deserializeJson(row, line);
    if (err) {
      Serial.println("WARN: bad JSON line in buffer, skip");
      continue;
    }

    JsonObject o = arr.createNestedObject();
    o["sal"] = row["sal"].as<float>();
    o["temp"] = row["temp"].as<float>();
    o["ph"] = row["ph"].as<float>();

    o["rawSal"] = row["rawSal"].as<float>();
    o["rawTemp"] = row["rawTemp"].as<float>();
    o["rawPh"] = row["rawPh"].as<float>();

    float pct  = row["batteryPct"].as<float>();
    float volt = row["batteryVolt"].as<float>();

    o["battPct"]  = pct;
    o["battVolt"] = volt;
    o["measuredAt"] = row["measuredAt"].as<uint32_t>();

    itemsBuilt++;
  }

  f.close();
  if (itemsBuilt == 0) return false;

  serializeJson(doc, outPayload);
  return true;
}

void markSentRecords(int n) {
  if (n <= 0) return;
  if ((uint32_t)n > bufCount) n = (int)bufCount;

  bufHeadLine += (uint32_t)n;
  bufCount -= (uint32_t)n;

  saveBufferMeta();

  // Compact liền để không resend batch cũ
  if (!compactBufferNow()) {
    Serial.println("WARN: compactBufferNow failed (may resend if meta lost)");
  }
}

/*
// ===================== Send single record in FS_DISABLED mode =====================
bool sendSingleRecordNow(float sal, float temp, float rawSal, float rawTemp, uint32_t measuredAtEpoch) {
  if (!connectWiFi()) {
    Serial.println("FS_DISABLED: No WiFi -> skip realtime send");
    disconnectWiFi();
    return false;
  }

  DynamicJsonDocument doc(512);
  doc["deviceId"] = DEVICE_ID;
  doc["key"] = DEVICE_KEY;
  JsonArray arr = doc.createNestedArray("records");
  JsonObject o = arr.createNestedObject();
  o["sal"] = sal;
  o["temp"] = temp;
  o["rawSal"] = rawSal;
  o["rawTemp"] = rawTemp;
  o["measuredAt"] = measuredAtEpoch;

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  http.begin(FUNCTION_URL);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(payload);
  String resp = http.getString();
  http.end();

  Serial.printf("FS_DISABLED realtime HTTP %d: %s\n", code, resp.c_str());
  disconnectWiFi();

  return (code == 200);
}

void sendRealtimeIfNeeded_FsDisabled(float sal, float temp, float rawSal, float rawTemp, uint32_t measuredAtEpoch) {
  if (sampleCount % SEND_EVERY_N_SAMPLES != 0) return;

  Serial.println("=== FS_DISABLED: send time (single record) ===");
  bool ok = sendSingleRecordNow(sal, temp, rawSal, rawTemp, measuredAtEpoch);
  Serial.println(ok ? "FS_DISABLED: single record sent OK" : "FS_DISABLED: single record send failed");
}*/

// ===================== flush batches =====================
void flushBatchIfNeeded() {
  if (sampleCount % SEND_EVERY_N_SAMPLES != 0) return;

  Serial.println("=== Batch send time ===");

  if (bufCount == 0) {
    Serial.println("No backlog to send.");
    return;
  }

  if (!connectWiFi()) {
    Serial.println("Skip batch send (no WiFi). Keep buffer.");
    disconnectWiFi();
    return;
  }

  for (uint8_t b = 0; b < MAX_BATCH_PER_WAKE; b++) {
    if (bufCount == 0) break;

    String payload;
    int items = 0;
    if (!buildBatchPayload(payload, RECORDS_PER_BATCH, items)) {
      Serial.println("No batch to send.");
      break;
    }

    Serial.printf("Built batch #%u with %d records\n", (unsigned)(b + 1), items);

    HTTPClient http;
    http.begin(FUNCTION_URL);
    http.addHeader("Content-Type", "application/json");

    int code = http.POST(payload);
    String resp = http.getString();
    http.end();

    Serial.printf("HTTP %d: %s\n", code, resp.c_str());

    if (code == 200) {
      markSentRecords(items);
      Serial.printf("Batch OK -> marked %d records as sent (headLine=%u, count=%u)\n", items, bufHeadLine, bufCount);
    } else {
      Serial.println("Batch failed -> stop flushing this wake (keep backlog).");
      break;
    }
  }

  disconnectWiFi();
}

// ===================== Modbus: callback + connect + read =====================
bool onReadDone(Modbus::ResultCode event, uint16_t transId, void*) {
  readOk = (event == Modbus::EX_SUCCESS);
  readDone = true;
  Serial.printf("Modbus cb: event=%d transId=%u\n", (int)event, (unsigned)transId);
  return true;
}

bool ensureModbusConnected(uint8_t tries = 3) {
  for (uint8_t i = 1; i <= tries; i++) {
    Serial.printf("Connecting Modbus TCP (try %u/%u)...\n", i, tries);

    if (mb.isConnected(SERVER_IP)) {
      Serial.println("Modbus already connected");
      return true;
    }

    if (mb.connect(SERVER_IP, SERVER_PORT)) {
      Serial.println("Modbus REALLY connected");
      delay(150); // cho socket ổn định một chút
      return true;
    }

    Serial.println("Modbus connect timeout");
    delay(500);
  }
  return false;
}

bool startRead3() {
  readDone = false;
  readOk = false;
  mb.readHreg(SERVER_IP, 0, hregBuf, 3, onReadDone, UNIT_ID);
  return true;
}

bool waitReadDone(uint32_t timeoutMs) {
  unsigned long t0 = millis();
  while (!readDone && (millis() - t0 < timeoutMs)) {
    mb.task();
    delay(1);
  }
  return readDone;
}

// ===================== Clamp step (Giả lập ngưỡng số liệu) =====================
void clampStepUpdate3(float rawSal, float rawTemp, float rawPh,
                      float &outSal, float &outTemp, float &outPh) {
  const float SAL_MIN_V = 0.0f, SAL_MAX_V = 50.0f;
  const float TEMP_MIN_V = 0.0f, TEMP_MAX_V = 60.0f;
  const float PH_MIN_V = 0.0f, PH_MAX_V = 14.0f;

  const float SAL_MAX_STEP = 0.10f;
  const float TEMP_MAX_STEP = 0.20f;
  const float PH_MAX_STEP = 0.05f;

  rawSal  = clampf(rawSal,  SAL_MIN_V, SAL_MAX_V);
  rawTemp = clampf(rawTemp, TEMP_MIN_V, TEMP_MAX_V);
  rawPh   = clampf(rawPh,   PH_MIN_V,  PH_MAX_V);

  if (!clampHasPrev) {
    clampHasPrev = true;
    clampPrevSal = rawSal;
    clampPrevTmp = rawTemp;
    clampPrevPh  = rawPh;
    outSal = rawSal;
    outTemp = rawTemp;
    outPh = rawPh;
    return;
  }

  float dSal  = clampf(rawSal  - clampPrevSal, -SAL_MAX_STEP, +SAL_MAX_STEP);
  float dTemp = clampf(rawTemp - clampPrevTmp, -TEMP_MAX_STEP, +TEMP_MAX_STEP);
  float dPh   = clampf(rawPh   - clampPrevPh,  -PH_MAX_STEP,  +PH_MAX_STEP);

  outSal  = clampPrevSal + dSal;
  outTemp = clampPrevTmp + dTemp;
  outPh   = clampPrevPh  + dPh;

  clampPrevSal = outSal;
  clampPrevTmp = outTemp;
  clampPrevPh  = outPh;
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);
  delay(500);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("\nWakeup cause: %d\n", (int)cause);

  // Mount LittleFS
  bool fsReady = false;

  if (fsDisabled) {
    Serial.println("FS_DISABLED: skip LittleFS mount");
    fsReady = false;
  } else {
    if (!LittleFS.begin(false)) {
      fsFailStreak++;
      Serial.printf("ERROR: LittleFS mount failed (streak=%u)\n", fsFailStreak);

      if (fsFailStreak >= FS_MOUNT_FAIL_LIMIT) {
        fsDisabled = true;
        Serial.println("FS_DISABLED: LittleFS failed too many times -> entering FS_DISABLED mode");
      }

      fsReady = false;
    } else {
      Serial.println("LittleFS mounted OK");
      fsFailStreak = 0;
      fsReady = true;
    }
  }

  if (fsReady) initMetaFromFileIfNeeded();
  else { bufHeadLine = 0; bufCount = 0; }

  // Cold boot: clear buffer (ấn reset để xóa mọi thứ bắt đầu lại)
  if (cause != ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("Cold boot -> clearing buffer");
    if (fsReady) {
      if (LittleFS.exists(BUFFER_FILE)) LittleFS.remove(BUFFER_FILE);
      if (LittleFS.exists(META_FILE)) LittleFS.remove(META_FILE);
      bufHeadLine = 0;
      bufCount = 0;
      saveBufferMeta();
    }

    sampleCount = 0;
    timeValid = false;
    currentEpoch = 0;

    clampHasPrev = false;
    clampPrevSal = 0;
    clampPrevTmp = 0;
    clampPrevPh  = 7.0f;

    batteryPct = 85.0f;
    batteryCharging = false;
  } else {
    Serial.println("Timer wake -> keep buffer");
  }

  // NTP sync nếu chưa có time chuẩn từ lần đầu 
  if (!timeValid) {
    Serial.println("Time not valid -> wait network stabilize");
    delay(3000);   // 3s để ổn định trước khi sync

    Serial.println("NTP sync once now");

    if (connectWiFi()) {
      if (!syncNtpOnce()) {
        Serial.println("WARN: NTP sync failed (measuredAt will be 0 until success)");
      }
      disconnectWiFi();
    } else {
      Serial.println("WARN: No WiFi for NTP (measuredAt will be 0 until success)");
    }
  }

  // ===================== Read Modbus (giả lập cho sensor thiệt) =====================
  // Kết nối WiFi để đọc Modbus TCP (LAN nội bộ)
   if (!connectWiFi()) {
    Serial.println("WARN: No WiFi for Modbus read -> go sleep");
    goToSleep(MEASURE_INTERVAL_SEC);
    return;
  }

    mb.client();

  if (!ensureModbusConnected()) {
    Serial.println("Modbus not connected -> sleep");
    disconnectWiFi();
    goToSleep(MEASURE_INTERVAL_SEC);
    return;
  }

    delay(200);
    mb.task();

    startRead3();
  if (!waitReadDone(1500) || !readOk) {
    Serial.println("Modbus read failed -> sleep");
    disconnectWiFi();
    goToSleep(MEASURE_INTERVAL_SEC);
    return;
  }

  disconnectWiFi();

  // RAW from registers: reg0=sal*10, reg1=temp*10, reg2=pH*10 , chia nhỏ giá trị ra vì Modbus không tự chọn mục để giả lập
  float rawSal  = hregBuf[0] / 10.0f;
  float rawTemp = hregBuf[1] / 10.0f; 
  float rawPh   = hregBuf[2] / 10.0f;

  float outSal, outTemp, outPh;
  clampStepUpdate3(rawSal, rawTemp, rawPh, outSal, outTemp, outPh);

  // Timestamp cho mẫu
  uint32_t measuredAtEpoch = timeValid ? currentEpoch : 0;
  float battPct = updateBatteryAndGet(measuredAtEpoch);
  float battVolt = batteryPctToVolt_2S(battPct);

  // RAW
  rawSal  = roundTo(rawSal, 1);
  rawTemp = roundTo(rawTemp, 1);
  rawPh   = roundTo(rawPh, 2);

  // OUT
  outSal  = roundTo(outSal, 1);
  outTemp = roundTo(outTemp, 1);
  outPh   = roundTo(outPh, 2);

  // BATTERY
  battPct  = roundTo(battPct, 1);
  battVolt = roundTo(battVolt, 2);

  Serial.printf("RAW : Sal=%.2f ppt | Temp=%.2f C | pH=%.2f\n", rawSal, rawTemp, rawPh);
  Serial.printf("OUT : Sal=%.2f ppt | Temp=%.2f C | pH=%.2f\n", outSal, outTemp, outPh);
  Serial.printf("Batt: %.1f%% | %.2fV (charging=%d)\n", battPct, battVolt, batteryCharging ? 1 : 0);
  Serial.printf("measuredAtEpoch=%u\n", measuredAtEpoch);

  // Save record
  if (fsReady) {
    saveRecord(outSal, outTemp, outPh, rawSal, rawTemp, rawPh, battPct, battVolt, measuredAtEpoch);
  } else {
    Serial.println("FS not ready -> skip saving to LittleFS");
  }

  // time/sampleCount 
  if (timeValid) currentEpoch += MEASURE_INTERVAL_SEC;
  sampleCount++;

  // Send 
  if (fsReady) flushBatchIfNeeded();
  /*} else {
    sendRealtimeIfNeeded_FsDisabled(outSal, outTemp, rawSal, rawTemp, measuredAtEpoch);
  }*/

  // Sleep
  goToSleep(MEASURE_INTERVAL_SEC);
  
}
void loop() {
  // không dùng loop vì deep sleep trong setup()
}
