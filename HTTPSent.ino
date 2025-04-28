#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <ArduinoJson.h>
#include <WebSocketsServer.h>
#include "iot_configs.h"

// กำหนด DHT
#define DHTPIN1 4  // DHT22 ตัวที่ 1 สำหรับ Step 2 (ระหว่างเติม)
#define DHTPIN2 15 // DHT22 ตัวที่ 2 สำหรับ Step 4 (หลังเติม)
#define DHTTYPE DHT22
DHT dht1(DHTPIN1, DHTTYPE);
DHT dht2(DHTPIN2, DHTTYPE);

// กำหนด IR Sensor
#define IR_PIN_1 13  // สำหรับ Step 1: นับวัตถุดิบ
#define IR_PIN_2 12  // สำหรับ Step 6: นับสินค้าหลังผสม
#define IR_PIN_3 14  // สำหรับ Step 5: นับสินค้าหลังเติม

// WiFi credentials
const char* ssid = IOT_CONFIG_WIFI_SSID;
const char* password = IOT_CONFIG_WIFI_PASSWORD;

// Azure IoT settings
const char* deviceId = "ConveyorMonitor";
const char* idScope = "0ne00EA800E";
const char* deviceKey = "D/d7CklYTuRF56qT1DLmMH2OH9zk6+RvFo1eQpFzzdU=";

// Azure IoT Hub REST API endpoint
String iotHubHost = "";
String iotHubEndpoint = "";
String sasToken = "";
unsigned long sasTokenExpiry = 0;

// ตัวแปรสำหรับเก็บข้อมูล
float temperature1 = -1;  // เก็บค่าล่าสุดจาก DHT22 ตัวที่ 1
float humidity1 = -1;     // เก็บค่าล่าสุดจาก DHT22 ตัวที่ 1
int materialCount = 0;
int batchCount = 0;
int finalCount = 0;
int totalItems = 0;
int batchNumber = 1;
int lastState1 = HIGH;
int lastState2 = HIGH;
int lastState3 = HIGH;
const int materialTarget = 3;
const int batchSize = 10;
float tempMax = -100.0;
float tempMin = 100.0;
float finalBatchTemp = -1.0;  // อุณหภูมิสุดท้ายของแบทช์ (จาก DHT22 ตัวที่ 2)
const float tempLimit = 55.0;
bool countingPhase = false;

// ตัวแปรสำหรับการจัดการเวลา
#define PST_TIME_ZONE 7
#define GMT_OFFSET_SECS (PST_TIME_ZONE * 3600)
#define NTP_SERVERS "pool.ntp.org", "time.nist.gov"
#define UNIX_TIME_NOV_13_2017 1510592825

WiFiServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);
bool isWiFiConnected = false;
bool isTimeSynced = false;
bool isProvisioned = false;
bool isFirstLoop = true;

// ฟังก์ชันเชื่อมต่อ WiFi
bool connect_to_wifi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  int retryCount = 0;
  while (WiFi.status() != WL_CONNECTED && retryCount < 20) {
    delay(500);
    Serial.print(".");
    retryCount++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("");
    Serial.println("Failed to connect to WiFi");
    return false;
  }
}

// ฟังก์ชันซิงโครไนซ์เวลา
bool sync_device_clock_with_ntp_server() {
  Serial.println("Setting time using SNTP");
  configTime(GMT_OFFSET_SECS, 0, NTP_SERVERS);
  time_t now = time(NULL);
  int retryCount = 0;
  while (now < UNIX_TIME_NOV_13_2017 && retryCount < 20) {
    delay(500);
    Serial.print(".");
    now = time(NULL);
    retryCount++;
  }
  if (now >= UNIX_TIME_NOV_13_2017) {
    Serial.println("");
    Serial.println("Time initialized!");
    return true;
  } else {
    Serial.println("");
    Serial.println("Failed to sync time!");
    return false;
  }
}

