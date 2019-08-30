/*
   For I2C SDA PIN 21
           SLC PIN 22

  NOTE - Error in the ESP32 header file "client.h". Need to edit and comment out the rows
  virtual int connect(IPAddress ip, uint16_t port, int timeout) =0;
  virtual int connect(const char *host, uint16_t port, int timeout) =0;
*/

#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <ArduinoMqttClient.h>
#include <analogWrite.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <TimeLib.h>
#include <ArduinoJson.h>
#include "network_config.h"

struct Readings {                     // Array to hold the incoming measurement
  String description;                 // Currently set to 3 chars long
  String topic;                       // MQTT topic
  String output;                      // To be output to screen - expected to be 2 chars long
  float currentValue;                 // Current value received
  float lastValue[STORED_READING];    // Defined that the zeroth element is the oldest
  byte changeChar;                    // To indicate change in status
  byte enoughData;                    // to inidate is a full set of STORED_READING number of data points received
  int dataType;                       // Type of data received
  int readingIndex;                   // Index of current reading max will be STORED_READING
  unsigned long lastMessageTime;      // Millis this was last updated
};

struct Settings {                     // Structure to hold the cincomming settings and outgoing confirmations
  String description;                 // Currently set to 3 chars long
  String topic;                       // MQTT topic
  String confirmTopic;                // To confirm setting changes back to broker
  float currentValue;                 // Current value received
  int dataType;                       // Type of data received
};

struct LcdValues {
  bool on;
};

struct Weather {
  float temperature;
  int pressure;
  float humidity;
  String overal;
  String description;
  time_t updateTime;
};

struct LcdOutput {
  char line1[255];
  char line2[255];
  bool update;
};

// Array and LCD string settings
#define NO_READING "--"            // Screen output before any mesurement is received
#define DESC_ONOFF "ONO"

// LCD Character settings
#define CHAR_UP 0
#define CHAR_DOWN 1
#define CHAR_SAME 3
#define CHAR_STAR 42
#define CHAR_BLANK 32
#define CHAR_NO_MESSAGE 33
#define CHAR_NOT_ENOUGH_DATA 46
#define CHAR_ENOUGH_DATA CHAR_BLANK
#define LCD_COL 16
#define LCD_ROW 2

// Data type definition for array
#define DATA_TEMPERATURE 0
#define DATA_HUMIDITY 1
#define DATA_SETTING 2
#define DATA_ONOFF 3

// Define constants used
#define MAX_NO_MESSAGE_SEC 3600LL        // Time before CHAR_NO_MESSAGE is set in seconds (long) 
#define TIME_RETRIES 100                 // Number of time to retry getting the time during setup
#define TIME_OFFSET 7200                 // Local time offset from UTC
#define WEATHER_UPDATE_INTERVAL 60       // Interval between weather updates

Readings readings[] { READINGS_ARRAY };
Settings settings[] {SETTINGS_ARRAY };
LcdValues lcdValues;
Weather weather = {0.0, 0, 0.0, "", "", 0};
LcdOutput lcdOutput = {{CHAR_BLANK}, {CHAR_BLANK}, true};
#define LCD_VALUES_ADDRESS 0

WiFiClientSecure wifiClient;
WiFiClientSecure wifiClientWeather;
MqttClient mqttClient(wifiClient);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, TIME_OFFSET);

LiquidCrystal_I2C lcd(0x27, LCD_COL, LCD_ROW);

// Weather set up
String apiKey = OPEN_WEATHER_API_KEY;
char weather_server[] = WEATHER_SERVER;
String location = LOCATION;


void setup() {

  Serial.begin(115200);  // Default speed of esp32
  EEPROM.get(LCD_VALUES_ADDRESS, lcdValues);
  lcd_setup();
  network_connect();
  time_init();
  mqtt_connect();
  lcd_update_temp;
  update_mqtt_settings();

  xTaskCreatePinnedToCore( lcd_output_t, "LCD Update", 8192 , NULL, 2, NULL, 1 );
  xTaskCreatePinnedToCore( get_weather_t, "Get Weather", 8192 , NULL, 3, NULL, 1 );
}

