#include <ESP8266WiFi.h>
#include <Wire.h>
#include <SoftwareSerial.h>
#include <PubSubClient.h>
#include "SparkFunCCS811.h"
#include "SparkFun_Si7021_Breakout_Library.h"
#include "Adafruit_BMP280.h"
#include "PMS.h"
#include <DHT.h>
#include "secrets.h"

// ====== Sensors ======
#define CCS811_ADDR 0x5A

#define DHT_PIN   5      // NodeMCU D1 = GPIO5
#define DHT_TYPE  DHT21  // For AM2301A / DHT21

CCS811 mySensor(CCS811_ADDR);
Weather sensor;              // Si7021 lib calls it Weather
Adafruit_BMP280 bmp;
DHT dht(DHT_PIN, DHT_TYPE);

SoftwareSerial swSer(12, 13, false); // RX=GPIO12, TX=GPIO13
PMS pms(swSer);
PMS::DATA pmsData;

// ====== Home Assistant Discovery ======
const char* DEVICE_NAME = "air_quality";
const char* HA_DISCOVERY_PREFIX = "homeassistant";

// Topics
String baseTopic;
String availTopic;

// MQTT client
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ====== Timing ======
const unsigned long SAMPLE_INTERVAL_MS  = 60000UL;   // 30 sec
const unsigned long PUBLISH_INTERVAL_MS = 300000UL;  // 5 min

unsigned long lastSampleMs = 0;
unsigned long lastPublishMs = 0;

// ====== Filter settings ======
const int SAMPLE_COUNT = 10;
const float REL_THRESHOLD = 0.30f;

// Minimum absolute thresholds
const float TEMP_MIN_THRESHOLD       = 0.5f;   // °C
const float HUM_MIN_THRESHOLD        = 2.0f;   // %
const float PRESSURE_MIN_THRESHOLD   = 1.0f;   // hPa
const float CO2_MIN_THRESHOLD        = 50.0f;  // ppm
const float TVOC_MIN_THRESHOLD       = 10.0f;  // ppb
const float PM_MIN_THRESHOLD         = 3.0f;   // µg/m³

// ====== Buffers ======
struct SensorBuffer {
  float values[SAMPLE_COUNT];
  uint8_t count = 0;

  void clear() {
    count = 0;
  }

  void add(float v) {
    if (count < SAMPLE_COUNT) {
      values[count++] = v;
    }
  }

  bool full() const {
    return count >= SAMPLE_COUNT;
  }

  bool hasData() const {
    return count > 0;
  }
};

SensorBuffer bufTempSi;
SensorBuffer bufHum;
SensorBuffer bufDhtTemp;
SensorBuffer bufDhtHum;
SensorBuffer bufBmpTemp;
SensorBuffer bufPressure;
SensorBuffer bufCO2;
SensorBuffer bufTVOC;
SensorBuffer bufPM1;
SensorBuffer bufPM25;
SensorBuffer bufPM10;

// ====== Helpers ======
String chipIdStr() {
  uint32_t id = ESP.getChipId();
  char buf[9];
  snprintf(buf, sizeof(buf), "%08X", id);
  return String(buf);
}

void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());
}

bool mqttConnect() {
  if (mqtt.connected()) return true;

  String clientId = String(DEVICE_NAME) + "_" + chipIdStr();
  Serial.print("MQTT connecting as ");
  Serial.println(clientId);

  bool ok = mqtt.connect(
    clientId.c_str(),
    MQTT_USER,
    MQTT_PASS,
    availTopic.c_str(),
    0,
    true,
    "offline"
  );

  if (!ok) {
    Serial.print("MQTT connect failed, rc=");
    Serial.println(mqtt.state());
    return false;
  }

  mqtt.publish(availTopic.c_str(), "online", true);
  return true;
}