// ฟังก์ชันสร้าง SAS token
String generateSasToken(const char* deviceKey, const char* resourceUri, unsigned long expiry) {
  size_t decodedKeyLength;
  unsigned char decodedKey[32];
  int decodeResult = mbedtls_base64_decode(decodedKey, sizeof(decodedKey), &decodedKeyLength, (unsigned char*)deviceKey, strlen(deviceKey));
  if (decodeResult != 0) {
    Serial.println("Failed to decode device key!");
    return "";
  }

  String stringToSign = String(resourceUri) + "\n" + String(expiry);
  unsigned char hmac[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
  mbedtls_md_hmac_starts(&ctx, decodedKey, decodedKeyLength);
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)stringToSign.c_str(), stringToSign.length());
  mbedtls_md_hmac_finish(&ctx, hmac);
  mbedtls_md_free(&ctx);

  size_t encodedSignLength;
  char encodedSign[64];
  int encodeResult = mbedtls_base64_encode((unsigned char*)encodedSign, sizeof(encodedSign), &encodedSignLength, hmac, 32);
  if (encodeResult != 0) {
    Serial.println("Failed to encode signature!");
    return "";
  }

  String sasToken = "SharedAccessSignature sr=" + String(resourceUri) + "&sig=" + URLEncode(String(encodedSign)) + "&se=" + String(expiry);
  return sasToken;
}

// ฟังก์ชัน URL encode
String URLEncode(String str) {
  String encodedString = "";
  char c;
  char code0;
  char code1;
  for (unsigned int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encodedString += '+';
    } else if (isalnum(c)) {
      encodedString += c;
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) {
        code1 = (c & 0xf) - 10 + 'A';
      }
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) {
        code0 = c - 10 + 'A';
      }
      encodedString += '%';
      encodedString += code0;
      encodedString += code1;
    }
  }
  return encodedString;
}

// ฟังก์ชันสำหรับ Connect Azure,DPS provisioning
bool provisionDevice() {
  HTTPClient http;
  String dpsEndpoint = "https://global.azure-devices-provisioning.net/" + String(idScope) + "/registrations/" + String(deviceId) + "/register?api-version=2021-06-01";

  DynamicJsonDocument doc(200);
  doc["registrationId"] = deviceId;
  String payload;
  serializeJson(doc, payload);

  String resourceUri = String(idScope) + "/registrations/" + String(deviceId);
  unsigned long expiry = time(NULL) + 3600;
  String sasToken = generateSasToken(deviceKey, resourceUri.c_str(), expiry);
  if (sasToken == "") {
    return false;
  }

  http.begin(dpsEndpoint);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", sasToken);
  int httpCode = http.PUT(payload);

  if (httpCode == 202) {
    String response = http.getString();
    DynamicJsonDocument responseDoc(1024);
    deserializeJson(responseDoc, response);

    String operationId = responseDoc["operationId"].as<String>();
    if (operationId == "") {
      Serial.println("Failed to get operationId from DPS response");
      http.end();
      return false;
    }

    String statusEndpoint = "https://global.azure-devices-provisioning.net/" + String(idScope) + "/registrations/" + String(deviceId) + "/operations/" + operationId + "?api-version=2021-06-01";
    int retryCount = 0;
    while (retryCount < 20) {
      http.begin(statusEndpoint);
      http.addHeader("Authorization", sasToken);
      httpCode = http.GET();

      if (httpCode == 200) {
        response = http.getString();
        deserializeJson(responseDoc, response);

        String status = responseDoc["status"].as<String>();
        Serial.println("DPS status: " + status);
        if (status == "assigned") {
          iotHubHost = responseDoc["registrationState"]["assignedHub"].as<String>();
          iotHubEndpoint = "https://" + iotHubHost + "/devices/" + String(deviceId) + "/messages/events?api-version=2021-04-12";
          Serial.println("Device provisioned successfully!");
          Serial.println("IoT Hub Endpoint: " + iotHubEndpoint);
          http.end();
          return true;
        } else if (status == "failed") {
          String errorCode = responseDoc["errorCode"].as<String>();
          String errorMessage = responseDoc["message"].as<String>();
          Serial.println("DPS provisioning failed with error: " + errorCode + " - " + errorMessage);
          http.end();
          return false;
        }
      }
      http.end();
      delay(5000);
      retryCount++;
    }
    Serial.println("DPS provisioning timed out after 20 retries.");
    http.end();
    return false;
  } else {
    Serial.println("DPS provisioning failed: " + String(httpCode));
    Serial.println(http.getString());
    http.end();
    return false;
  }
}