void get_weather_t(void * pvParameters ) {

  while (true) {
    if (now() - weather.updateTime > WEATHER_UPDATE_INTERVAL) {
      if (wifiClientWeather.connect(weather_server, 443)) {
        wifiClientWeather.print("GET /data/2.5/weather?");
        wifiClientWeather.print("q=" + location);
        wifiClientWeather.print("&appid=" + apiKey);
        wifiClientWeather.print("&cnt=3");
        wifiClientWeather.println("&units=metric");
        wifiClientWeather.println("Host: api.openweathermap.org");
        wifiClientWeather.println("Connection: close");
        wifiClientWeather.println();
      } else {
        Serial.println("unable to connect to weather server");
      }
      delay(2000);
      String line = "";

      line = wifiClientWeather.readStringUntil('\n');
      if (line.length() != 0) {
        DynamicJsonDocument root(5000);
        auto deseraliseError = deserializeJson(root, line);
        String weatherTemperature = root["main"]["temp"];
        String weatherPressure = root["main"]["pressure"];
        String weatherHumidity = root["main"]["humidity"];
        String weatherOveral = root["weather"][0]["main"];
        String weatherDescription = root["weather"][0]["description"];

        weather.temperature = weatherTemperature.toFloat();
        weather.pressure = weatherPressure.toInt();
        weather.humidity = weatherHumidity.toInt();
        weather.overal = weatherOveral;
        weather.description = weatherDescription;
        weather.updateTime = now();

        Serial.println("Weather Updated");
      }
      wifiClientWeather.flush();
      wifiClientWeather.stop();
    }
    delay(2000);
  }
}

void network_connect() {

  String lcdWait[9] = {".    ", " .   ", "  .  ", "   . ", "    .", "   . ", "  .  ", " .   "};
  int lcdWaitCount = 0;

  Serial.print("Connect to WPA SSID: ");
  Serial.println(WIFI_SSID);

  lcd.setCursor(0, 0);
  lcd.print("Waiting for");
  lcd.setCursor(0, 1);
  lcd.print(WIFI_SSID);

  while (WiFi.begin(WIFI_SSID, WIFI_PASSWORD) != WL_CONNECTED) {
    Serial.print(".");
    lcd.setCursor(11, 0);
    lcd.print(lcdWait[lcdWaitCount]);
    lcdWaitCount++;
    if (lcdWaitCount > (sizeof(lcdWait) / sizeof(lcdWait[0])) - 1) {
      lcdWaitCount = 0;
    }
    delay(500);
  }
  lcd.setCursor(0, 0);
  lcd.print("Connected to:   ");
}

void time_init() {
  timeClient.begin();
  for (int i = 0; i < TIME_RETRIES; i++) {
    bool retcode;
    retcode = timeClient.forceUpdate();
    if (retcode == true) {
      break;
    }
    timeClient.begin();
  }
  setTime(timeClient.getEpochTime());
  Serial.println("Epoch time is: " + String(timeClient.getEpochTime()));
  Serial.println("Time is: " + String(timeClient.getFormattedTime()));

}

void mqtt_connect() {

  mqttClient.setUsernamePassword(MQTT_USER, MQTT_PASSWORD);
  Serial.println();
  Serial.print("Attempting to connect to the MQTT broker: ");
  Serial.println(MQTT_SERVER);

  lcd.setCursor(0, 0);
  lcd.print("Connecting to: ");
  lcd.setCursor(0, 1);
  lcd.print(MQTT_SERVER);

  while (!mqttClient.connect(MQTT_SERVER, MQTT_PORT)) {
    Serial.print("MQTT connection failed");
    lcd.setCursor(0, 0);
    lcd.print("Can't connect");
    delay(5000);
    ESP.restart();
  }

  Serial.println("Connected to the MQTT broker");

  for (int i = 0; i < sizeof(readings) / sizeof(readings[0]); i++) {
    mqttClient.subscribe(readings[i].topic);
    readings[i].lastMessageTime = millis();
  }

  for (int i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
  }

  lcd.setCursor(0, 0);
  lcd.print("Connected to: ");
  delay(1000);  // For the message on the LCD to be read


}