void publishDiscoverySensor(
  const String& sensorKey,
  const String& name,
  const String& stateTopic,
  const String& unit,
  const String& deviceClass,
  const String& stateClass,
  const String& icon
) {
  String uniq = String(DEVICE_NAME) + "_" + chipIdStr() + "_" + sensorKey;
  String configTopic = String(HA_DISCOVERY_PREFIX) + "/sensor/" + uniq + "/config";

  String payload = "{";
  payload += "\"name\":\"" + name + "\",";
  payload += "\"unique_id\":\"" + uniq + "\",";
  payload += "\"state_topic\":\"" + stateTopic + "\",";
  payload += "\"availability_topic\":\"" + availTopic + "\",";
  payload += "\"payload_available\":\"online\",";
  payload += "\"payload_not_available\":\"offline\",";

  if (unit.length()) payload += "\"unit_of_measurement\":\"" + unit + "\",";
  if (deviceClass.length()) payload += "\"device_class\":\"" + deviceClass + "\",";
  if (stateClass.length()) payload += "\"state_class\":\"" + stateClass + "\",";
  if (icon.length()) payload += "\"icon\":\"" + icon + "\",";

  payload += "\"device\":{";
  payload += "\"identifiers\":[\"" + String(DEVICE_NAME) + "_" + chipIdStr() + "\"],";
  payload += "\"name\":\"" + String(DEVICE_NAME) + "\",";
  payload += "\"model\":\"ESP8266 Air Quality\",";
  payload += "\"manufacturer\":\"DIY\"";
  payload += "}";
  payload += "}";

  bool ok = mqtt.publish(configTopic.c_str(), payload.c_str(), true);
  Serial.print("DISCOVERY ");
  Serial.print(sensorKey);
  Serial.print(" bytes=");
  Serial.print(payload.length());
  Serial.print(" ok=");
  Serial.println(ok ? "true" : "false");
}

void publishAllDiscovery() {
  publishDiscoverySensor("co2", "CO2", baseTopic + "/co2", "ppm", "carbon_dioxide", "measurement", "mdi:molecule-co2");
  publishDiscoverySensor("tvoc", "TVOC", baseTopic + "/tvoc", "ppb", "volatile_organic_compounds_parts", "measurement", "mdi:air-filter");

  publishDiscoverySensor("temp", "Temperature (Si7021)", baseTopic + "/temperature", "°C", "temperature", "measurement", "");
  publishDiscoverySensor("hum", "Humidity (Si7021)", baseTopic + "/humidity", "%", "humidity", "measurement", "");

  publishDiscoverySensor("dht_temp", "Temperature (DHT21)", baseTopic + "/dht_temperature", "°C", "temperature", "measurement", "");
  publishDiscoverySensor("dht_hum", "Humidity (DHT21)", baseTopic + "/dht_humidity", "%", "humidity", "measurement", "");

  publishDiscoverySensor("bmp_temp", "Temperature (BMP280)", baseTopic + "/bmp_temperature", "°C", "temperature", "measurement", "");
  publishDiscoverySensor("pressure", "Pressure", baseTopic + "/pressure", "hPa", "pressure", "measurement", "mdi:gauge");

  publishDiscoverySensor("pm1", "PM1.0", baseTopic + "/pm1_0", "µg/m³", "pm1", "measurement", "mdi:blur");
  publishDiscoverySensor("pm25", "PM2.5", baseTopic + "/pm2_5", "µg/m³", "pm25", "measurement", "mdi:blur");
  publishDiscoverySensor("pm10", "PM10", baseTopic + "/pm10", "µg/m³", "pm10", "measurement", "mdi:blur");
}

void publishFloat(const String& topic, float val, uint8_t decimals = 2) {
  char buf[32];
  dtostrf(val, 0, decimals, buf);
  mqtt.publish(topic.c_str(), buf, false);
}

void publishInt(const String& topic, int val) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", val);
  mqtt.publish(topic.c_str(), buf, false);
}

// ====== Math helpers ======
void copyArray(const float* src, float* dst, uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    dst[i] = src[i];
  }
}

void sortArray(float* arr, uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    for (uint8_t j = i + 1; j < count; j++) {
      if (arr[j] < arr[i]) {
        float t = arr[i];
        arr[i] = arr[j];
        arr[j] = t;
      }
    }
  }
}

float medianOfArray(const float* arr, uint8_t count) {
  if (count == 0) return NAN;

  float tmp[SAMPLE_COUNT];
  copyArray(arr, tmp, count);
  sortArray(tmp, count);

  if (count % 2 == 1) {
    return tmp[count / 2];
  } else {
    uint8_t idx = count / 2;
    return (tmp[idx - 1] + tmp[idx]) / 2.0f;
  }
}

float filterAndGetMedian(const SensorBuffer& buf, float minThreshold, const char* label) {
  if (buf.count == 0) return NAN;

  float baseMedian = medianOfArray(buf.values, buf.count);
  if (isnan(baseMedian)) return NAN;

  float threshold = max((float)(fabs(baseMedian) * REL_THRESHOLD), minThreshold);

  float filtered[SAMPLE_COUNT];
  uint8_t filteredCount = 0;

  for (uint8_t i = 0; i < buf.count; i++) {
    float v = buf.values[i];
    if (fabs(v - baseMedian) <= threshold) {
      filtered[filteredCount++] = v;
    }
  }

  Serial.printf("%s: raw_count=%d, base_median=%.2f, threshold=%.2f, filtered_count=%d\n",
                label, buf.count, baseMedian, threshold, filteredCount);

  if (filteredCount >= 3) {
    return medianOfArray(filtered, filteredCount);
  }

  return baseMedian;
}

