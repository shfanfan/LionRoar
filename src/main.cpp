#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>

#pragma region defines

#if DEBUG_MODE
  #define DEBUG_PRINT(x) Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_PRETTY(x,y) serializeJsonPretty(x, y);
  #define DEBUG_PRINTF(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
  #define DEBUG_PRINTF(...)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINT(x)
  #define DEBUG_PRETTY(x,y)
#endif

#ifndef DEVICE_HOSTNAME
#define DEVICE_HOSTNAME "lion-roar_test" // Fallback just in case
#endif

#ifdef LOCALHOST_TESTING
    const char *SERVER_URL = "http://192.168.1.218:3000/settings"; // <--- נקודת קצה מקומית לצורך בדיקות
#else
    const char *SERVER_URL = "https://www.oref.org.il/warningMessages/alert/History/AlertsHistory.json";
#endif

#ifndef LED_PIN
#define LED_PIN 48
#endif
#ifndef NUM_LEDS
#define NUM_LEDS 15
#endif

#pragma endregion defines


const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;     // Replace with your timezone offset in seconds (e.g., -18000 for EST)
const int daylightOffset_sec = 0; // Set to 3600 if your timezone observes daylight saving time

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

#pragma region settings //settings variables(updated from config.json):
//
String wifiSsid = "empty ssid";
String wifiPassword = "wrong password";
String area = "הוגוורטס";
String exactArea = "\"\"";
bool useBuzzer = false;
bool useVibrations = false;
uint32_t colorUnconnected;
uint32_t colorUnconnectedDim;
uint32_t colorNoAlerts;
uint32_t colorWarning;
uint32_t colorAlertRockets;
uint32_t colorAlertGeneral;
uint32_t colorEventEnded;
uint32_t colorEventEndedDim;

#pragma endregion 

unsigned long API_CHECK_INTERVAL = 5000;

//state machine states:
enum AlertState
{
  STATE_UNCONNECTED=    0,
  STATE_ALERT_ROCKETS=  1,
  STATE_ALERT_AIRCRAFT=  2, 
  STATE_EVENT_ENDED=    13, 
  STATE_WARNING =       14,      
  STATE_NO_ALERTS =     100
};


//TODO: add additional scenarios
//3 חדירת מחבלים	אירוע ביטחוני ביישוב
//4 רעידת אדמה	התראה לצאת לשטח פתוח
//5 צונאמי	רלוונטי ליישובי החוף
//6  אירוע חומרים מסוכנים דליפה של חומ"ס
//10 בדיקה / תרגיל	מופעל בזמן תרגילים של פיקוד העורף


int networkErrorCount = 0; // מונה שגיאות רשת ברצף
int MajorNetworkErrorRecoveryCount = 0; // סף לשגיאה חמורה שתוביל לריסט
AlertState currentState = STATE_UNCONNECTED;
unsigned long eventStartTime = 0; // טיימר למצב סיום אירוע


String stateToStr(AlertState state) {
  switch (state) {
    case STATE_UNCONNECTED:
      return "Unconnected - Waiting for data";
      
    case STATE_NO_ALERTS:
      return "Routine - No active alerts";
      
    case STATE_WARNING:
      return "Warning - Incoming alerts expected";
      
    case STATE_ALERT_ROCKETS:
      return "Rocket & Missile Alert!";
      
    case STATE_ALERT_AIRCRAFT:
      return "Aircraft Invasion Alert!";
      
    case STATE_EVENT_ENDED:
      return "Event Ended";

    default:
      // This triggers if an integer is cast to AlertState 
      // but doesn't match any of the defined enum values.
      return "Unknown State";
  }
}

void printLocalTime()
{
  struct tm timeinfo;

  // getLocalTime() fetches the time from the internal clock, which is synced by NTP
  if (!getLocalTime(&timeinfo))
  {
    Serial.println("Failed to obtain time from NTP server");
    return;
  }

  // Print the time to the serial monitor in a readable format
  Serial.println(&timeinfo, "network time: %Y-%m-%d %H:%M:%S ");
}

