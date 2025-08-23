#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseESP8266.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>

// WiFi credentials
const char *ssid = "SAMURAI@CREEDS_2.4G";
const char *password = "samurai2@creeds";

// Firebase credentials
// https://esp-gas-ai-default-rtdb.asia-southeast1.firebasedatabase.app
#define FIREBASE_HOST "https://leaf-disease-recognition-53a80-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH "a31455e1b02ef8a8908d0f806df476a906594501"

// ----- CD4051 Control Pins -----
#define S0 12    // D6
#define S1 13    // D7
#define S2 14    // D5
#define Z_PIN A0 // ESP8266 ADC

// ----- DHT22 -----
#define DHT_PIN 4 // D2
#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);

// ----- DS18B20 -----
#define DS18B20_PIN 2 // D4
OneWire oneWire(DS18B20_PIN);
DallasTemperature ds18b20(&oneWire);

// ----- MQ135 Digital Output -----
#define MQ135_DO_PIN 5 // D1

// Sensor value ranges for percentage calculation
#define MQ135_MIN 0
#define MQ135_MAX 1024
#define SOIL_MIN 0
#define SOIL_MAX 1024
#define TEMP_MIN -40.0
#define TEMP_MAX 80.0
#define HUMIDITY_MIN 0
#define HUMIDITY_MAX 100

// Firebase objects
FirebaseData firebaseData;
FirebaseConfig config;
FirebaseAuth auth;

// NTP Client for timestamps - Asia/Colombo is UTC+5:30 (19800 seconds)
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000); // Changed offset to 19800 (5.5 hours)

// Time sync variables
bool timeSync = false;
unsigned long lastTimeSync = 0;
const unsigned long TIME_SYNC_INTERVAL = 3600000; // 1 hour

// Sensor reading structure
struct SensorReadings
{
  float ds18b20_temp;       // DS18B20 temperature (°C)
  float dht_temp;           // DHT22 temperature (°C)
  float dht_hum;            // DHT22 humidity (%)
  int mq135_raw;            // MQ135 analog value
  int mq135_digital;        // MQ135 digital value
  int soil_raw;             // Soil moisture analog value
  int ds18b20_temp_percent; // DS18B20 temp percentage
  int dht_temp_percent;     // DHT22 temp percentage
  int dht_hum_percent;      // DHT22 humidity percentage
  int mq135_percent;        // MQ135 percentage
  int soil_percent;         // Soil moisture percentage
  String timestamp;
  unsigned long uptime;
  String deviceId;
  String ipAddress;
};

// Configuration
const unsigned long READING_INTERVAL = 10000; // 10 seconds
unsigned long lastReading = 0;
int readingCounter = 0;

// Multiplexer channel selection
void selectMuxChannel(byte channel)
{
  digitalWrite(S0, channel & 0x01);
  digitalWrite(S1, (channel >> 1) & 0x01);
  digitalWrite(S2, (channel >> 2) & 0x01);
}

void setup()
{
  Serial.begin(115200);
  delay(2000);
  Serial.println("=== ESP8266 with CD4051 Multiple Sensor Logger ===");

  // Initialize pins
  pinMode(S0, OUTPUT);
  pinMode(S1, OUTPUT);
  pinMode(S2, OUTPUT);
  pinMode(MQ135_DO_PIN, INPUT);

  // Initialize sensors
  dht.begin();
  ds18b20.begin();

  // Connect to WiFi
  connectToWiFi();

  // Initialize NTP with multiple attempts
  timeClient.begin();
  syncTimeWithNTP();

  // Configure Firebase
  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  firebaseData.setBSSLBufferSize(1024, 1024);

  Serial.println("System initialized successfully!");
}