// ฟังก์ชันตรวจสอบและสร้าง SAS token
bool ensureSasToken() {
  if (sasToken == "" || time(NULL) >= sasTokenExpiry - 300) {
    String resourceUri = iotHubHost + "/devices/" + String(deviceId);
    sasTokenExpiry = time(NULL) + 3600;
    sasToken = generateSasToken(deviceKey, resourceUri.c_str(), sasTokenExpiry);
    if (sasToken == "") {
      Serial.println("Failed to generate SAS token!");
      return false;
    }
  }
  return true;
}

// ฟังก์ชันส่ง telemetry ผ่าน HTTP
bool sendTelemetry() {
  float temp = dht1.readTemperature();
  float hum = dht1.readHumidity();

  if (!isnan(temp) && !isnan(hum)) {
    temperature1 = temp;
    humidity1 = hum;
  } else {
    Serial.println("Failed to read from DHT1 sensor in sendTelemetry!");
  }

  if (temperature1 != -1 && !countingPhase) {  // อัปเดต tempMax/tempMin เฉพาะใน Phase 1
    if (temperature1 > tempMax) tempMax = temperature1;
    if (temperature1 < tempMin) tempMin = temperature1;
  }

  DynamicJsonDocument doc(200);
  doc["temperature1"] = temperature1;
  doc["humidity1"] = humidity1;
  doc["finalBatchTemp"] = finalBatchTemp;
  doc["materialCount"] = materialCount;
  doc["batchCount"] = batchCount;
  doc["finalCount"] = finalCount;
  doc["totalItems"] = totalItems;
  doc["batchNumber"] = batchNumber;
  doc["tempMax"] = tempMax;
  doc["tempMin"] = tempMin;
  String payload;
  serializeJson(doc, payload);

  if (!ensureSasToken()) {
    return false;
  }

  HTTPClient http;
  http.begin(iotHubEndpoint);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", sasToken);
  int httpCode = http.POST(payload);

  if (httpCode == 204) {
    Serial.println("Telemetry sent successfully!");
    Serial.print("Temperature (DHT1): ");
    Serial.print(temperature1);
    Serial.print(" C, Humidity (DHT1): ");
    Serial.print(humidity1);
    Serial.println(" %");
    Serial.print("Final Batch Temperature (DHT2): ");
    Serial.print(finalBatchTemp);
    Serial.println(" C");
    Serial.print("Material Count: ");
    Serial.println(materialCount);
    Serial.print("Batch Count: ");
    Serial.println(batchCount);
    Serial.print("Final Count: ");
    Serial.println(finalCount);
    http.end();
    return true;
  } else {
    Serial.println("Failed to send telemetry: " + String(httpCode));
    Serial.println(http.getString());
    http.end();
    return false;
  }
}

// WebSocket Event Handler
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("WebSocket Client [%u] Disconnected\n", num);
      break;
    case WStype_CONNECTED:
      Serial.printf("WebSocket Client [%u] Connected\n", num);
      break;
    case WStype_TEXT:
      break;
  }
}