bool changeEvent(AlertState state){
  bool res = currentState!=state;
  if(res){
    currentState=state;
    eventStartTime=millis();
    Serial.println();
    printLocalTime();
    Serial.printf("Moving to state - \"%s\" !!!\n", stateToStr(state).c_str());
  }
  return res;
}

uint32_t loadColorOrDefault(JsonDocument& doc, const char* key, uint8_t defR, uint8_t defG, uint8_t defB) {
  if (doc["colors"][key].is<JsonArray>()) {
    JsonArray arr = doc["colors"][key];
    return strip.Color(arr[0], arr[1], arr[2]);
  }
  return strip.Color(defR, defG, defB);
}

bool getDataFromFile()
{
  Serial.println("Initializing File System and reading config...");

  if (!LittleFS.begin(true))
  {
    Serial.println("Error: Failed to mount LittleFS.");
    return false; // Return false if it fails
  }

  File file = LittleFS.open("/config.json", "r");
  if (!file)
  {
    Serial.println("Error: Failed to open config.json.");
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  
  if (error)
  {
    Serial.print("Error: Failed to parse config file: ");
    Serial.println(error.f_str());
    file.close();
    return false;
  } else {
    Serial.println("Config file parsed successfully:");
    serializeJsonPretty(doc, Serial);
    Serial.println();
  }

  // 5. Extract values into global variables with safe fallbacks
  area = doc["area"] | "Unknown_Area";
  exactArea = "\""+area+"\"";
  wifiSsid = doc["wifi_ssid"] | "Your_SSID";
  wifiPassword = doc["wifi_password"] | "your_password";
  useBuzzer = doc["use_buzzer"] | false;
  useVibrations = doc["use_vibrations"] | false;

  colorUnconnected    = loadColorOrDefault(doc, "unconnected", 0, 0, 120);
  colorUnconnectedDim = loadColorOrDefault(doc, "unconnected_dim", 0, 0, 60);
  colorNoAlerts       = loadColorOrDefault(doc, "no_alerts", 0, 255, 0);
  colorWarning        = loadColorOrDefault(doc, "warning", 255, 100, 0);
  colorAlertRockets   = loadColorOrDefault(doc, "alert_rockets", 255, 0, 0);
  colorAlertGeneral   = loadColorOrDefault(doc, "alert_general", 255, 60, 30);
  colorEventEnded     = loadColorOrDefault(doc, "event_ended", 0, 255, 0);
  colorEventEndedDim  = loadColorOrDefault(doc, "event_ended_dim", 0, 10, 0);

  // Close the file to free up memory
  file.close();

  // If we made it this far, everything worked perfectly!
  return true;
}

bool parseAlertJsonAndUpdateState(String jsonPayload)
{
  //Serial.printf("parseAlertJsonAndUpdateState - parsing JSON: %s\n\n", jsonPayload.c_str());

  DEBUG_PRINTF("current state is: << %s >>\n",stateToStr(currentState).c_str());

  bool res = false;
  if (jsonPayload == "ERROR")
  {
    changeEvent(STATE_UNCONNECTED);
    return res;
  }else if (jsonPayload.equals("[]")){
    Serial.print(".");
    changeEvent( STATE_NO_ALERTS);
    DEBUG_PRINTF("\n\n>>> small json :\n%s\n(length: %d)-> no alerts (Moving to STATE_NO_ALERTS).", jsonPayload.c_str(), jsonPayload.length());
    return res;
  } else {


    JsonDocument doc;
    DeserializationError parsingEerror = deserializeJson(doc, jsonPayload);

    if (parsingEerror){
      Serial.printf("parseAlertJsonAndUpdateState - parsing JSON: %s\n\n", jsonPayload.c_str());
      Serial.print("JSON Parse failed: ");
      Serial.println(parsingEerror.c_str());
      return res;
    } else {
      DEBUG_PRETTY(doc, Serial);
      DEBUG_PRINTLN();
    }

    String city = doc["data"].as<String>();

    if (city.equals(area.c_str())){

      String currentAlertDate = doc["alertDate"].as<String>();
      int category = doc["category"].as<int>();
      res = true;

      bool changedState = false;
      switch(AlertState(category)){
        case STATE_ALERT_ROCKETS:
          changedState = changeEvent(STATE_ALERT_ROCKETS);
          break;        
        case STATE_ALERT_AIRCRAFT:
          changedState = changeEvent(STATE_ALERT_AIRCRAFT);
          break;
        case STATE_EVENT_ENDED: 
          if (currentState!=STATE_NO_ALERTS){ //ending event only after event was triggered
            changedState = changeEvent(STATE_EVENT_ENDED);
          }
          break;
        case STATE_WARNING:
          changedState = changeEvent(STATE_WARNING);
          break;

        default:
          Serial.println("UNKNOWN CATEGORY NUMBER DETECTED :");
          serializeJsonPretty(doc, Serial);
          Serial.println();
          break;

      }//end switch

      if(changedState){
        DEBUG_PRETTY(doc, Serial);
        DEBUG_PRINTLN();
      }

    }//end city match
    return true;
  }//end handling json sring
}


String fetchAlertJson()
{
  String payload = "ERROR";

  if (WiFi.status() == WL_CONNECTED)
  {


#ifdef LOCALHOST_TESTING
    WiFiClient client;
#else
    WiFiClientSecure client;
    client.setInsecure(); // דילוג על אימות תעודת SSL
#endif

    HTTPClient http;
    http.begin(client, SERVER_URL);
    http.setTimeout(2000);
    http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    http.addHeader("Accept", "application/json, text/plain, */*");
    http.addHeader("Referer", "https://www.oref.org.il/");
    http.addHeader("X-Requested-With", "XMLHttpRequest");
    http.addHeader("Connection", "close");
    

    int checkedElementsCount = 0;
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK)
    {
      payload = "[]"; // Return an empty JSON array if no relevant objects were found

      WiFiClient &stream = http.getStream();

      String currentObject = "";
      currentObject.reserve(512); // Pre-allocate memory to prevent heap fragmentation

      int braceCount = 0;
      bool inObject = false;

      Serial.print("%");

#ifdef LOCALHOST_TESTING
    const unsigned long TIMEOUT_MS = 1000;
#else
    const unsigned long TIMEOUT_MS = 50;
#endif
      
      unsigned long lastDataTime = millis();

      // Read the stream byte-by-byte
      while (stream.connected())
      {
        //Serial.printf("stream available %d , stream connected %d\n", stream.available(), stream.connected());
        if (stream.available())
        {
          char c = stream.read();

          // Track when an object starts and handles nested braces
          if (c == '{')
          {
            inObject = true;
            braceCount++;
          }

          // Build the string for the current object
          if (inObject)
          {
            currentObject += c;
          }

          // Track when the object ends
          if (c == '}')
          {
            braceCount--;

            if (braceCount == 0)
            {
              checkedElementsCount++;

              // Serial.print(currentObject);
              // Serial.println(";");

              if (currentObject.indexOf(exactArea.c_str()) > 0)
              {
                DEBUG_PRINTLN("\n>>> Found a JSON object containing our area! Parsing this object:");

                DEBUG_PRINTF("------------------object #%05d--------------------\n", checkedElementsCount);
                DEBUG_PRINT(currentObject);
                DEBUG_PRINTLN("--------------------------------------------------");
  
                payload = currentObject;
                break; // Stop reading more objects since we found a relevant one
              }

              // If we are here, it wasn't a match. Clear the buffer for the next object.
              currentObject = "";
              inObject = false;
            }
          }
          lastDataTime = millis();
        }
        else
        {
          // No data available right now. Have we waited too long?
          if (millis() - lastDataTime > TIMEOUT_MS)
          {
            break; // We've waited long enough. Exit the loop.
          }
          // CRITICAL: A tiny delay to prevent the ESP32 from crashing
          delay(1);
        }
      }
      DEBUG_PRINTF("Finished reading stream. stream available %d , stream connected %d\n Total objects read:", stream.available(), stream.connected(),checkedElementsCount);
      stream.stop();
    }
    else
    {
      Serial.printf("HTTP GET Failed, error code: %d\n", httpCode);
    }

    http.end();
    client.stop(); // <--- משחרר את חיבור הרשת ומונע קריסת DNS!
  }

  //Serial.printf(">>> fetchAlertJson - received payload: %s\n\n", payload.c_str());
  return payload;
}


