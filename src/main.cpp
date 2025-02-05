#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include "DHT.h"
#include <NTPClient.h>
#include <WiFiUdp.h>

// Deklarasi fungsi sebelum digunakan di loop()
void sendSensorData();
void sendSoilMoistureData();
void checkMode();
void checkWateringSchedule();
bool checkTime(String schedule, int hour, int minute);
void triggerRelay(int relayPin);
void deactivateRelay(int relayPin);
void checkMoistureLevel();

// Konfigurasi WiFi
const char* ssid = "Bluehouse";
const char* password = "Ripple0808?x";

// Konfigurasi DHT22
#define DHTPIN D1  // Pin D1 (GPIO5)
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// Konfigurasi Relay
#define RELAY_PIN D7  // Pin D3 (GPIO0)

// Konfigurasi Sensor Kelembapan Tanah
#define SOIL_SENSOR_PIN A0

// API Endpoints (tanpa trailing slash)
const char* dataEndpoint = "https://smartio-api.infonering.com/dht/data";
const char* modeEndpoint = "https://smartio-api.infonering.com/device/mode/XE23214";
const char* soilEndpoint = "https://smartio-api.infonering.com/soil/data";
const char* deviceID = "XE23214";

// NTP Client
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 25200); // GMT+7

// Timing Variables
unsigned long lastDataSend = 0;
unsigned long lastModeCheck = 0;
const long dataInterval = 10000; // 10 detik
const long modeInterval = 5000;  // 5 detik

// Relay Control
unsigned long relayStartTime = 0;
int activeRelay = -1; // -1 = tidak aktif

// Mode Settings
String pumpMode = "";
String firstTime = "07:00";
String secondTime = "16:30";

void setup() {
  Serial.begin(115200);
  
  // Inisialisasi Sensor dan Relay
  dht.begin();
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // Matikan relay saat awal

  pinMode(SOIL_SENSOR_PIN, INPUT);
  
  // Koneksi WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print(".");
  }
  Serial.println("\nWiFi Terhubung!");
  
  // Sinkronisasi Waktu
  timeClient.begin();
  timeClient.update();
}

void loop() {
  unsigned long currentMillis = millis();
  timeClient.update();

  // Kirim Data Sensor
  if (currentMillis - lastDataSend >= dataInterval) {
    lastDataSend = currentMillis;
    sendSensorData();
    sendSoilMoistureData();
  }

  // Cek Mode
  if (currentMillis - lastModeCheck >= modeInterval) {
    lastModeCheck = currentMillis;
    checkMode();
  }

  // Kontrol Relay
  if (activeRelay != -1 && currentMillis - relayStartTime >= 60000) { // 60 detik
    deactivateRelay(activeRelay);
  }

  // Penjadwalan Timebased
  if (pumpMode == "timebased") {
    checkWateringSchedule();
  }

  // Penjadwalan Moisturebased
  if (pumpMode == "threshold") {
    checkMoistureLevel();
  }
}

void sendSensorData() {
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();
  
  if (isnan(temp) || isnan(hum)) {
    Serial.println("Gagal baca sensor!");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure(); // Nonaktifkan verifikasi sertifikat SSL
  
  HTTPClient http;
  http.begin(client, dataEndpoint);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  StaticJsonDocument<201> doc;
  doc["device"] = deviceID;
  doc["temperature"] = temp;
  doc["humidity"] = hum;
  
  String payload;
  serializeJson(doc, payload);
  
  int httpCode = http.POST(payload);
  
  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
    Serial.println("Data berhasil dikirim!");
  } else {
    Serial.printf("Gagal kirim data. HTTP status code: %d\n", httpCode);
  }
  http.end();
}

void sendSoilMoistureData() {
  int moisture = analogRead(SOIL_SENSOR_PIN);

  // Pemetaan nilai kelembapan: 0 (basah) -> 100% dan 1023 (kering) -> 0%
  int moisturePercentage = map(moisture, 0, 1023, 100, 0);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, soilEndpoint);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  StaticJsonDocument<200> doc;
  doc["device"] = deviceID;
  doc["moisture"] = moisturePercentage; // Kirim nilai persentase kelembapan

  String payload;
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);

  if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
    Serial.println("Data kelembapan tanah berhasil dikirim!");
  } else {
    Serial.printf("Gagal kirim data kelembapan tanah. HTTP status code: %d\n", httpCode);
  }
  http.end();
}

void checkMode() {
  WiFiClientSecure client;
  client.setInsecure(); // Nonaktifkan verifikasi sertifikat SSL
  
  HTTPClient http;
  http.begin(client, modeEndpoint);
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, http.getString());
    
    pumpMode = doc["data"]["pumpMode"].as<String>();
    firstTime = doc["data"]["firstTime"].as<String>();
    secondTime = doc["data"]["secondTime"].as<String>();
    
    Serial.println("Mode: " + pumpMode);
  } else {
    Serial.printf("Gagal cek mode. HTTP status code: %d\n", httpCode);
  }
  http.end();
}

void checkWateringSchedule() {
  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();
  
  if (checkTime(firstTime, currentHour, currentMinute)) {
    triggerRelay(RELAY_PIN);
  }
  
  if (checkTime(secondTime, currentHour, currentMinute)) {
    triggerRelay(RELAY_PIN);
  }
}

bool checkTime(String schedule, int hour, int minute) {
  int sHour = schedule.substring(0,2).toInt();
  int sMinute = schedule.substring(3,5).toInt();
  return (hour == sHour && minute == sMinute);
}

void triggerRelay(int relayPin) {
  if (activeRelay == -1) {
    digitalWrite(relayPin, LOW); // Aktifkan relay
    activeRelay = relayPin;
    relayStartTime = millis();
    Serial.println("Pompa diaktifkan pada pin " + String(relayPin));
  }
}

void deactivateRelay(int relayPin) {
  digitalWrite(relayPin, HIGH); // Matikan relay
  activeRelay = -1;
  Serial.println("Pompa dimatikan pada pin " + String(relayPin));
}

void checkMoistureLevel() {
  int moisture = analogRead(SOIL_SENSOR_PIN);
  int percentageMoisture = map(moisture, 0, 1023, 100, 0);
  Serial.println("Kelembapan Tanah: " + String(percentageMoisture));
  int limitMoisture = 45;

  if (percentageMoisture < limitMoisture && activeRelay == -1) {
    triggerRelay(RELAY_PIN);
  } else if (percentageMoisture >= limitMoisture && activeRelay != -1) {
    deactivateRelay(RELAY_PIN);
  }
}