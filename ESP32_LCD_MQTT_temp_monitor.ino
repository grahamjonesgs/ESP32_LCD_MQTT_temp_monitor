
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
//#include <analogWrite.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <TimeLib.h>
#include <ArduinoJson.h>
#include "network_config.h"

#define CHAR_LEN 255

struct Readings {                     // Array to hold the incoming measurement
  const char description[CHAR_LEN];   // Currently set to 3 chars long
  const char topic[CHAR_LEN];         // MQTT topic
  char output[CHAR_LEN];                      // To be output to screen - expected to be 2 chars long
  float currentValue;                 // Current value received
  float lastValue[STORED_READING];    // Defined that the zeroth element is the oldest
  byte changeChar;                    // To indicate change in status
  byte enoughData;                    // to inidate is a full set of STORED_READING number of data points received
  int dataType;                       // Type of data received
  int readingIndex;                   // Index of current reading max will be STORED_READING
  unsigned long lastMessageTime;      // Millis this was last updated
};

struct Settings {                     // Structure to hold the cincomming settings and outgoing confirmations
  const char description[CHAR_LEN];   // Currently set to 3 chars long
  const char topic[CHAR_LEN];         // MQTT topic
  const char confirmTopic[CHAR_LEN];      // To confirm setting changes back to broker
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
  char overal[CHAR_LEN];
  char description[CHAR_LEN];
  time_t updateTime;
};

struct LcdOutput {
  char line1[CHAR_LEN];
  char line2[CHAR_LEN];
  int outputType;
  bool updated;
};

// Array and LCD string settings
#define NO_READING "--"            // Screen output before any mesurement is received
#define DESC_ONOFF "ONO"
#define OUTPUT_TEMPERATURE 1
#define OUTPUT_WEATHER 2

// LCD Character settings
#define CHAR_UP 1
#define CHAR_DOWN 2
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

// Global Variables
Readings readings[] { READINGS_ARRAY };
Settings settings[] {SETTINGS_ARRAY };
LcdValues lcdValues;
Weather weather = {0.0, 0, 0.0, "", "", 0};
LcdOutput lcdOutput = {{CHAR_BLANK}, {CHAR_BLANK}, true};
bool touch_light = false;
WiFiClientSecure wifiClient;
WiFiClient wifiClientWeather;
MqttClient mqttClient(wifiClient);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, TIME_OFFSET);

// Define eprom address
#define LCD_VALUES_ADDRESS 0

// for touch
#define TOUCH_ARRAY_SIZE 100
#define SENSITIVITY 4  // lower more sensitive
#define TOUCH_LOOPS_NEEDED 3  // number of touched loops to turn on 
#define TOUCH_LIGHT_DELAY 10L

// Monitor Heap size for fragmentation
size_t old_biggest_free_block = 0;

void setup() {

  Serial.begin(115200);  // Default speed of esp32
  //EEPROM.get(LCD_VALUES_ADDRESS, lcdValues);
  xTaskCreatePinnedToCore( lcd_output_t, "LCD Update", 8192 , NULL, 0, NULL, 1 ); // Highest priorit on this cpu to avoid coms errors
  delay(3000);
  welcome_message();
  network_connect();
  time_init();
  mqtt_connect();
  lcd_update_temp;

  xTaskCreatePinnedToCore( get_weather_t, "Get Weather", 8192 , NULL, 3, NULL, 0 );
  xTaskCreatePinnedToCore( touch_check_t, "Touch", 8192 , NULL, 4, NULL, 0 );
  xTaskCreatePinnedToCore( receive_mqtt_messages_t, "mqtt", 8192 , NULL, 1, NULL, 1 );
}

void welcome_message() {

  strncpy(lcdOutput.line1, "Welcome to the", LCD_COL);
  strncpy(lcdOutput.line2, "Klauss-o-meter", LCD_COL);

  delay(3000);
}