void clearAllBuffers() {
  bufTempSi.clear();
  bufHum.clear();
  bufDhtTemp.clear();
  bufDhtHum.clear();
  bufBmpTemp.clear();
  bufPressure.clear();
  bufCO2.clear();
  bufTVOC.clear();
  bufPM1.clear();
  bufPM25.clear();
  bufPM10.clear();
}

// ====== Sampling ======
void sampleSensorsOnce() {
  Serial.println("----- SAMPLE START -----");

  // Si7021
  float humidity = sensor.getRH();
  float temp = sensor.getTemp();
  bool envCompSet = false;

  if (!isnan(humidity) && !isnan(temp)) {
    bufHum.add(humidity);
    bufTempSi.add(temp);
    Serial.printf("Si7021 sample: T=%.2f C, RH=%.1f %%\n", temp, humidity);

    // compensation for CCS811
    mySensor.setEnvironmentalData(humidity, temp);
    envCompSet = true;
  } else {
    Serial.println("Si7021 sample invalid");
  }

  // DHT21 / AM2301A
  float dhtHumidity = dht.readHumidity();
  float dhtTemp = dht.readTemperature();

  if (!isnan(dhtHumidity) && !isnan(dhtTemp)) {
    bufDhtHum.add(dhtHumidity);
    bufDhtTemp.add(dhtTemp);
    Serial.printf("DHT21 sample: T=%.2f C, RH=%.1f %%\n", dhtTemp, dhtHumidity);

    // Optional fallback compensation for CCS811 if Si7021 failed
    if (!envCompSet) {
      mySensor.setEnvironmentalData(dhtHumidity, dhtTemp);
      envCompSet = true;
    }
  } else {
    Serial.println("DHT21 sample invalid");
  }

  // CCS811
  if (mySensor.dataAvailable()) {
    mySensor.readAlgorithmResults();
    int co2 = mySensor.getCO2();
    int tvoc = mySensor.getTVOC();

    bufCO2.add((float)co2);
    bufTVOC.add((float)tvoc);

    Serial.printf("CCS811 sample: CO2=%d ppm, TVOC=%d ppb\n", co2, tvoc);
  } else {
    Serial.println("CCS811: no new data");
  }

  // BMP280
  float bmpTemp = bmp.readTemperature();
  float pressure_hPa = bmp.readPressure() / 100.0f;

  if (!isnan(bmpTemp) && !isnan(pressure_hPa)) {
    bufBmpTemp.add(bmpTemp);
    bufPressure.add(pressure_hPa);
    Serial.printf("BMP280 sample: T=%.2f C, P=%.1f hPa\n", bmpTemp, pressure_hPa);
  } else {
    Serial.println("BMP280 sample invalid");
  }

  // PMS
  Serial.println("PMS: request read...");
  pms.requestRead();
  if (pms.readUntil(pmsData, 1000)) {
    bufPM1.add((float)pmsData.PM_AE_UG_1_0);
    bufPM25.add((float)pmsData.PM_AE_UG_2_5);
    bufPM10.add((float)pmsData.PM_AE_UG_10_0);

    Serial.printf("PMS sample: PM1=%d, PM2.5=%d, PM10=%d (ug/m3)\n",
                  pmsData.PM_AE_UG_1_0, pmsData.PM_AE_UG_2_5, pmsData.PM_AE_UG_10_0);
  } else {
    Serial.println("PMS: no data");
  }

  Serial.println("----- SAMPLE END -----");
}

