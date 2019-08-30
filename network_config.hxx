// Upate with WiFI and MQTT definitions
#define WIFI_SSID "xxxx"         
#define WIFI_PASSWORD "xxxx"

#define MQTT_SERVER "xxxx.xxxx.xxxx"  // server name or IP
#define MQTT_USER "xxxx"      // username 
#define MQTT_PASSWORD "xxxx"  // password
#define MQTT_PORT 8883              // MQTT port
#define OPEN_WEATHER_API_KEY "xxx"
#define LOCATION "Munich"
#define WEATHER_SERVER "api.openweathermap.org"

#define STORED_READING 6
#define READINGS_ARRAY  \
  {"Bth", "gBridge/xxxx", NO_READING, 0.0, {0.0}, CHAR_BLANK, NOT_ENOUGH_DATA, DATA_TEMPERATURE, 0}, \
  {"Bed", "gBridge/xxxx", NO_READING, 0.0, {0.0}, CHAR_BLANK, NOT_ENOUGH_DATA, DATA_HUMIDITY, 0}

  
#define SETTINGS_ARRAY \ 
  {DESC_BRIGHTNESS, "gBridge/xxx", "gBridge/xxx/set", 0.0, DATA_SETTING}, \
  {DESC_CONTRAST, "gBridge/xxxx", "gBridge/xxx/set", 0.0, DATA_SETTING}, \
  {DESC_ONOFF, "gBridge/xxx", "gBridge/xxx/set", 0.0, DATA_ONOFF}