void loop()
{
  // Check WiFi connection
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi disconnected, reconnecting...");
    connectToWiFi();
  }

  // Update NTP time periodically
  if (millis() - lastTimeSync >= TIME_SYNC_INTERVAL || !timeSync)
  {
    syncTimeWithNTP();
  }

  // Force time update
  timeClient.update();

  // Take readings at specified interval
  if (millis() - lastReading >= READING_INTERVAL)
  {
    SensorReadings readings = takeSensorReadings();
    printReadings(readings);
    sendToFirebase(readings);
    lastReading = millis();
    readingCounter++;
  }

  delay(100);
}

void connectToWiFi()
{
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20)
  {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println();
    Serial.println("WiFi connected successfully!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Signal strength: ");
    Serial.println(WiFi.RSSI());
  }
  else
  {
    Serial.println();
    Serial.println("Failed to connect to WiFi. Check credentials.");
  }
}

SensorReadings takeSensorReadings()
{
  SensorReadings readings;

  // MQ135 Analog via MUX channel 0
  selectMuxChannel(0);
  delay(5);
  readings.mq135_raw = analogRead(Z_PIN);
  readings.mq135_percent = map(readings.mq135_raw, MQ135_MIN, MQ135_MAX, 0, 100);
  readings.mq135_percent = constrain(readings.mq135_percent, 0, 100);

  // Soil Moisture Analog via MUX channel 1
  selectMuxChannel(1);
  delay(5);
  readings.soil_raw = analogRead(Z_PIN);
  readings.soil_percent = map(readings.soil_raw, SOIL_MIN, SOIL_MAX, 0, 100);
  readings.soil_percent = constrain(readings.soil_percent, 0, 100);

  // MQ135 Digital direct pin
  readings.mq135_digital = digitalRead(MQ135_DO_PIN);

  // DHT22
  readings.dht_temp = dht.readTemperature();
  readings.dht_hum = dht.readHumidity();
  if (!isnan(readings.dht_temp) && !isnan(readings.dht_hum))
  {
    readings.dht_temp_percent = map(readings.dht_temp * 100, TEMP_MIN * 100, TEMP_MAX * 100, 0, 100);
    readings.dht_temp_percent = constrain(readings.dht_temp_percent, 0, 100);
    readings.dht_hum_percent = readings.dht_hum; // Already in percentage
  }
  else
  {
    readings.dht_temp = -999;
    readings.dht_hum = -999;
    readings.dht_temp_percent = 0;
    readings.dht_hum_percent = 0;
  }

  // DS18B20
  ds18b20.requestTemperatures();
  readings.ds18b20_temp = ds18b20.getTempCByIndex(0);
  if (readings.ds18b20_temp != DEVICE_DISCONNECTED_C)
  {
    readings.ds18b20_temp_percent = map(readings.ds18b20_temp * 100, TEMP_MIN * 100, TEMP_MAX * 100, 0, 100);
    readings.ds18b20_temp_percent = constrain(readings.ds18b20_temp_percent, 0, 100);
  }
  else
  {
    readings.ds18b20_temp = -999;
    readings.ds18b20_temp_percent = 0;
  }

  // Additional data
  readings.timestamp = getFormattedTime();
  readings.uptime = millis();
  readings.deviceId = WiFi.macAddress();
  readings.ipAddress = WiFi.localIP().toString();

  return readings;
}

void syncTimeWithNTP()
{
  Serial.println("Synchronizing time with NTP server...");

  // Try multiple NTP servers
  String ntpServers[] = {"pool.ntp.org", "time.nist.gov", "time.google.com"};

  for (int server = 0; server < 3 && !timeSync; server++)
  {
    Serial.println("Trying NTP server: " + ntpServers[server]);
    timeClient.setPoolServerName(ntpServers[server].c_str());

    // Multiple attempts per server
    for (int attempt = 0; attempt < 5; attempt++)
    {
      Serial.print("Attempt " + String(attempt + 1) + "...");

      if (timeClient.forceUpdate())
      {
        time_t epochTime = timeClient.getEpochTime();
        if (epochTime > 946684800)
        { // After year 2000
          timeSync = true;
          lastTimeSync = millis();
          Serial.println(" SUCCESS!");
          Serial.println("Current time: " + getFormattedTime());
          return;
        }
      }

      Serial.println(" failed");
      delay(2000);
    }
  }

  if (!timeSync)
  {
    Serial.println("Failed to sync time with any NTP server!");
  }
}