// ฟังก์ชันสร้าง JSON สำหรับส่งผ่าน WebSocket
String getDataAsJson(float temp1, float humidity1, float finalTemp, String materialStatus, String batchStatus, String finalStatus, String tempStatus, String tempAlert) {
  DynamicJsonDocument doc(512);
  doc["materialCount"] = materialCount;
  doc["materialStatus"] = materialStatus;
  doc["temperature1"] = temp1;
  doc["humidity1"] = humidity1;
  doc["tempMax"] = tempMax;
  doc["tempMin"] = tempMin;
  doc["finalBatchTemp"] = finalTemp;
  doc["tempStatus"] = tempStatus;
  doc["tempAlert"] = tempAlert;
  doc["batchCount"] = batchCount;
  doc["batchStatus"] = batchStatus;
  doc["finalCount"] = finalCount;
  doc["finalStatus"] = finalStatus;
  doc["totalItems"] = totalItems;
  doc["batchNumber"] = batchNumber;
  doc["countingPhase"] = countingPhase ? "Phase 2 (Items After Filling)" : "Phase 1 (Material Count)";

  String json;
  serializeJson(doc, json);
  return json;
}

// หน้าเว็บ
String getHTML() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Conveyor Dashboard</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; background-color: #f5f5f5; margin: 0; padding: 20px; color: #333; }";
  html += "h1 { color: #1a3c5a; text-align: center; font-size: 2em; margin-bottom: 20px; }";
  html += ".container { max-width: 1000px; margin: 0 auto; }";
  html += ".section { background-color: #fff; border: 1px solid #ddd; padding: 15px; margin-bottom: 15px; }";
  html += ".section h2 { color: #1a3c5a; font-size: 1.4em; margin-bottom: 10px; border-bottom: 1px solid #1a3c5a; padding-bottom: 5px; }";
  html += ".data { font-size: 1.1em; margin: 10px 0; display: flex; justify-content: space-between; align-items: center; padding: 5px 0; border-bottom: 1px solid #eee; }";
  html += ".data-label { font-weight: bold; color: #333; width: 200px; }";
  html += ".data-value { color: #555; }";
  html += ".status { padding: 5px 10px; border-radius: 3px; font-weight: bold; font-size: 0.9em; margin-left: 10px; }";
  html += ".status.ok, .status.pass { background-color: #28a745; color: white; }";
  html += ".status.not-ok, .status.fail { background-color: #dc3545; color: white; }";
  html += ".status.warning { background-color: #ff9800; color: white; }";
  html += ".alert { background-color: #dc3545; color: white; padding: 10px; text-align: center; margin: 10px 0; font-weight: bold; }";
  html += ".buttons { text-align: center; margin-top: 20px; display: flex; justify-content: center; gap: 10px; }";
  html += "button { padding: 10px 25px; font-size: 1em; border: none; border-radius: 3px; cursor: pointer; transition: background-color 0.3s; font-weight: bold; }";
  html += ".reset-batch { background-color: #dc3545; color: white; }";
  html += ".reset-batch:hover { background-color: #c82333; }";
  html += ".reset-total { background-color: #ff9800; color: white; }";
  html += ".reset-total:hover { background-color: #e68a00; }";
  html += "@media (max-width: 768px) { h1 { font-size: 1.8em; } .section { padding: 10px; } .data { font-size: 1em; flex-direction: column; align-items: flex-start; gap: 5px; } .data-label { width: auto; } .data-value { margin-left: 0; } .buttons { flex-direction: column; gap: 10px; } button { width: 100%; padding: 10px; } }";
  html += "</style></head><body>";
  
  html += "<div class='container'>";
  html += "<h1>Conveyor Dashboard</h1>";

  // Section 1: Process Overview
  html += "<div class='section'>";
  html += "<h2>Process Overview</h2>";
  html += "<div class='data'><span class='data-label'>Current Phase:</span><span class='data-value' id='countingPhase'>Phase 1 (Material Count)</span></div>";
  html += "</div>";

  // Section 2: Material Count (Step 1)
  html += "<div class='section'>";
  html += "<h2>Step 1: Material Count</h2>";
  html += "<div class='data'><span class='data-label'>Material Count:</span><span class='data-value' id='materialCount'>0 / 3 <span class='status' id='materialStatus'>Not OK</span></span></div>";
  html += "</div>";

  // Section 3: Environment Monitoring (Step 2) - ใช้ DHT22 ตัวที่ 1
  html += "<div class='section'>";
  html += "<h2>Step 2: Environment Monitoring (During Filling)</h2>";
  html += "<div class='data'><span class='data-label'>Temperature:</span><span class='data-value' id='temperature1'>Error</span></div>";
  html += "<div class='data'><span class='data-label'>Humidity:</span><span class='data-value' id='humidity1'>Error</span></div>";
  html += "<div class='data'><span class='data-label'>Max Temperature:</span><span class='data-value' id='tempMax'>0 C</span></div>";
  html += "<div class='data'><span class='data-label'>Min Temperature:</span><span class='data-value' id='tempMin'>0 C</span></div>";
  html += "</div>";

  // Section 4: Final Temperature Check (Step 4) - ใช้ DHT22 ตัวที่ 2
  html += "<div class='section'>";
  html += "<h2>Step 4: Final Temperature Check (After Filling)</h2>";
  html += "<div class='data'><span class='data-label'>Final Batch Temperature:</span><span class='data-value' id='finalBatchTemp'>Error <span class='status' id='tempStatus'>Fail</span></span></div>";
  html += "<div class='alert' id='tempAlert' style='display:none;'>Temperature Warning</div>";
  html += "</div>";

  // Section 5: Item Counting (Step 5 & 6)
  html += "<div class='section'>";
  html += "<h2>Step 5 & 6: Item Counting</h2>";
  html += "<div class='data'><span class='data-label'>Items After Filling:</span><span class='data-value' id='batchCount'>0 / 10 <span class='status' id='batchStatus'>Fail</span></span></div>";
  html += "<div class='data'><span class='data-label'>Items After Mixing:</span><span class='data-value' id='finalCount'>0 / 10 <span class='status' id='finalStatus'>Fail</span></span></div>";
  html += "</div>";

  // Section 6: Summary
  html += "<div class='section'>";
  html += "<h2>Summary</h2>";
  html += "<div class='data'><span class='data-label'>Total Items Processed:</span><span class='data-value' id='totalItems'>0</span></div>";
  html += "<div class='data'><span class='data-label'>Batch Number:</span><span class='data-value' id='batchNumber'>1</span></div>";
  html += "</div>";

  // Buttons
  html += "<div class='buttons'>";
  html += "<a href='/resetBatch'><button class='reset-batch'>Reset Batch</button></a>";
  html += "<a href='/resetTotal'><button class='reset-total'>Reset Total Count</button></a>";
  html += "</div>";

  html += "</div>";

  // JavaScript สำหรับ WebSocket
  html += "<script>";
  html += "var ws = new WebSocket('ws://' + window.location.hostname + ':81/');";
  html += "ws.onopen = function() { console.log('WebSocket connected'); };";
  html += "ws.onclose = function() { console.log('WebSocket disconnected'); };";
  html += "ws.onerror = function(error) { console.error('WebSocket error:', error); };";
  html += "ws.onmessage = function(event) {";
  html += "  var data = JSON.parse(event.data);";
  html += "  document.getElementById('countingPhase').innerText = data.countingPhase;";
  html += "  document.getElementById('materialCount').innerHTML = data.materialCount + ' / 3 <span class=\"status ' + (data.materialStatus == 'OK' ? 'ok' : 'not-ok') + '\">' + data.materialStatus + '</span>';";
  html += "  document.getElementById('temperature1').innerText = data.temperature1 == -1 ? 'Error' : (data.temperature1 + ' C');";
  html += "  document.getElementById('humidity1').innerText = data.humidity1 == -1 ? 'Error' : (data.humidity1 + ' %');";
  html += "  document.getElementById('tempMax').innerText = data.tempMax + ' C';";
  html += "  document.getElementById('tempMin').innerText = data.tempMin + ' C';";
  html += "  document.getElementById('finalBatchTemp').innerHTML = (data.finalBatchTemp == -1 ? 'Error' : (data.finalBatchTemp + ' C')) + ' <span class=\"status ' + (data.tempStatus == 'Pass' ? 'pass' : 'fail') + '\">' + data.tempStatus + '</span>';";
  html += "  let tempAlert = document.getElementById('tempAlert');";
  html += "  if (data.tempAlert != '') {";
  html += "    tempAlert.style.display = 'block';";
  html += "    tempAlert.innerText = data.tempAlert;";
  html += "  } else {";
  html += "    tempAlert.style.display = 'none';";
  html += "  }";
  html += "  document.getElementById('batchCount').innerHTML = data.batchCount + ' / 10 <span class=\"status ' + (data.batchStatus == 'Pass' ? 'pass' : 'fail') + '\">' + data.batchStatus + '</span>';";
  html += "  document.getElementById('finalCount').innerHTML = data.finalCount + ' / 10 <span class=\"status ' + (data.finalStatus == 'Pass' ? 'pass' : 'fail') + '\">' + data.finalStatus + '</span>';";
  html += "  document.getElementById('totalItems').innerText = data.totalItems;";
  html += "  document.getElementById('batchNumber').innerText = data.batchNumber;";
  html += "};";
  html += "</script>";

  html += "</body></html>";
  return html;
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting setup...");

  dht1.begin();
  dht2.begin();
  Serial.println("DHT22 sensors initialized");

  pinMode(IR_PIN_1, INPUT);
  pinMode(IR_PIN_2, INPUT);
  pinMode(IR_PIN_3, INPUT);
  Serial.println("IR Sensors initialized");

  isWiFiConnected = connect_to_wifi();
  if (!isWiFiConnected) {
    Serial.println("Proceeding without WiFi...");
  } else {
    Serial.println("WiFi setup complete");
  }

  if (isWiFiConnected) {
    isTimeSynced = sync_device_clock_with_ntp_server();
    if (!isTimeSynced) {
      Serial.println("Proceeding without time sync...");
    } else {
      Serial.println("Time sync complete");
    }
  }

  if (isWiFiConnected && isTimeSynced) {
    isProvisioned = provisionDevice();
    if (!isProvisioned) {
      Serial.println("Proceeding without provisioning...");
    } else {
      Serial.println("Provisioning complete");
    }
  }

  server.begin();
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("Web server started on port 80");
  Serial.println("WebSocket server started on port 81");
  Serial.println("Setup complete! Entering loop...");
}

