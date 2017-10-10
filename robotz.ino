#include <FS.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include "SSD1306.h"
#include "ESP8266TrueRandom.h"
#include <ArduinoJson.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <Servo.h>

// use arduino library manager to get libraries
// sketch->include library->manage libraries
// WiFiManager, ArduinoJson, PubSubClient, ArduinoOTA, SimpleDHT, "ESP8266 and ESP32 Oled Driver for SSD1306 display"
// wget https://github.com/marvinroger/ESP8266TrueRandom/archive/master.zip
// sketch->include library->Add .zip Library

#define TRIGGER_PIN 0
#define ACTIVATE_MAX 8

int angle1 = 20;
int angle2 = 55;
bool shouldSaveConfig = false;
long lastMsg = 0;
long lastReading = 0;
long lastSwap = 0;
char msg[200];
char errorMsg[200];
int reconfigure_counter = 0;
int activate = ACTIVATE_MAX;
Servo myservo;

char name[20] = "Robot1";
char mqtt_server[20] = "mqtt.geothunk.com";
char mqtt_port[6] = "8080";
char uuid[64] = "";
char ota_password[10] = "012345678";
char *version = "1.0";
int sdelay = 4;
int pos;

int reportGap = 5;

WiFiClientSecure *tcpClient;
PubSubClient *client;
ESP8266WebServer *webServer;
SSD1306 display(0x3c,5,4);

const char* serverIndex = "<html><a href=\"/\"><img src=\"http://flamebot.com/fly.png\"/></a></html>";

t_httpUpdate_return update() {
  return ESPhttpUpdate.update("http://updates.geothunk.com/updates/robotz.ino.bin");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("Message arrived [%s]\n", topic);
  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(payload);
  json.printTo(Serial);

  if(json["activate"] && json["activate"] > activate) {
    Serial.printf("\nupdating activate from bus\n");
    activate = json["activate"];
  }
}

int mqttConnect() {
  if (client->connected()) return 1;

  Serial.print("Attempting MQTT connection...");
  if (client->connect(uuid)) {
    Serial.println("connected");
    client->subscribe("+/robots");
    return 1;
  } else {
    Serial.print("failed, rc=");
    Serial.println(client->state());
    return 0;
  }
}

void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void setup() {
  WiFiManager wifiManager;
  bool create_ota_password = true;
  byte uuidNumber[16];
  byte uuidCode[16];
  
  Serial.begin(9600);
  Serial.println("\n Starting");
  pinMode(TRIGGER_PIN, INPUT);
  WiFi.printDiag(Serial);
  
  display.init();
  display.setContrast(255);
  display.clear();
  
  ESP8266TrueRandom.uuid(uuidCode);
  ESP8266TrueRandom.uuidToString(uuidCode).toCharArray(ota_password, 7);
  ota_password[6] = '\0';

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

        // manually parse name just in case it doesn't work below
        char *nameStart = buf.get() + 6;
        char *nameEnd = strchr(nameStart, '"');
        strncpy(name, nameStart, nameEnd - nameStart);
        name[nameEnd - nameStart] = '\0';

        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");

          if(json["n"]) strcpy(name, json["n"]);
          if(json["mqtt_server"]) strcpy(mqtt_server, json["mqtt_server"]);
          if(json["mqtt_port"]) strcpy(mqtt_port, json["mqtt_port"]);
          if(json["uuid"]) strcpy(uuid, json["uuid"]);
          if(json["ota_password"]) {
            strcpy(ota_password, json["ota_password"]);
            ota_password[6] = '\0';
            create_ota_password = false;
          }

          printf("name=%s mqtt_server=%s mqtt_port=%s uuid=%s ota_password=%s\n", name, mqtt_server, mqtt_port, uuid, ota_password);

        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }

  Serial.println("loaded config");

  WiFiManagerParameter custom_name("name", "robot name", name, 64);
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 6);
  WiFiManagerParameter custom_ota_password("ota_password", "OTA password (optional)", ota_password, 6);

  wifiManager.setSaveConfigCallback(saveConfigCallback);

  wifiManager.addParameter(&custom_name);
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  if(create_ota_password) {
    Serial.println("generating ota_password");
    wifiManager.addParameter(&custom_ota_password);
    saveConfigCallback();
  }

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
  display.setFont(ArialMT_Plain_10);
  if(WiFi.SSID() && WiFi.SSID() != "") {
    String status("Connecting to ");
    status.concat(WiFi.SSID());
    status.concat(" or...");
    display.drawString(DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2 - 24, status);
  }
  display.drawString(DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2 - 14, String("Connect to this wifi"));
  display.setFont(ArialMT_Plain_16);
  display.drawString(DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2, String(name));
  display.display();
  
  wifiManager.autoConnect(name);
  Serial.println("stored wifi connected");

  strcpy(name, custom_name.getValue());
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  if(uuid == NULL || *uuid == 0) {
    Serial.println("generating uuid");
    ESP8266TrueRandom.uuid(uuidNumber);
    ESP8266TrueRandom.uuidToString(uuidNumber).toCharArray(uuid, 8);
    saveConfigCallback();
  }

  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["n"] = name;
    json["mqtt_server"] = mqtt_server;
    json["mqtt_port"] = mqtt_port;
    json["uuid"] = uuid;
    json["ota_password"] = ota_password;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    json.printTo(Serial);
    Serial.println("");
    json.printTo(configFile);
    configFile.close();
  }

  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
  display.setFont(ArialMT_Plain_10);
  display.drawString(DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2 - 5, String("Connecting to Server"));
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, DISPLAY_HEIGHT - 10, WiFi.localIP().toString());
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(DISPLAY_WIDTH, DISPLAY_HEIGHT - 10, String(WiFi.SSID()));
  display.display();

  client = new PubSubClient(*(new WiFiClient()));
  client->setServer(mqtt_server, strtoul(mqtt_port, NULL, 10));
  client->setCallback(mqttCallback);

  tcpClient = new WiFiClientSecure();

  Serial.printf("ota_password is %s\n", ota_password);

  ArduinoOTA.setPassword(ota_password);
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
  webServer->on("/", HTTP_GET, [](){
    activate = ACTIVATE_MAX;
    webServer->sendHeader("Connection", "close");
    webServer->sendHeader("Access-Control-Allow-Origin", "*");
    webServer->send(200, "text/html", serverIndex);
  });
  webServer->on("/update", HTTP_POST, [](){
    webServer->sendHeader("Connection", "close");
    webServer->sendHeader("Access-Control-Allow-Origin", "*");
    webServer->send(200, "text/plain", String(update()));
  });
  webServer->begin();

  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
}

