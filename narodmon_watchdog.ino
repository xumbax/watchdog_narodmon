/*
 * ============================================================
 *  NARODMON WATCHDOG — Wemos D1 Mini (ESP8266)
 *  Watches one or two sensors on narodmon.ru.
 *  If the data is older than STALE_MINUTES, power-cycles the
 *  target device via a relay.
 *
 *  Libraries (Library Manager):
 *    - ArduinoJson   by Benoit Blanchon (v6)
 *    - NTPClient     by Fabrice Weinberg
 *    - ESP8266WiFi / ESP8266HTTPClient / MD5Builder — bundled
 *      with the ESP8266 board package
 *
 *  Boards Manager URL (File -> Preferences -> Additional URLs):
 *    http://arduino.esp8266.com/stable/package_esp8266com_index.json
 *
 *  Wiring:
 *
 *    RELAY (NC — normally closed):
 *      D1 Mini D1 (GPIO5) -> IN  on the relay module
 *      D1 Mini 3V3        -> VCC on the relay module
 *      D1 Mini GND        -> GND on the relay module
 *      Relay COM          -> +5V from the power adapter
 *      Relay NC           -> +5V (red wire) of the target device's power cable
 *      Adapter GND        -> GND of the target device's power cable (direct, bypassing the relay)
 *
 *    LEDs (each through a 220-330 ohm resistor to GND):
 *      LED1 (WiFi)    : D5 (GPIO14) -> anode -> resistor -> GND
 *      LED2 (API OK)  : D6 (GPIO12) -> anode -> resistor -> GND
 *      LED3 (Fresh)   : D7 (GPIO13) -> anode -> resistor -> GND
 *
 *    LED meaning:
 *      LED1 on        — WiFi connected
 *      LED2 on        — narodmon responded and at least one sensor was found
 *      LED3 on        — data is fresher than 10 minutes (all good)
 *      LED3 off       — data is 10-30 minutes old (warning)
 *      All blink 3x   — a relay reset just happened
 *      LED1 blinking  — currently connecting to WiFi
 *
 *  HOW TO GET THE API DATA YOU NEED:
 *    1. narodmon.ru -> Profile -> My applications -> New application -> api_key
 *    2. UUID: any string (e.g. "my-watchdog"), the firmware MD5-hashes it for you
 *    3. Sensor IDs: open the sensor page on narodmon -> the number after "S"
 *       (e.g. S12345 -> ID = 12345)
 *    4. Both sensors must be public (uncheck "Private" in their settings)
 *    5. "OR" logic: if at least one of the two sensors is fresh, everything
 *       is considered fine. The relay only resets if BOTH sensors are stale
 *       at the same time.
 * ============================================================
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUDP.h>
#include <MD5Builder.h>

// ============================================================
//  SETTINGS — CHANGE THESE FOR YOUR SETUP
// ============================================================

const char* WIFI_SSID      = "YOUR_WIFI";
const char* WIFI_PASS      = "YOUR_PASSWORD";

const char* NM_UUID_SOURCE = "my-watchdog-app";  // any string -> MD5
const char* NM_API_KEY     = "YOUR_API_KEY";     // narodmon.ru -> Profile -> My applications
const int   NM_SENSOR_ID   = 12345;              // primary sensor ID (e.g. outdoor temperature)
const int   NM_SENSOR_ID2  = 67890;              // secondary sensor ID (e.g. pressure)

const int   STALE_MINUTES  = 30;  // reboot if data is older than N minutes
const int   FRESH_MINUTES  = 10;  // LED3 is on if data is fresher than N minutes

const int   RELAY_PIN      = 5;   // D1 = GPIO5
const int   RELAY_OFF_MS   = 8000;

const int   LED_WIFI       = 14;  // D5 = GPIO14 — WiFi
const int   LED_API        = 12;  // D6 = GPIO12 — API / sensor found
const int   LED_FRESH      = 13;  // D7 = GPIO13 — data fresher than FRESH_MINUTES

const int   CHECK_INTERVAL = 5 * 60 * 1000;  // check every 5 minutes
const int   MAX_RESETS     = 3;
const long  LOCKOUT_MS     = 60L * 60 * 1000; // 1 hour
const int   TZ_OFFSET_SEC  = 0 * 3600;        // your UTC offset in hours, only used for log display

// ============================================================
//  Globals
// ============================================================
WiFiUDP       ntpUDP;
NTPClient     timeClient(ntpUDP, "pool.ntp.org", 0, 60000);  // offset=0 — pure UTC, matching narodmon's "time" field

// Format a unix timestamp as a readable local-time string using TZ_OFFSET_SEC (log display only)
String formatLocalTime(long utcEpoch) {
  long local = utcEpoch + TZ_OFFSET_SEC;
  long h = (local % 86400) / 3600;
  long m = (local % 3600) / 60;
  long s = local % 60;
  char buf[12];
  snprintf(buf, sizeof(buf), "%02ld:%02ld:%02ld", h, m, s);
  return String(buf);
}

String        nmUUID;
int           resetCount   = 0;
unsigned long lockoutUntil = 0;
unsigned long lastCheck    = 0;

// ============================================================
//  LEDs
// ============================================================
void ledsOff() {
  digitalWrite(LED_WIFI,  LOW);
  digitalWrite(LED_API,   LOW);
  digitalWrite(LED_FRESH, LOW);
}

// Blink all three LEDs N times — reset signal
void blinkAll(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_WIFI,  HIGH);
    digitalWrite(LED_API,   HIGH);
    digitalWrite(LED_FRESH, HIGH);
    delay(200);
    ledsOff();
    delay(200);
  }
}

// Update LED state based on the current situation
void updateLeds(bool wifiOk, bool apiOk, bool fresh) {
  digitalWrite(LED_WIFI,  wifiOk ? HIGH : LOW);
  digitalWrite(LED_API,   apiOk  ? HIGH : LOW);
  digitalWrite(LED_FRESH, fresh  ? HIGH : LOW);
}

// Blink LED1 while connecting to WiFi
void wifiConnectingBlink() {
  static unsigned long lastBlink = 0;
  static bool state = false;
  if (millis() - lastBlink > 300) {
    state = !state;
    digitalWrite(LED_WIFI, state ? HIGH : LOW);
    lastBlink = millis();
  }
}

// ============================================================
//  MD5
// ============================================================
String md5String(const String& s) {
  MD5Builder md5;
  md5.begin();
  md5.add(s);
  md5.calculate();
  return md5.toString();
}

// ============================================================
//  WiFi
// ============================================================
void connectWiFi() {
  Serial.println("Connecting to network: \"" + String(WIFI_SSID) + "\"");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  ledsOff();
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    wifiConnectingBlink();
    Serial.print(".");
    delay(300);
  }
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_WIFI, HIGH);
    Serial.println(" OK  IP: " + WiFi.localIP().toString());
  } else {
    ledsOff();
    Serial.println(" FAIL");
    Serial.printf("Could not connect to \"%s\". Status code: %d\n", WIFI_SSID, WiFi.status());
    Serial.println("Networks visible nearby:");
    int n = WiFi.scanNetworks();
    if (n == 0) {
      Serial.println("  (no networks found — check the antenna/placement)");
    } else {
      for (int i = 0; i < n; i++) {
        Serial.printf("  \"%s\"  RSSI=%d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
      }
    }
    Serial.println("Check WIFI_SSID and WIFI_PASS in the code. Rebooting in 5s...");
    delay(5000);
    ESP.restart();
  }
}

// ============================================================
//  Request to narodmon — both sensors in a single call.
//  Returns the FRESHEST (maximum) unix timestamp of the two —
//  "OR" logic: if at least one sensor is fresh, everything is alive.
//  -1 = network/parsing/API error
//  -2 = none of the sensors were found in the response
// ============================================================
long fetchLastSensorTime() {
  if (WiFi.status() != WL_CONNECTED) return -1;

  String body = "{\"cmd\":\"sensorsValues\","
                "\"sensors\":[" + String(NM_SENSOR_ID) + "," + String(NM_SENSOR_ID2) + "],"
                "\"uuid\":\"" + nmUUID + "\","
                "\"api_key\":\"" + NM_API_KEY + "\","
                "\"lang\":\"en\"}";

  // DIAGNOSTICS: show exactly what we're sending
  Serial.printf("Request: sensor_id=%d,%d, uuid=%s\n", NM_SENSOR_ID, NM_SENSOR_ID2, nmUUID.c_str());
  Serial.println("Request body: " + body);

  WiFiClient client;
  HTTPClient http;
  http.begin(client, "http://narodmon.ru/api");
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("HTTP response code: %d (expected 200)\n", code);
    http.end();
    return -1;
  }

  String resp = http.getString();
  http.end();
  Serial.println("API response: " + resp);

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, resp)) {
    Serial.println("JSON parsing error");
    return -1;
  }
  if (doc.containsKey("error")) {
    Serial.println("narodmon error: " + String(doc["error"].as<const char*>()));
    return -1;
  }

  JsonArray sensors = doc["sensors"].as<JsonArray>();
  if (sensors.size() == 0) return -2;

  // Find the freshest (maximum) timestamp among all returned sensors
  long bestTime = -1;
  for (JsonObject s : sensors) {
    int id = s["id"].as<int>();
    long t = s["time"].as<long>();
    Serial.printf("  sensor %d: time=%ld\n", id, t);
    if (t > bestTime) bestTime = t;
  }

  return bestTime;
}

// ============================================================
//  Power-cycle the target device
// ============================================================
void rebootTargetDevice() {
  Serial.printf(">>> RESET: relay OFF for %d sec\n", RELAY_OFF_MS / 1000);
  blinkAll(3);                       // signal: blink all three LEDs 3 times
  digitalWrite(RELAY_PIN, HIGH);     // NC opens — power is cut
  delay(RELAY_OFF_MS);
  digitalWrite(RELAY_PIN, LOW);      // NC closes — power is restored
  Serial.println(">>> Target device power restored");
  resetCount++;
  Serial.printf("Resets in a row: %d / %d\n", resetCount, MAX_RESETS);
}

// ============================================================
//  Main check
// ============================================================
void checkAndDecide() {
  bool wifiOk  = (WiFi.status() == WL_CONNECTED);
  bool apiOk   = false;
  bool fresh   = false;

  // Anti-cycling lockout
  if (resetCount >= MAX_RESETS) {
    if (!lockoutUntil) {
      lockoutUntil = millis() + LOCKOUT_MS;
      Serial.printf("LOCKOUT: %d resets in a row. Pausing for 1 hour.\n", MAX_RESETS);
    }
    if (millis() < lockoutUntil) {
      Serial.printf("Lockout: %ld more minutes\n", (lockoutUntil - millis()) / 60000);
      updateLeds(wifiOk, apiOk, fresh);
      return;
    }
    resetCount = 0; lockoutUntil = 0;
    Serial.println("Lockout lifted.");
  }

  // NTP
  timeClient.update();
  long nowTs = (long)timeClient.getEpochTime();

  // DIAGNOSTICS: show both times in a readable form
  Serial.printf("NTP now:    %ld  (%s local)\n", nowTs, formatLocalTime(nowTs).c_str());

  if (nowTs < 1700000000L) {  // sanity check (after year 2023)
    Serial.println("NTP not synced yet — skipping this check");
    updateLeds(wifiOk, false, false);
    return;
  }

  // Request to narodmon
  long lastTs = fetchLastSensorTime();

  if (lastTs == -1) {
    // No internet or narodmon unreachable — don't touch the target device
    Serial.println("No connection to narodmon — skipping");
    updateLeds(wifiOk, false, false);
    return;
  }

  if (lastTs == -2) {
    Serial.println("No sensor found — check NM_SENSOR_ID and NM_SENSOR_ID2");
    updateLeds(wifiOk, false, false);
    return;
  }

  // DIAGNOSTICS: show the sensor timestamp and the difference before computing age
  Serial.printf("Sensor timestamp: %ld\n", lastTs);
  Serial.printf("Difference (now - lastTs): %ld sec\n", nowTs - lastTs);

  if (lastTs > nowTs) {
    // Sensor timestamp is in the future relative to our NTP time — clearly a
    // sync glitch, NOT a reason to reboot (protects against false positives)
    Serial.println("WARNING: sensor timestamp is later than our NTP time!");
    Serial.println("Looks like NTP hasn't synced correctly yet. Skipping this cycle.");
    updateLeds(wifiOk, true, false);
    return;
  }

  // Sensor found
  apiOk = true;
  long ageMin = (nowTs - lastTs) / 60;
  fresh = (ageMin < FRESH_MINUTES);

  Serial.printf("Data age: %ld min  (fresh<%d: %s, reboot threshold: %d)\n",
    ageMin, FRESH_MINUTES, fresh ? "YES" : "NO", STALE_MINUTES);

  updateLeds(wifiOk, apiOk, fresh);

  if (ageMin >= STALE_MINUTES) {
    Serial.println("Data is stale! Rebooting the target device...");
    rebootTargetDevice();
    // After the reset: API was OK, freshness unknown yet
    updateLeds(wifiOk, true, false);
  } else {
    Serial.println("OK.");
    resetCount = 0;
  }
}

// ============================================================
//  Setup / Loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  // Pins
  pinMode(RELAY_PIN,  OUTPUT); digitalWrite(RELAY_PIN,  LOW); // NC closed — power is on
  pinMode(LED_WIFI,   OUTPUT); digitalWrite(LED_WIFI,   LOW);
  pinMode(LED_API,    OUTPUT); digitalWrite(LED_API,    LOW);
  pinMode(LED_FRESH,  OUTPUT); digitalWrite(LED_FRESH,  LOW);

  Serial.println("\n=== Narodmon Watchdog ===");
  Serial.printf("Reboot at: >%d min  LED3 turns off at: >=%d min\n",
    STALE_MINUTES, FRESH_MINUTES);

  nmUUID = md5String(String(NM_UUID_SOURCE));

  connectWiFi();
  timeClient.begin();

  // Force NTP sync — several attempts with a sanity check
  Serial.print("Syncing NTP");
  bool ntpOk = false;
  for (int i = 0; i < 10; i++) {
    timeClient.forceUpdate();
    long t = (long)timeClient.getEpochTime();
    Serial.printf(" [%ld]", t);
    if (t > 1700000000L) { ntpOk = true; break; }
    delay(1000);
  }
  Serial.println();
  if (!ntpOk) {
    Serial.println("NTP did not sync after 10 attempts! Rebooting...");
    delay(2000);
    ESP.restart();
  }

  Serial.printf("NTP: %s local  (epoch UTC %ld)\n", formatLocalTime((long)timeClient.getEpochTime()).c_str(), (long)timeClient.getEpochTime());
  Serial.printf("Sensor IDs: %d (primary), %d (secondary)\n", NM_SENSOR_ID, NM_SENSOR_ID2);
  Serial.println("First check in 30s...");
  delay(30000);

  checkAndDecide();
  lastCheck = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_WIFI, LOW);
    connectWiFi();
  }

  if (millis() - lastCheck >= (unsigned long)CHECK_INTERVAL) {
    checkAndDecide();
    lastCheck = millis();
  }

  delay(100);
}