void operateLEDs()
{
  static unsigned long lastLedUpdate = 0;
  static bool ledState = false;

  // קצב הבהוב משתנה בהתאם למצב
  unsigned long blinkInterval = 1000;
  if (currentState == STATE_ALERT_ROCKETS)
    blinkInterval = 500;
  else if (currentState == STATE_UNCONNECTED)
    blinkInterval = 1000;

  if (millis() - lastLedUpdate >= blinkInterval)
  {
    lastLedUpdate = millis();
    ledState = !ledState;
    uint32_t targetColor = 0;

    switch (currentState)
    {
      case STATE_UNCONNECTED:
        targetColor = ledState ? colorUnconnected : colorUnconnectedDim;
        break;

      case STATE_NO_ALERTS:
        targetColor = colorNoAlerts;
        break;

      case STATE_WARNING:
        targetColor = colorWarning;
        break;

      case STATE_ALERT_ROCKETS:
        targetColor = colorAlertRockets;
        break;

      case STATE_ALERT_AIRCRAFT:
        targetColor = ledState ? colorAlertGeneral : colorAlertRockets;
        break;

      case STATE_EVENT_ENDED:
        targetColor = ledState ? colorEventEnded : colorEventEndedDim;
        break;
    }

    // צביעת כל פס הלדים בפקודה אחת במקום בלולאה
    strip.fill(targetColor);
    strip.show();
  }
}