void lcd_setup() {

  //Sets up the special charaters in the LCD
  byte charUp[8] = {B00100, B01110, B10101, B00100, B00100, B00100, B00100, B00000};
  byte charDown[8] = {B00000, B00100, B00100, B00100, B00100, B10101, B01110, B00100};
  byte charSame[8] = {B00000, B00100, B00010, B11111, B00010, B00100, B00000, B00000};

  lcd.init();
  set_lcd_values();
  lcd.createChar(CHAR_UP, charUp);
  lcd.createChar(CHAR_DOWN, charDown);
  lcd.createChar(CHAR_SAME, charSame);
  lcd.backlight();

}

void lcd_update_temp() {

  String line1;
  String line2;

  line1 = String(readings[0].description) + ":" + String(readings[0].output) + "  " + String(readings[1].description) + ":" + String(readings[1].output)  + "  ";

  line2 = String(readings[2].description) + ":" + String(readings[2].output) +  + "  " + String(readings[3].description) + ":" + String(readings[3].output) +  + "  ";

  while (line1.length() <= LCD_COL) {
    line1 = line1 + " ";
  }
  while (line2.length() <= LCD_COL) {
    line2 = line2 + " ";
  }
  line1.toCharArray(lcdOutput.line1, LCD_COL);
  line2.toCharArray(lcdOutput.line2, LCD_COL);
  lcdOutput.line1[6] = (char)readings[0].changeChar;
  lcdOutput.line1[7] = (char)readings[0].enoughData;
  lcdOutput.line1[14] = (char)readings[1].changeChar;
  lcdOutput.line1[15] = (char)readings[1].enoughData;
  lcdOutput.line2[6] = (char)readings[2].changeChar;
  lcdOutput.line2[7] = (char)readings[2].enoughData;
  lcdOutput.line2[14] = (char)readings[3].changeChar;
  lcdOutput.line2[15] = (char)readings[3].enoughData;

}

void lcd_update_weather() {

  String line1;
  String line2;

  if (weather.updateTime == 0) {
    line1 = "Waiting for       ";
    line2 = "weather....       ";
  }
  else {

    line1 = "T:" + String(weather.temperature, 1) + " H:" + String(weather.humidity, 0);
    String firstLetter = weather.description.substring(0, 1);
    firstLetter.toUpperCase();
    line2 = firstLetter + weather.description.substring(1);
  }

  while (line1.length() <= LCD_COL + 1) {
    line1 = line1 + " ";
  }
  while (line2.length() <= LCD_COL + 1) {
    line2 = line2 + " ";
  }
  line1.toCharArray(lcdOutput.line1, LCD_COL + 1);
  line2.toCharArray(lcdOutput.line2, LCD_COL + 1);

}

void lcd_output_t(void * pvParameters ) {
  int line1Counter = 0;
  int line2Counter = 0;

  while (true) {
    lcd.setCursor(0, 0);
    for (int i = 0; i <= LCD_COL; i++) {
      lcd.write(lcdOutput.line1[i]);
    }
    lcd.setCursor(0, 1);
    for (int i = 0; i <= LCD_COL; i++) {
      lcd.write(lcdOutput.line2[i]);
    }

  }
}