void loop() {
  if (isFirstLoop) {
    Serial.println("Loop running...");
    isFirstLoop = false;
  }

  // จัดการ WebSocket
  webSocket.loop();

  // จัดการคำขอจากไคลเอนต์ (หน้าเว็บ)
  WiFiClient client = server.available();
  if (client) {
    Serial.println("New HTTP client connected");
    client.setTimeout(5000);
    String req = client.readStringUntil('\r');
    Serial.println("Request: " + req);
    client.flush();

    float temp = dht1.readTemperature();
    float hum = dht1.readHumidity();
    if (!isnan(temp) && !isnan(hum)) {
      temperature1 = temp;
      humidity1 = hum;
      Serial.print("Temperature (DHT1): ");
      Serial.print(temperature1);
      Serial.print(" C, Humidity (DHT1): ");
      Serial.print(humidity1);
      Serial.println(" %");
    } else {
      Serial.println("Failed to read DHT1 in HTTP request");
    }

    String materialStatus = (materialCount >= materialTarget) ? "OK" : "Not OK";
    String batchStatus = (batchCount == batchSize) ? "Pass" : "Fail";
    String finalStatus = (finalCount == batchSize) ? "Pass" : "Fail";
    String tempStatus = (finalBatchTemp <= tempLimit && finalBatchTemp != -1) ? "Pass" : "Fail";
    String tempAlert = (finalBatchTemp > tempLimit) ? "Warning: Temperature too high" : "";

    if (req.indexOf("/data") != -1) {
      String html = getHTML();
      client.println("HTTP/1.1 200 OK");
      client.println("Content-type:text/html");
      client.println();
      client.println(html);
      Serial.println("Served webpage");
    } else if (req.indexOf("/resetBatch") != -1) {
      materialCount = 0;
      batchCount = 0;
      finalCount = 0;
      batchNumber = 1;
      countingPhase = false;
      tempMax = -100.0;
      tempMin = 100.0;
      finalBatchTemp = -1.0;
      client.println("HTTP/1.1 303 See Other");
      client.println("Location: /data");
      client.println();
      Serial.println("Reset Batch");
    } else if (req.indexOf("/resetTotal") != -1) {
      materialCount = 0;
      batchCount = 0;
      finalCount = 0;
      totalItems = 0;
      batchNumber = 1;
      countingPhase = false;
      tempMax = -100.0;
      tempMin = 100.0;
      finalBatchTemp = -1.0;
      client.println("HTTP/1.1 303 See Other");
      client.println("Location: /data");
      client.println();
      Serial.println("Reset Total");
    } else {
      client.println("HTTP/1.1 404 Not Found");
      client.println();
      Serial.println("Served 404 Not Found");
    }
    client.stop();
    Serial.println("HTTP client disconnected");
  }

  // อ่านอุณหภูมิจาก DHT22 ตัวที่ 1 (ระหว่างเติม)
  static unsigned long lastDHT1Read = 0;
  if (millis() - lastDHT1Read > 2000) {  // อ่าน DHT1 ทุก 2 วินาที
    float temp = dht1.readTemperature();
    float hum = dht1.readHumidity();
    if (!isnan(temp) && !isnan(hum)) {  // ถ้าอ่านค่าได้
      temperature1 = temp;
      humidity1 = hum;
      if (!countingPhase) {  // อัปเดต tempMax/tempMin เฉพาะใน Phase 1
        if (temperature1 > tempMax) tempMax = temperature1;
        if (temperature1 < tempMin) tempMin = temperature1;
      }
    } else {
      Serial.println("Failed to read DHT1 in loop");
    }
    lastDHT1Read = millis();
  }

  // ส่งข้อมูลผ่าน WebSocket ทุก 5 วินาที
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate > 5000) {
    String materialStatus = (materialCount >= materialTarget) ? "OK" : "Not OK";
    String batchStatus = (batchCount == batchSize) ? "Pass" : "Fail";
    String finalStatus = (finalCount == batchSize) ? "Pass" : "Fail";
    String tempStatus = (finalBatchTemp <= tempLimit && finalBatchTemp != -1) ? "Pass" : "Fail";
    String tempAlert = (finalBatchTemp > tempLimit) ? "Warning: Temperature too high" : "";

    String json = getDataAsJson(temperature1, humidity1, finalBatchTemp, materialStatus, batchStatus, finalStatus, tempStatus, tempAlert);
    webSocket.broadcastTXT(json);
    Serial.print("WebSocket JSON sent: ");
    Serial.println(json);
    lastUpdate = millis();
  }

  // อ่านสถานะของ IR Sensor 1 (GPIO 13) สำหรับ Step 1: นับวัตถุดิบ
  int sensorState1 = digitalRead(IR_PIN_1);
  if (!countingPhase) {
    if (sensorState1 == LOW && lastState1 == HIGH) {
      materialCount++;
      Serial.print("Step 1 - Material Count: ");
      Serial.println(materialCount);
      if (materialCount >= materialTarget) {
        countingPhase = true;
        Serial.println("Material Count reached target. Switching to Phase 2 (Items After Filling)");
      }
    }
  }
  lastState1 = sensorState1;

  // อ่านสถานะของ IR Sensor 3 (GPIO 14) สำหรับ Step 5: นับสินค้าหลังเติม
  int sensorState3 = digitalRead(IR_PIN_3);
  if (countingPhase) {
    if (sensorState3 == LOW && lastState3 == HIGH) {
      batchCount++;
      totalItems++;
      Serial.print("Step 5 - Items After Filling: ");
      Serial.println(batchCount);
      if (batchCount >= batchSize) {
        // อ่านอุณหภูมิจาก DHT22 ตัวที่ 2 เมื่อแบทช์เสร็จ
        finalBatchTemp = dht2.readTemperature();
        if (isnan(finalBatchTemp)) {
          finalBatchTemp = -1;
          Serial.println("Failed to read DHT2 at batch completion");
        } else {
          Serial.print("Step 4 - Final Batch Temperature (DHT2): ");
          Serial.print(finalBatchTemp);
          Serial.println(" C");
        }
        batchNumber++;
        Serial.print("Batch Number incremented to: ");
        Serial.println(batchNumber);
      }
    }
  }
  lastState3 = sensorState3;

  // Debug: ตรวจสอบสถานะ
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug > 15000) {
    Serial.print("IR Sensor 1 State: ");
    Serial.println(sensorState1);
    Serial.print("IR Sensor 3 State: ");
    Serial.println(sensorState3);
    Serial.print("Counting Phase: ");
    Serial.println(countingPhase ? "Phase 2 (Items After Filling)" : "Phase 1 (Material Count)");
    Serial.print("Material Count: ");
    Serial.println(materialCount);
    Serial.print("Batch Count: ");
    Serial.println(batchCount);
    Serial.print("Batch Number: ");
    Serial.println(batchNumber);
    lastDebug = millis();
  }

  // ขั้นตอน 6: นับสินค้าหลังผสม (Step 6: Items After Mixing)
  int sensorState2 = digitalRead(IR_PIN_2);
  if (sensorState2 == LOW && lastState2 == HIGH) {
    finalCount++;
    totalItems++;
    Serial.print("Step 6 - Items After Mixing: ");
    Serial.println(finalCount);
    if (finalCount >= batchSize) {
      finalCount = 0;
      batchCount = 0;  // รีเซ็ต batchCount
      materialCount = 0;  // รีเซ็ต materialCount
      countingPhase = false;  // กลับไป Phase 1
      finalBatchTemp = -1.0;  // รีเซ็ต finalBatchTemp
      Serial.println("Final Count reset to 0, Batch Count reset to 0, Material Count reset to 0, Final Batch Temp reset to -1, Switching back to Phase 1");
    }
  }
  lastState2 = sensorState2;

  // Debug: ตรวจสอบ IR Sensor 2
  if (millis() - lastDebug > 15000) {
    Serial.print("IR Sensor 2 State: ");
    Serial.println(sensorState2);
    lastDebug = millis();
  }

  // ส่ง Telemetry
  static unsigned long lastTelemetry = 0;
  if (millis() - lastTelemetry > 30000) {
    Serial.println("Checking telemetry...");
    if (isWiFiConnected && isTimeSynced && isProvisioned) {
      Serial.println("Attempting to send telemetry...");
      if (!sendTelemetry()) {
        Serial.println("Retrying telemetry in 5 seconds...");
        delay(5000);
        if (!sendTelemetry()) {
          Serial.println("Telemetry failed after retry. Checking connection...");
          isWiFiConnected = connect_to_wifi();
          if (isWiFiConnected) {
            isTimeSynced = sync_device_clock_with_ntp_server();
            if (isTimeSynced) {
              isProvisioned = provisionDevice();
            }
          }
        }
      }
    } else {
      Serial.println("Skipping telemetry: Not fully connected");
      Serial.print("WiFi Connected: ");
      Serial.println(isWiFiConnected ? "Yes" : "No");
      Serial.print("Time Synced: ");
      Serial.println(isTimeSynced ? "Yes" : "No");
      Serial.print("Provisioned: ");
      Serial.println(isProvisioned ? "Yes" : "No");
      if (!isWiFiConnected) {
        isWiFiConnected = connect_to_wifi();
        if (isWiFiConnected) {
          isTimeSynced = sync_device_clock_with_ntp_server();
          if (isTimeSynced) {
            isProvisioned = provisionDevice();
          }
        }
      }
    }
    lastTelemetry = millis();
  }
}