void touch_check_t(void * pvParameters) {
  long touch_total;
  int touch_loop_max;
  float touch_average;
  int touch_sensor_value = 0;
  int loops_touched = 0;
  int touch_array[TOUCH_ARRAY_SIZE];
  int touch_counter = 0;
  bool touch_looped = false;
  long touch_light_pressed = 0;

  while (true) {

    delay(1000);
    touch_sensor_value = touchRead(T0);
    touch_array[touch_counter] = touch_sensor_value;
    touch_counter++;

    if (touch_counter > TOUCH_ARRAY_SIZE - 1) {
      touch_counter = 0;
      touch_looped = true;
    }
    if (touch_looped) {
      touch_loop_max = TOUCH_ARRAY_SIZE;
    }
    else
    {
      touch_loop_max = touch_counter;
    }
    touch_total = 0;
    for (int i = 0; i < touch_loop_max; i++) {
      touch_total = touch_total + touch_array[i];
    }
    touch_average = (float)touch_total / (float)touch_loop_max;

    if (touch_sensor_value < touch_average - SENSITIVITY ) {
      loops_touched++;
    }
    else
    {
      loops_touched = 0;
    }
    if (loops_touched >= TOUCH_LOOPS_NEEDED ) {
      touch_light_pressed = now();
      touch_light = true;
    }
    else
    {
      if (touch_light_pressed + TOUCH_LIGHT_DELAY < now()) {
        touch_light = false;
      }
    }
  }
}

void get_weather_t(void * pvParameters ) {

  const char apiKey[] = OPEN_WEATHER_API_KEY;
  const char weather_server[] = WEATHER_SERVER;
  const char location[] = LOCATION;

  while (true) {
    delay(2000);
    if (now() - weather.updateTime > WEATHER_UPDATE_INTERVAL) {
      if (wifiClientWeather.connect(weather_server, 80)) {
        wifiClientWeather.print(F("GET /data/2.5/weather?"));
        wifiClientWeather.print(F("q="));
        wifiClientWeather.print(location);
        wifiClientWeather.print(F("&appid="));
        wifiClientWeather.print(apiKey);
        wifiClientWeather.print(F("&cnt=3"));
        wifiClientWeather.println(F("&units=metric"));
        wifiClientWeather.println(F("Host: api.openweathermap.org"));
        wifiClientWeather.println(F("Connection: close"));
        wifiClientWeather.println();
      } else {
        Serial.println(F("unable to connect to weather server"));
      }
      delay(2000);
      String line = "";

      line = wifiClientWeather.readStringUntil('\n');
      if (line.length() != 0) {
        DynamicJsonDocument root(1000);
        auto deseraliseError = deserializeJson(root, line);
        float weatherTemperature = root["main"]["temp"];
        int weatherPressure = root["main"]["pressure"];
        int weatherHumidity = root["main"]["humidity"];
        const char* weatherOveral = root["weather"][0]["main"];
        const char* weatherDescription = root["weather"][0]["description"];

        weather.temperature = weatherTemperature;
        weather.pressure = weatherPressure;
        weather.humidity = weatherHumidity;
        strncpy(weather.description, weatherDescription, CHAR_LEN);
        strncpy(weather.overal, weatherOveral, CHAR_LEN);
        weather.updateTime = now();
        Serial.println("Weather Updated");
      }
      wifiClientWeather.flush();
      wifiClientWeather.stop();
    }

  }
}

void network_connect() {

  const char lcdWait[8][6] = {".    ", " .   ", "  .  ", "   . ", "    .", "   . ", "  .  ", " .   "};
  int lcdWaitCount = 0;

  Serial.print("Connect to WPA SSID: ");
  Serial.println(WIFI_SSID);
  strncpy(lcdOutput.line1, "Waiting for", LCD_COL);
  strncpy(lcdOutput.line2, WIFI_SSID, LCD_COL);

  while (WiFi.begin(WIFI_SSID, WIFI_PASSWORD) != WL_CONNECTED) {
    Serial.print(".");
    strncpy(lcdOutput.line1, "Waiting for", LCD_COL);
    strncat(lcdOutput.line1, lcdWait[lcdWaitCount], 5);

    lcdWaitCount++;
    if (lcdWaitCount > (sizeof(lcdWait) / sizeof(lcdWait[0])) - 1) {
      lcdWaitCount = 0;
    }
    delay(500);
  }
  strncpy(lcdOutput.line1, "Connected to:   ", LCD_COL);
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


  Serial.println(F("Epoch time is: "));
  Serial.println(timeClient.getEpochTime());
  Serial.print(F("Time is: "));
  Serial.println(timeClient.getFormattedTime());
}