// ====== Publish filtered ======
void publishFilteredData() {
  Serial.println("===== FILTER + PUBLISH START =====");

  float tempSi    = filterAndGetMedian(bufTempSi,   TEMP_MIN_THRESHOLD,     "Temperature (Si7021)");
  float hum       = filterAndGetMedian(bufHum,      HUM_MIN_THRESHOLD,      "Humidity (Si7021)");
  float dhtTemp   = filterAndGetMedian(bufDhtTemp,  TEMP_MIN_THRESHOLD,     "Temperature (DHT21)");
  float dhtHum    = filterAndGetMedian(bufDhtHum,   HUM_MIN_THRESHOLD,      "Humidity (DHT21)");
  float bmpTemp   = filterAndGetMedian(bufBmpTemp,  TEMP_MIN_THRESHOLD,     "Temperature (BMP280)");
  float pressure  = filterAndGetMedian(bufPressure, PRESSURE_MIN_THRESHOLD, "Pressure");
  float co2       = filterAndGetMedian(bufCO2,      CO2_MIN_THRESHOLD,      "CO2");
  float tvoc      = filterAndGetMedian(bufTVOC,     TVOC_MIN_THRESHOLD,     "TVOC");
  float pm1       = filterAndGetMedian(bufPM1,      PM_MIN_THRESHOLD,       "PM1.0");
  float pm25      = filterAndGetMedian(bufPM25,     PM_MIN_THRESHOLD,       "PM2.5");
  float pm10      = filterAndGetMedian(bufPM10,     PM_MIN_THRESHOLD,       "PM10");

  if (!mqtt.connected()) {
    Serial.println("MQTT not connected, publish skipped");
    clearAllBuffers();
    return;
  }

  if (!isnan(co2))      publishInt(baseTopic + "/co2", (int)roundf(co2));
  if (!isnan(tvoc))     publishInt(baseTopic + "/tvoc", (int)roundf(tvoc));

  if (!isnan(hum))      publishFloat(baseTopic + "/humidity", hum, 1);
  if (!isnan(tempSi))   publishFloat(baseTopic + "/temperature", tempSi, 2);

  if (!isnan(dhtHum))   publishFloat(baseTopic + "/dht_humidity", dhtHum, 1);
  if (!isnan(dhtTemp))  publishFloat(baseTopic + "/dht_temperature", dhtTemp, 2);

  if (!isnan(bmpTemp))  publishFloat(baseTopic + "/bmp_temperature", bmpTemp, 2);
  if (!isnan(pressure)) publishFloat(baseTopic + "/pressure", pressure, 1);

  if (!isnan(pm1))      publishInt(baseTopic + "/pm1_0", (int)roundf(pm1));
  if (!isnan(pm25))     publishInt(baseTopic + "/pm2_5", (int)roundf(pm25));
  if (!isnan(pm10))     publishInt(baseTopic + "/pm10", (int)roundf(pm10));

  Serial.printf("PUBLISHED: CO2=%.0f, TVOC=%.0f, Si_T=%.2f, Si_RH=%.1f, DHT_T=%.2f, DHT_RH=%.1f, BMP_T=%.2f, P=%.1f, PM1=%.0f, PM2.5=%.0f, PM10=%.0f\n",
                co2, tvoc, tempSi, hum, dhtTemp, dhtHum, bmpTemp, pressure, pm1, pm25, pm10);

  clearAllBuffers();
  Serial.println("===== FILTER + PUBLISH END =====");
}

void setup() {
  Serial.begin(115200);
  delay(200);

  Wire.begin(2, 14);

  Serial.println("Init CCS811...");
  if (!mySensor.begin()) {
    Serial.println("CCS811 error. Check wiring.");
    while (1) delay(1000);
  }
  mySensor.setDriveMode(3);

  Serial.println("Init Si7021...");
  sensor.begin();

  Serial.println("Init DHT21...");
  dht.begin();

  Serial.println("Init BMP280...");
  if (!bmp.begin(0x76)) {
    Serial.println("BMP280 not found at 0x76");
    while (1) delay(1000);
  }
  bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                  Adafruit_BMP280::SAMPLING_X2,
                  Adafruit_BMP280::SAMPLING_X16,
                  Adafruit_BMP280::FILTER_X16,
                  Adafruit_BMP280::STANDBY_MS_500);

  Serial.println("Init PMS...");
  swSer.begin(9600);
  pms.passiveMode();
  pms.wakeUp();

  wifiConnect();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);

  baseTopic = String(DEVICE_NAME) + "/" + chipIdStr();
  availTopic = baseTopic + "/availability";

  if (mqttConnect()) {
    publishAllDiscovery();
  }

  lastSampleMs = millis() - SAMPLE_INTERVAL_MS;    // first sample immediately
  lastPublishMs = millis();                        // publish after 5 full minutes
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnect();
  }

  if (!mqtt.connected()) {
    if (mqttConnect()) {
      publishAllDiscovery();
    }
  }

  mqtt.loop();

  unsigned long now = millis();

  if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
    lastSampleMs = now;
    sampleSensorsOnce();
  }

  if (now - lastPublishMs >= PUBLISH_INTERVAL_MS) {
    lastPublishMs = now;
    publishFilteredData();
  }
}
