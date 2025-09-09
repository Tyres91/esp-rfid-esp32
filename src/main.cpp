#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>    // <-- NEU: damit DynamicJsonDocument/serializeJson bekannt sind
#include <Wiegand.h>        // esp-rfid-kompatibler Fork (class WIEGAND)
#include <Bounce2.h>
#include <TimeLib.h>
#include <Ticker.h>
#include <SPI.h>
#include <Update.h>

#include "config.h"

// Forward declarations
void setupWifi(bool apMode);
void setupWeb();
void wsBroadcastStatus();
void setupMQTT();
void loopRFID();
void logMaintenance(const String& cmd, const String& arg);
void writeEvent(const String& t1, const String& t2, const String& t3, const String& t4);
void writeLatest(const String& uid, const String& user, int granted, int rssi);

AppConfig config;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
Ticker statusTicker;

// --- Wiegand (esp-rfid API: class WIEGAND, available/getCode/getWiegandType)
WIEGAND wiegand;

// --- LED helper
static void ledBlink(int times = 1, int onMs = 50, int offMs = 50) {
  for (int i = 0; i < times; ++i) {
    digitalWrite(PIN_LED, HIGH);
    delay(onMs);
    digitalWrite(PIN_LED, LOW);
    delay(offMs);
  }
}

// ESP32 WiFi event handler
#if defined(ESP32)
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_START:
      Serial.println(F("[WiFi] AP started"));
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println(F("[WiFi] STA connected"));
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WiFi] Got IP: %s\n", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[WiFi] STA disconnected (reason=%d) -> reconnect\n", info.wifi_sta_disconnected.reason);
      WiFi.reconnect();
      break;
    default:
      break;
  }
}
#endif

// Helpers
static uint32_t chipId32() {
  uint64_t mac = ESP.getEfuseMac();
  return (uint32_t)(mac >> 24);
}

void wsBroadcastStatus() {
  DynamicJsonDocument root(512);
  root["command"]  = "status";
  root["heap"]     = ESP.getFreeHeap();
  root["ip"]       = WiFi.localIP().toString();
  root["hostname"] = (WiFi.getHostname() ? WiFi.getHostname() : "");
  root["chipid"]   = String(chipId32(), HEX);

  String out;
  serializeJson(root, out);
  ws.textAll(out);
}

// WebSocket
static void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type,
                      void * arg, uint8_t * data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("[WS] Client %u connected\n", client->id());
    wsBroadcastStatus();
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("[WS] Client %u disconnected\n", client->id());
  } else if (type == WS_EVT_DATA) {
    String s((char*)data, len);
    s.trim();
    if (s == "status") wsBroadcastStatus();
    else if (s.startsWith("log:")) {
      logMaintenance("cmd", s);
    }
  }
}

// Webserver/OTA
void setupWeb() {
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

  server.on("/update", HTTP_POST,
    [](AsyncWebServerRequest *request){
      bool ok = !Update.hasError();
      auto *res = request->beginResponse(200, "text/plain", ok ? "OK" : "FAIL");
      res->addHeader("Connection", "close");
      request->send(res);
      ESP.restart();
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
      if (!index) {
        Serial.printf("[OTA] Update start: %s\n", filename.c_str());
        // Update.runAsync(true);         // <-- ENTFERNT: gibt's im ESP32 Core 3.x nicht mehr
        Update.begin(UPDATE_SIZE_UNKNOWN);
      }
      if (Update.write(data, len) != len) {
        // Fehlerbehandlung optional
      }
      if (final) {
        if (!Update.end(true)) {
          // Fehlerbehandlung optional
        }
      }
    }
  );

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.begin();
}

// WiFi
void setupWifi(bool apMode) {
  WiFi.mode(WIFI_AP_STA);
  #if defined(ESP32)
    WiFi.onEvent(onWiFiEvent);
  #endif

  if (apMode || config.wifi_ssid.isEmpty()) {
    config.accessPointMode = true;
  }

  if (config.accessPointMode) {
    WiFi.softAP(config.ap_ssid.c_str(), config.ap_pass.c_str(), 1, false, 4);
    Serial.printf("[WiFi] AP SSID: %s  IP: %s\n",
                  config.ap_ssid.c_str(), WiFi.softAPIP().toString().c_str());
  }

  if (!config.wifi_ssid.isEmpty()) {
    WiFi.begin(config.wifi_ssid.c_str(), config.wifi_pass.c_str());
  }

  if (!MDNS.begin(config.hostname.c_str())) {
    Serial.println(F("[mDNS] start failed"));
  } else {
    Serial.printf("[mDNS] http://%s.local\n", config.hostname.c_str());
  }
}

// Logging (SPIFFS)
void writeEvent(const String& t1, const String& t2, const String& t3, const String& t4) {
  DynamicJsonDocument doc(256);
  doc["ts"] = (uint32_t)now();
  doc["a"] = t1; doc["b"] = t2; doc["c"] = t3; doc["d"] = t4;
  String line; serializeJson(doc, line); line += "\n";
  File f = SPIFFS.open("/eventlog.json", "a");
  if (f) { f.print(line); f.close(); }
}

void writeLatest(const String& uid, const String& user, int granted, int rssi) {
  DynamicJsonDocument doc(256);
  doc["ts"] = (uint32_t)now();
  doc["uid"] = uid; doc["user"] = user; doc["ok"] = granted; doc["rssi"] = rssi;
  String line; serializeJson(doc, line); line += "\n";
  File f = SPIFFS.open("/latestlog.json", "a");
  if (f) { f.print(line); f.close(); }
}

void logMaintenance(const String& cmd, const String& arg) {
  (void)cmd; (void)arg; // Platzhalter
}

// MQTT (minimal)
AsyncMqttClient mqtt;
Ticker mqttReconnectTimer;

static void mqttConnect() {
  if (config.mqtt_host.length()) {
    mqtt.connect();
  }
}

void setupMQTT() {
  if (!config.mqtt_host.length()) return;
  mqtt.setServer(config.mqtt_host.c_str(), config.mqtt_port);
  mqtt.onConnect([](bool sess){
    Serial.println(F("[MQTT] connected"));
  });
  mqtt.onDisconnect([](AsyncMqttClientDisconnectReason r){
    Serial.printf("[MQTT] disconnected (%d)\n", (int)r);
    mqttReconnectTimer.once(2, mqttConnect);
  });
}

// RFID / Wiegand – esp-rfid API
void loopRFID() {
  if (wiegand.available()) {
    uint64_t code = wiegand.getCode();
    uint8_t  bits = wiegand.getWiegandType();

    Serial.printf("[RFID] code=%llu bits=%u\n", (unsigned long long)code, bits);
    writeLatest(String((unsigned long long)code), "unknown", 1, -50);
    writeEvent("rfid", String((unsigned long long)code), "granted", "");
    ledBlink(2, 30, 30);

    DynamicJsonDocument root(256);
    root["command"] = "rfid";
    root["code"]    = String((unsigned long long)code);
    root["bits"]    = bits;
    String out; serializeJson(root, out);
    ws.textAll(out);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);

  if (!SPIFFS.begin(true)) {
    Serial.println(F("[SPIFFS] mount failed"));
  }

  setTime(12,0,0, 1,1,2025);

  wiegand.begin(PIN_WIEGAND_D0, PIN_WIEGAND_D1);

  setupWifi(false);
  setupWeb();
  setupMQTT();

  statusTicker.once_ms(500, [](){ wsBroadcastStatus(); });
}

void loop() {
  loopRFID();
  delay(2);
}