void paint_display(long now, byte temperature, byte humidity) {
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.setFont(ArialMT_Plain_24);
  display.drawString(DISPLAY_WIDTH, 0, String(name));
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  if(now < 24 * 60 * 60 * 1000)
    display.drawString(0, 0, String(now / (60 * 60 * 1000)) + String("h ") + String(version));
  else
    display.drawString(0, 0, String(now / (24 * 60 * 60 * 1000)) + String("d ") + String(version));
  display.drawString(0, DISPLAY_HEIGHT - 10, WiFi.localIP().toString());
  display.drawString(0, DISPLAY_HEIGHT - 20, String(WiFi.SSID()));
  display.display();
}

void loop() {
  int index = 0;
  char value;
  char previousValue;
  char topic_name[90];

  *errorMsg = 0;
  *msg = 0;

  ArduinoOTA.handle();
  webServer->handleClient();
  client->loop();

  long now = millis();

  if ( digitalRead(TRIGGER_PIN) == LOW ) {
    activate = ACTIVATE_MAX;
  } else if (now - lastMsg < reportGap * 1000) {
    return;
  }
  lastMsg = now;

  while(activate > 0) {
    
    display.clear();
    display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
    display.setFont(ArialMT_Plain_24);
    display.drawString(DISPLAY_WIDTH/2, 15, String("Hello! from!"));
    display.drawString(DISPLAY_WIDTH/2, 45, String(name));
    display.display();
  
    myservo.attach(16);
    for (pos = angle1; pos <= angle2; pos += 1) {
      // in steps of 1 degree
      myservo.write(pos);
      delay(activate);
    }
    for (pos = angle2; pos >= angle1; pos -= 1) {
      myservo.write(pos);
      delay(activate);
    }

    activate >>= 1;
    snprintf(msg, 200, "{\"name\":\"%s\",\"activate\":%d}", name, activate);

    if (mqttConnect()) {
      if (*msg) {
        snprintf(topic_name, 90, "%s/robots", uuid);
        Serial.printf("%s %s\n", topic_name, msg);
        client->publish(topic_name, msg);
      }
      if (*errorMsg) {
        snprintf(topic_name, 90, "%s/robots/errors", uuid);
        Serial.printf("%s %s\n", topic_name, msg);
        client->publish(topic_name, errorMsg);
      }
    }
  }
  
  time_t clocktime = time(nullptr);
  Serial.println(ctime(&clocktime));

  if ( digitalRead(TRIGGER_PIN) == LOW ) {
    if(reconfigure_counter > 0) {
      display.clear();
      display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
      display.setFont(ArialMT_Plain_10);
      display.drawString(DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2 - 10, String("Hold to clear settings"));
      display.drawString(DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2, String(3-reconfigure_counter));
      display.display();
    }

    reconfigure_counter++;
    if(reconfigure_counter > 2) {
      Serial.println("disconnecting from wifi to reconfigure");
      WiFi.disconnect(true);

      display.clear();
      display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
      display.setFont(ArialMT_Plain_10);
      display.drawString(DISPLAY_WIDTH/2, DISPLAY_HEIGHT/2 - 10, String("Release and tap reset"));
      display.display();
    }
    return;
  } else {
    reconfigure_counter = 0;
  }

  paint_display(now, 0, 0);
}