void update_temperature(String recMessage, int index) {

  float averageHistory;
  float totalHistory = 0.0;

  readings[index].currentValue = String(recMessage).toFloat();
  readings[index].output = String((int)round(readings[index].currentValue));
  if (readings[index].readingIndex == 0) {
    readings[index].changeChar = CHAR_BLANK;  // First reading of this boot
    readings[index].lastValue[0] = readings[index].currentValue;
  }
  else
  {
    for (int i = 0; i < readings[index].readingIndex; i++) {
      totalHistory = totalHistory +  readings[index].lastValue[i];
    }
    averageHistory = totalHistory / readings[index].readingIndex;

    if (readings[index].currentValue > averageHistory) {
      readings[index].changeChar = CHAR_UP;
    }
    if (readings[index].currentValue < averageHistory) {
      readings[index].changeChar = CHAR_DOWN;
    }
    if (readings[index].currentValue == averageHistory) {
      readings[index].changeChar = CHAR_SAME;
    }

    if (readings[index].readingIndex == STORED_READING) {
      readings[index].readingIndex--;
      readings[index].enoughData = CHAR_ENOUGH_DATA;      // Set flag that we have all the readings
      for (int i = 0; i < STORED_READING - 1; i++) {
        readings[index].lastValue[i] = readings[index].lastValue[i + 1];
      }
    }
    else {
      readings[index].enoughData = CHAR_NOT_ENOUGH_DATA;
    }

    readings[index].lastValue[readings[index].readingIndex] = readings[index].currentValue; // update with latest value
  }

  readings[index].readingIndex++;
  readings[index].lastMessageTime = millis();
  // Force output length to be 2 chars by right padding
  if (readings[index].output.length() > 2) {
    readings[index].output[2] = 0;   //Truncate long message
  }
  if (readings[index].output.length() == 1) {
    readings[index].output = readings[index].output + " ";
  }
  if (readings[index].output.length() == 0) {
    readings[index].output = "  ";
  }
}

void set_lcd_values() {

  if (lcdValues.on == true) {
    lcd.backlight();
  }
  else {
    //lcd.NoBacklight();
  }

}

void update_mqtt_settings() {

  String topic;
  String message;

  for (int i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
    if (settings[i].description == DESC_ONOFF) {
      mqttClient.beginMessage(settings[i].confirmTopic);
      mqttClient.print(String(lcdValues.on));
      mqttClient.endMessage();
    }
  }
}


void update_on_off(String topic, String recMessage, int index) {

  settings[index].currentValue = String(recMessage).toFloat();
  if (recMessage == "1") {
    lcdValues.on = true;
  }
  if (recMessage == "0") {
    lcdValues.on = false;
  }
  set_lcd_values();
  update_mqtt_settings();
  // Store for reboot
  EEPROM.put(LCD_VALUES_ADDRESS, lcdValues);
}

void receive_mqtt_messages() {
  int messageSize = mqttClient.parseMessage();
  String topic;
  char recMessage[255] = {0};
  int index;
  bool readingMessageReceived;


  if (messageSize) {   //Message received
    topic = String(mqttClient.messageTopic());
    mqttClient.read((unsigned char*)recMessage, (size_t)sizeof(recMessage)); //Distructive read of message
    Serial.println("Topic: " + topic + " Msg: " + recMessage);
    readingMessageReceived = false;               // To check if non reading message
    for (int i = 0; i < sizeof(readings) / sizeof(readings[0]); i++) {
      if (topic == readings[i].topic) {
        index = i;
        if (readings[i].dataType == DATA_TEMPERATURE) {
          update_temperature(recMessage, index);
        }
        if (readings[i].dataType == DATA_HUMIDITY) {
          //update_temperature(recMessage, index);
        }
      }
    }
    for (int i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
      if (topic == settings[i].topic) {
        index = i;
        if (settings[i].dataType == DATA_ONOFF) {
          update_on_off(topic, recMessage, index);
        }
      }
    }

  }
}

void loop() {
  timeClient.update();

  for (int i = 0; i < sizeof(readings) / sizeof(readings[0]); i++) {
    if ((millis() > readings[i].lastMessageTime + (MAX_NO_MESSAGE_SEC * 1000)) && (readings[i].output != NO_READING)) {
      readings[i].changeChar = CHAR_NO_MESSAGE;
    }
  }

  // Temp code to swtich display
  if ((int)round(second() / 5) % 2 == 0) {
    lcd_update_temp();
  }
  else
  {
    lcd_update_weather();
  }

  if (WiFi.status() != WL_CONNECTED) {
    network_connect();
  }
  if (!mqttClient.connected()) {
    Serial.println("MQTT error detected");
    mqtt_connect();
  }
  receive_mqtt_messages();

}