const int buzzerPin = 5;
const int vibrationsPin = 6;
const int ledcChannel = 0;
const int resolution = 8;
const int freq = 2000;
void operateBuzzer()
{
  static unsigned long lastBuzzerUpdate = 0;
  static bool buzzerState = false;

  unsigned long buzzInterval = (currentState == STATE_ALERT_ROCKETS) ? 500 : 1000;

  if (currentState == STATE_NO_ALERTS || currentState == STATE_EVENT_ENDED || currentState == STATE_UNCONNECTED)
  {
    ledcWriteTone(ledcChannel, 0);
    return;
  }

  if (millis() - lastBuzzerUpdate >= buzzInterval)
  {
    lastBuzzerUpdate = millis();
    buzzerState = !buzzerState;
    ledcWriteTone(ledcChannel, buzzerState ? 1000 : 0);
  }
}

void operateVibrations(){

}

static unsigned long lastWiFiAttempt = 0;
void connectToWifiIfNeeded()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    // Serial.println("WiFi unconnected.");
    if (millis() - lastWiFiAttempt >= 2000)
    {
      lastWiFiAttempt = millis();
      Serial.println("Attempting to reconnect...");
      currentState = STATE_UNCONNECTED;

      WiFi.disconnect();
      delay(100);       // השהייה קטנטנה שמאפשרת לאנטנה להתנתק באמת
      WiFi.reconnect(); // פקודה נקייה יותר מ-begin שמאלצת חיבור מחדש
      delay(1000);
    }
  }
}