void mqtt_connect() {

  mqttClient.setUsernamePassword(MQTT_USER, MQTT_PASSWORD);
  Serial.println();
  Serial.print("Attempting to connect to the MQTT broker: ");
  Serial.println(MQTT_SERVER);

  strncpy(lcdOutput.line1, "Connecting to: ", LCD_COL);
  strncpy(lcdOutput.line2, MQTT_SERVER, LCD_COL);

  while (!mqttClient.connect(MQTT_SERVER, MQTT_PORT)) {
    Serial.print("MQTT connection failed");
    strncpy(lcdOutput.line2, "Can't connect:", LCD_COL);
    delay(5000);
    ESP.restart();
  }

  Serial.println("Connected to the MQTT broker");

  for (int i = 0; i < sizeof(readings) / sizeof(readings[0]); i++) {
    mqttClient.subscribe(readings[i].topic);
    readings[i].lastMessageTime = millis();
  }
  for (int i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
    mqttClient.subscribe(settings[i].topic);
  }
  //strncpy(lcdOutput.line1, "Connected to:  ", LCD_COL);
  delay(1000);  // For the message on the LCD to be read
}


void lcd_update_temp() {

  strncpy(lcdOutput.line1, readings[0].description , LCD_COL);
  strncat(lcdOutput.line1, ":" , LCD_COL);
  strncat(lcdOutput.line1, readings[0].output , LCD_COL);
  strncat(lcdOutput.line1, "  " , LCD_COL);
  strncat(lcdOutput.line1, readings[1].description , LCD_COL);
  strncat(lcdOutput.line1, ":" , LCD_COL);
  strncat(lcdOutput.line1, readings[1].output , LCD_COL);
  strncat(lcdOutput.line1, "  " , LCD_COL);

  strncpy(lcdOutput.line2, readings[2].description , LCD_COL);
  strncat(lcdOutput.line2, ":" , LCD_COL);
  strncat(lcdOutput.line2, readings[2].output , LCD_COL);
  strncat(lcdOutput.line2, "  " , LCD_COL);
  strncat(lcdOutput.line2, readings[3].description , LCD_COL);
  strncat(lcdOutput.line2, ":" , LCD_COL);
  strncat(lcdOutput.line2, readings[3].output , LCD_COL);
  strncat(lcdOutput.line2, "  " , LCD_COL);

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

  if (weather.updateTime == 0) {
    strncpy(lcdOutput.line1, "Waiting for       " , LCD_COL);
    strncpy(lcdOutput.line2, "weather....       " , LCD_COL);
  }
  else
  {
    sprintf(lcdOutput.line1, "T:%2.1f H:%2.0f" , weather.temperature, weather.humidity );
    strncpy(lcdOutput.line2, weather.description , LCD_COL);

  }
  while (strlen(lcdOutput.line1) <= LCD_COL) {
    strcat(lcdOutput.line1, " ");
  }
  while (strlen(lcdOutput.line2) <= LCD_COL) {
    strcat(lcdOutput.line2, " ");
  }

}

void lcd_output_t(void * pvParameters ) {
  int line1Counter = 0;
  int line2Counter = 0;

  LiquidCrystal_I2C lcd(0x27, LCD_COL, LCD_ROW);
  byte charUp[8] = {B00100, B01110, B10101, B00100, B00100, B00100, B00100, B00000};
  byte charDown[8] = {B00000, B00100, B00100, B00100, B00100, B10101, B01110, B00100};
  byte charSame[8] = {B00000, B00100, B00010, B11111, B00010, B00100, B00000, B00000};

  lcd.init();
  lcd.createChar(CHAR_UP, charUp);
  lcd.createChar(CHAR_DOWN, charDown);
  lcd.createChar(CHAR_SAME, charSame);
  lcd.backlight();
  strncpy(lcdOutput.line1, "                 ", LCD_COL);
  strncpy(lcdOutput.line1, "                 ", LCD_COL);
  lcdValues.on = true;

  while (true) {
    delay(10);
    if (lcdOutput.updated) {
      lcdOutput.updated = false;
      lcd.init();
    }

    if (lcdValues.on || touch_light) {
      lcd.backlight();
    }
    else {
      lcd.noBacklight();
    }
    lcd.setCursor(0, 0);
    for (int i = 0; i <= LCD_COL; i++) {
      if (lcdOutput.line1[i] == 0 ) {
        lcd.write(' ');
      }
      else
      {
        lcd.write(lcdOutput.line1[i]);
      }
    }
    lcd.setCursor(0, 1);
    for (int i = 0; i <= LCD_COL; i++) {
      if (lcdOutput.line2[i] == 0 ) {
        lcd.write(' ');
      }
      else
      {
        lcd.write(lcdOutput.line2[i]);
      }
    }
  }
}