String getFormattedTime()
{
  time_t epochTime = timeClient.getEpochTime();

  // Check if time is valid (after year 2000)
  if (epochTime < 946684800)
  {
    return "Time not synced";
  }

  // Convert to local time manually (UTC + 5:30)
  epochTime += 19800; // Add 5.5 hours in seconds

  struct tm *ptm = gmtime(&epochTime);
  char timeString[35];
  sprintf(timeString, "%04d-%02d-%02d %02d:%02d:%02d (LK)",
          ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday,
          ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
  return String(timeString);
}

void printReadings(SensorReadings readings)
{
  Serial.println("----- New Reading -----");
  Serial.println("Reading " + String(readingCounter + 1));
  Serial.println("Timestamp: " + readings.timestamp);

  // DS18B20
  if (readings.ds18b20_temp != -999)
  {
    Serial.printf("DS18B20 Temp: %.2f °C (%d%%)\n", readings.ds18b20_temp, readings.ds18b20_temp_percent);
  }
  else
  {
    Serial.println("DS18B20 Temp: ERROR");
  }

  // DHT22
  if (readings.dht_temp != -999 && readings.dht_hum != -999)
  {
    Serial.printf("DHT22 Temp: %.2f °C (%d%%), Humidity: %.2f %% (%d%%)\n",
                  readings.dht_temp, readings.dht_temp_percent, readings.dht_hum, readings.dht_hum_percent);
  }
  else
  {
    Serial.println("DHT22: Error reading sensor");
  }

  // MQ135
  Serial.printf("MQ135 Analog: %d (%d%%), Digital: %d (%s)\n",
                readings.mq135_raw, readings.mq135_percent, readings.mq135_digital,
                readings.mq135_digital ? "Air OK" : "Threshold Exceeded");

  // Soil Moisture
  Serial.printf("Soil Moisture Analog: %d (%d%%)\n", readings.soil_raw, readings.soil_percent);

  Serial.println("IP Address: " + readings.ipAddress);
  Serial.println("------------------------\n");
}

void sendToFirebase(SensorReadings readings)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi not connected, skipping Firebase upload");
    return;
  }

  FirebaseJson sensorJson;
  sensorJson.set("ds18b20_soil_temp", readings.ds18b20_temp);
  sensorJson.set("ds18b20_soil_temp_%", readings.ds18b20_temp_percent);
  sensorJson.set("soil_moisture", readings.soil_raw);
  sensorJson.set("soil_moisture_%", readings.soil_percent);

  sensorJson.set("dht22_temp", readings.dht_temp);
  sensorJson.set("dht22_temp_%", readings.dht_temp_percent);
  sensorJson.set("dht22_humidity", readings.dht_hum);
  sensorJson.set("dht22_humidity_%", readings.dht_hum_percent);
  sensorJson.set("mq135_air_analog", readings.mq135_raw);
  sensorJson.set("mq135_air_analog_%", readings.mq135_percent);
  sensorJson.set("mq135_air_digital", readings.mq135_digital);

  sensorJson.set("timestamp", readings.timestamp);
  sensorJson.set("ipAddress", readings.ipAddress);

  String sensorPath = "/sensor_readings";
  if (Firebase.pushJSON(firebaseData, sensorPath, sensorJson))
  {
    Serial.println("✓ Sensor data sent to Firebase");
    Serial.println("Firebase Path: " + firebaseData.dataPath());
  }
  else
  {
    Serial.println("✗ Failed to send sensor data to Firebase");
    Serial.println("Reason: " + firebaseData.errorReason());
  }
}