void handleAlertsPoling(){
  static unsigned long lastApiCheck = 0;
  API_CHECK_INTERVAL = (currentState == STATE_NO_ALERTS) ? 5000 : 1000;
  if (millis() - lastApiCheck >= API_CHECK_INTERVAL) //did enough time passed from last check?
  {
    lastApiCheck = millis();

    if (WiFi.status() == WL_CONNECTED)
    {
      String jsonPayload = fetchAlertJson();

      // Serial.printf("---> %s\n", jsonPayload.c_str());
      if (jsonPayload == "ERROR")
      {
        networkErrorCount++;
        Serial.printf("Network error count: %d, network recovery events count: %d\n", networkErrorCount, MajorNetworkErrorRecoveryCount);

        // TODO: consider calling connectToWifiIfNeeded()
        if (networkErrorCount >= 3)
        {
          Serial.println(">>> Too many network errors. Forcing HARD WiFi Reset...");
          currentState = STATE_UNCONNECTED;

          WiFi.disconnect(true); // ניתוק אגרסיבי כולל מחיקת הגדרות זמניות
          delay(200);
          WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
          delay(1000);
          if (WiFi.status() == WL_CONNECTED)
          {
            MajorNetworkErrorRecoveryCount++;
            lastWiFiAttempt = millis();
            // אנחנו נאפס גם את זמן בדיקת ה-API כדי שלא ינסה לבדוק מיד לפני שהרשת קמה
            lastApiCheck = millis();
          }
          if (networkErrorCount > 10 || MajorNetworkErrorRecoveryCount > 5)
          {
            // ESP.reset()
            ESP.restart();
          }
        }
      }
      else
      {
        networkErrorCount = 0;
        parseAlertJsonAndUpdateState(jsonPayload);
      }
    }
  }
}

void handleAlertEndedStateTimout(){
  if (currentState == STATE_EVENT_ENDED)
  {
    if (millis() - eventStartTime > 20000)
    {
      changeEvent( STATE_NO_ALERTS);
    }
  }
}

void setupOTA()
{
  ArduinoOTA.setHostname(DEVICE_HOSTNAME);

  // These are just optional callbacks to print the update progress to the Serial monitor
  ArduinoOTA.onStart([]()
                     { Serial.println("\n--- OTA Update Starting ---"); });
  ArduinoOTA.onEnd([]()
                   { Serial.println("\n--- OTA Update Complete ---"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); });
  ArduinoOTA.onError([](ota_error_t error)
                     { Serial.printf("Error[%u]: ", error); });

  // Start the OTA service
  ArduinoOTA.begin();
  Serial.println("OTA Service Started. Ready for remote uploads.");
}

void setup()
{
  esp_task_wdt_init(5, true); // טיימר של 30 שניות, הפעלה מחדש במקרה של תקיעה
  esp_task_wdt_add(NULL);      // הוספת הלופ הנוכחי לניטור

  Serial.begin(115200);

  if (!getDataFromFile())
  {
    Serial.println(">>> Failed to read config from file. Using default values.");
  }
  else
  {
    Serial.println(">>> Config loaded successfully:");
  }

  // define buzzer
  ledcSetup(ledcChannel, freq, resolution);
  ledcAttachPin(buzzerPin, ledcChannel);
  // test buzzer
  ledcWriteTone(ledcChannel, 1000);
  delay(1000);
  ledcWriteTone(ledcChannel, 0);

  strip.begin();
  strip.setPixelColor(2, strip.Color(255, 0, 0));
  strip.setPixelColor(1, strip.Color(0, 255, 0));
  strip.setPixelColor(0, strip.Color(0, 0, 255));
  strip.show();

  Serial.printf("connect to %s WiFi \n\n", wifiSsid.c_str());
  WiFi.begin(wifiSsid.c_str(), wifiPassword);
  delay(1000);

  Serial.print("ESP32 IP Address: ");
  Serial.println(WiFi.localIP());

  // to have most fastest wifi. power hungry - disable if on battery use:
  setupOTA();

  Serial.printf("area of interest -  %s\n", area.c_str());

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.print("setup complete at ");
  printLocalTime();
  Serial.println("----------------------------------------------------------\n");
}

void loop(){
  esp_task_wdt_reset();
  connectToWifiIfNeeded();
  ArduinoOTA.handle();

  handleAlertsPoling();
  handleAlertEndedStateTimout();

  operateLEDs();
  if (useBuzzer) operateBuzzer();
  if (useVibrations) operateVibrations();
}