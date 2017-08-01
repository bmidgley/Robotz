#include <FS.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <Servo.h>

#define TRIGGER_PIN 0

bool shouldSaveConfig = false;
char name[40] = "Robotz";
ESP8266WebServer *webServer;
long lastLoop = 0;
bool activate = false;
Servo myservo;

void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

// use arduino library manager to get libraries
// sketch->include library->manage libraries
// WiFiManager, ArduinoJson, ArduinoOTA

// connect servo to GND, 3v, D9/GPIO16

void setup() {
  WiFiManager wifiManager;
  
  Serial.begin(9600);
  Serial.println("\n Starting");
  pinMode(TRIGGER_PIN, INPUT);
  WiFi.printDiag(Serial);
  
  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      Serial.println("found /config.json");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("reading /config.json");
        size_t size = configFile.size();
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");
          if(json["name"]) strcpy(name, json["name"]);
        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }

  Serial.println("loaded config");
  
  WiFiManagerParameter custom_name("Name", "name", name, 40);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.addParameter(&custom_name);
  wifiManager.autoConnect(name);
  
  Serial.println("stored wifi connected");

  strcpy(name, custom_name.getValue());
  
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["name"] = name;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    json.printTo(Serial);
    Serial.println("");
    json.printTo(configFile);
    configFile.close();
  }
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();

  MDNS.begin(name);
  MDNS.addService("http", "tcp", 80);
  webServer = new ESP8266WebServer(80);
  webServer->onNotFound([]() {
    webServer->send(404, "text/plain", "File not found");
  });
  webServer->on("/", []() {
    webServer->send(200, "text/html", "<html><a href=\"/\"><img src=\"http://flamebot.com/fly.png\"/></a></html>");
    activate = true;
  });
  webServer->begin();

  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
}

void loop() {
  int sdelay = 4;
  int pos;
  
  ArduinoOTA.handle();
  webServer->handleClient();

  if ( digitalRead(TRIGGER_PIN) == LOW ) {
    activate = true;
  }

  if(activate) {
    Serial.println("activate");
    myservo.attach(16);
    for (pos = 0; pos <= 90; pos += 1) {
      // in steps of 1 degree
      myservo.write(pos);
      delay(sdelay);
    }
    for (pos = 90; pos >= 0; pos -= 1) {
      myservo.write(pos);
      delay(sdelay);
    }
    myservo.detach();
    Serial.print(".");
    activate = false;
  }

  long now = millis();
  if (now - lastLoop < 1000 * 60 * 5) {
    return;
  }
  lastLoop = now;

  activate = true;
}
