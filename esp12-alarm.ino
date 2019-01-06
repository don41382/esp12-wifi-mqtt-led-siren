#include <FS.h>
#include "Adafruit_WS2801.h"
#include "SPI.h" // Comment out this line if using Trinket or Gemma
#include <MQTT.h>

#include <ESP8266WiFi.h> 
#include <DNSServer.h>
#include <ESP8266WebServer.h> 
#include <WiFiManager.h>

#include <ArduinoJson.h>
#include <DoubleResetDetector.h>

#define SSID_START_NAME "Alarm"
#define DATA_PIN 2
#define CLOCK_PIN 3

char mqtt_server[40];
char mqtt_port[6] = "8080";
char mqtt_username[40];
char mqtt_password[40];

#define DRD_TIMEOUT 10
#define DRD_ADDRESS 0

DoubleResetDetector drd(DRD_TIMEOUT, DRD_ADDRESS);

bool shouldSaveConfig = false;

enum colors {
  OFF_COLOR,
  RED_COLOR,
  GREEN_COLOR,
  BLUE_COLOR
};

enum modes {
  NONE,
  SIREN,
  FLASH
};

struct AlarmMode {
  modes mode;
  colors color;
  uint8_t duration;
};

AlarmMode alarmMode = { NONE, RED_COLOR, 0 };

Adafruit_WS2801 strip = Adafruit_WS2801(24, DATA_PIN, CLOCK_PIN);
WiFiManager wifiManager;
WiFiClient net;
MQTTClient mqtt;

void saveConfigCallback () {
  Serial.println("Save config");
  shouldSaveConfig = true;
  drd.stop();
}

bool loadConfig() {
    if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");

          strcpy(mqtt_server, json["mqtt_server"]);
          strcpy(mqtt_port, json["mqtt_port"]);

          strcpy(mqtt_username, json["mqtt_username"]);
          strcpy(mqtt_password, json["mqtt_password"]);

        } else {
          Serial.println("failed to load json config");
        }
        configFile.close();
      }
      return true;
    }
  } else {
    Serial.println("failed to mount FS");
    return false;
  }
}

colors toColor(String c) {
  if (c.equalsIgnoreCase("red")) {
    return RED_COLOR;
  } else if (c.equalsIgnoreCase("green")) {
    return GREEN_COLOR;
  } else if (c.equalsIgnoreCase("blue")) {
    return BLUE_COLOR;
  } else {
    return OFF_COLOR;
  }
}

modes toModes(String m) {
  if (m.equalsIgnoreCase("siren")) {
    return SIREN;
  } else if (m.equalsIgnoreCase("flash")) {
    return FLASH;
  } else {
    return NONE;
  }
}

int toDuration(String d) {
  if (d == NULL) {
    return 5;
  } else {
    return d.toInt();
  }
}

void messageReceived(String &topic, String &payload) {
  Serial.println("incoming: " + topic + " - " + payload);

  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(payload.c_str());

  if (json.success()) {
    alarmMode =  (AlarmMode) { toModes(json["mode"]), toColor(json["color"]), toDuration(json["duration"])};
  } else {
    Serial.println("Invalid json. Please provide { 'color': 'red', 'mode': 'flash', 'duration': 1}");
  }
}

String deviceName() {
  String chipId = String(ESP.getChipId());
  return (SSID_START_NAME + String("-") + chipId.substring(chipId.length()-2));
}


void setup() {
  Serial.begin(115200);

  char dName[40];
  deviceName().toCharArray(dName,40);
  Serial.printf("## Booting up %s \n\r", dName);

  strip.begin();
  strip.show();

  if (loadConfig()) {
    Serial.printf("Loaded: MQTT %s[%s] - user: %s\n\r",mqtt_server,mqtt_port,mqtt_username);
  }

  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "mqtt port", mqtt_port, 6);
  WiFiManagerParameter custom_mqtt_username("user", "mqtt username", mqtt_username, 40);
  WiFiManagerParameter custom_mqtt_password("password", "mqtt password", mqtt_password, 40);

  WiFiManager wifiManager;

  wifiManager.setSaveConfigCallback(saveConfigCallback);  
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_username);
  wifiManager.addParameter(&custom_mqtt_password);

  if (drd.detectDoubleReset()) {
    Serial.println("Double Reset Detected");
    wifiManager.startConfigPortal(dName);
  } else {
    Serial.println("No Double Reset Detected");
    if (!wifiManager.autoConnect(dName)) {
      Serial.println("failed to connect and hit timeout");
      delay(3000);
      ESP.reset();
      delay(5000);
    }
  }
  
  if (shouldSaveConfig) {
    strcpy(mqtt_server, custom_mqtt_server.getValue());
    strcpy(mqtt_port, custom_mqtt_port.getValue());
    strcpy(mqtt_username, custom_mqtt_username.getValue());
    strcpy(mqtt_password, custom_mqtt_password.getValue());

    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["mqtt_server"] = mqtt_server;
    json["mqtt_port"] = mqtt_port;
    json["mqtt_username"] = mqtt_username;
    json["mqtt_password"] = mqtt_password;
  
    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }
  
    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
  }

  Serial.print("\nConnecting to MQTT ...");
  mqtt.begin(mqtt_server, net);
  mqtt.onMessage(messageReceived);

  for (int i=0; i++; i<5) { 
    if (mqtt.connect(dName, mqtt_username, mqtt_password))) {
      break;
    }
    Serial.print(".");
    delay(1000);
  }

  mqtt.subscribe(strcat("/",dName));

  Serial.println("Done!"); 

  for (int i=0; i<2; i++) { siren(GREEN_COLOR); }
  blank();
  drd.stop();
}

bool toggle = false;

uint32_t Color(colors ct, uint8_t b) {
  uint32_t c;
  c = (ct == BLUE_COLOR ? b : 0);
  c <<= 8;
  c |= (ct == GREEN_COLOR ? b : 0);
  c <<= 8;
  c |= (ct == RED_COLOR ? b : 0);
  return c;
}

uint8_t brightness(int c, int i) {
  if (((i+c) % 4) == 0) {
    return 0xFF;
  } else {
    return 0;
  }
}


void blank() {
  for (int i=0; i < 24; i++) {
    strip.setPixelColor(i,0);
  }
  strip.show();
}

void siren(colors color) {
  for (int c=0; c < 4; c++) {
    for (int i=0; i < strip.numPixels(); i++) {
      strip.setPixelColor(i,Color(color, brightness(c,i)));
    }
    strip.show();
    delay(70);
  }
}

void flash(colors color) {
  for (int b=-255; b < 255; b++) {
    for (int i=0; i < strip.numPixels(); i++) {
      strip.setPixelColor(i,Color(color, abs(b)));
    }
    strip.show();
    delay(2);
  }
}

long runUntil = -1;

void loop() {
  mqtt.loop();
  
  if (runUntil == -1 && alarmMode.mode != NONE) {
    runUntil = millis() + (alarmMode.duration * 1000);
  } else if (runUntil < millis()) {
    runUntil = -1;
    alarmMode.mode = NONE;
    blank();
  }

  switch (alarmMode.mode) {
    case NONE:
      delay(500);
      blank();
      break;
    case SIREN:
      siren(alarmMode.color);
      break;
    case FLASH:
      flash(alarmMode.color);
      break;
  }
}