void update_temperature(char* recMessage, int index) {

  float averageHistory;
  float totalHistory = 0.0;

  readings[index].currentValue = atof(recMessage);
  sprintf(readings[index].output, "%2.0f", readings[index].currentValue);

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
  /*if (readings[index].output.length() > 2) {
    readings[index].output[2] = 0;   //Truncate long message
    }
    if (readings[index].output.length() == 1) {
    readings[index].output = readings[index].output + " ";
    }
    if (readings[index].output.length() == 0) {
    readings[index].output = "  ";
    }*/
}

void update_mqtt_settings() {

  for (int i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
    if (settings[i].description == DESC_ONOFF) {
      mqttClient.beginMessage(settings[i].confirmTopic);

      mqttClient.print(lcdValues.on);
      mqttClient.endMessage();
    }
  }
}

void update_on_off(char* recMessage, int index) {

  if (strcmp(recMessage, "1") == 0) {
    lcdValues.on = true;
    settings[index].currentValue = 1;
  }
  if (strcmp(recMessage, "0") == 0) {
    lcdValues.on = false;
    settings[index].currentValue = 0;
  }
  update_mqtt_settings();
  // Store for reboot
  //EEPROM.put(LCD_VALUES_ADDRESS, lcdValues);
}

void receive_mqtt_messages_t(void * pvParams) {
  int messageSize = 0;
  String topic;
  char recMessage[CHAR_LEN] = {0};
  int index;
  bool readingMessageReceived;

  while (true) {
    delay(10);
    if (!mqttClient.connected()) {
      Serial.println("MQTT error detected");
      mqtt_connect();
    }

    messageSize = mqttClient.parseMessage();
    if (messageSize) {   //Message received
      topic = mqttClient.messageTopic();
      mqttClient.read((unsigned char*)recMessage, (size_t)sizeof(recMessage)); //Distructive read of message
      recMessage[messageSize] = 0;
      Serial.println("Topic: " + String(topic) + " Msg: " + recMessage);
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
            update_on_off(recMessage, index);
          }
        }
      }
    }
  }
}

void loop() {

  size_t free_heap = 0;
  size_t new_biggest_free_block = 0;

  delay(500);
  timeClient.update();

  free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  new_biggest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (old_biggest_free_block != new_biggest_free_block) {
    old_biggest_free_block = new_biggest_free_block;
    Serial.print(F("Heap data: "));
    Serial.print(free_heap);
    Serial.print(F(" largest free block "));
    Serial.println(new_biggest_free_block);
  }
  for (int i = 0; i < sizeof(readings) / sizeof(readings[0]); i++) {
    if ((millis() > readings[i].lastMessageTime + (MAX_NO_MESSAGE_SEC * 1000)) && (readings[i].output != NO_READING)) {
      readings[i].changeChar = CHAR_NO_MESSAGE;
    }
  }
  if ((int)round(second() / 5) % 2 == 0 || touch_light) {
    if (lcdOutput.outputType != OUTPUT_TEMPERATURE) {
      lcdOutput.outputType = OUTPUT_TEMPERATURE;
      lcdOutput.updated=true;
    }
    
    lcd_update_temp();
  }
  else
  {
    if (lcdOutput.outputType != OUTPUT_WEATHER) {
      lcdOutput.outputType = OUTPUT_WEATHER;
      lcdOutput.updated=true;
    }
    lcd_update_weather();
  }

  if (WiFi.status() != WL_CONNECTED) {
    network_connect();
  }
}
