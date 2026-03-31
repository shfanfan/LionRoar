#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>
#include <WebServer.h>

// ניצור מופע של השרת על פורט 80 (פורט סטנדרטי לגלישה)
WebServer server(80);
// משתנה שיעזור לנו לדעת אם אנחנו במצב "ראוטר" (AP) או מחוברים לרשת
bool isAPMode = false;
//TODO: connectivity setup via local web page
//TODO: settings setup (including save) via local web page
//TODO: OTA updates via remote web access (per individual MAC)



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

#ifdef LOCALHOST_TESTING
    WiFiClient client;
#else
    WiFiClientSecure client;
#endif

#ifndef LED_PIN
#define LED_PIN 48
#endif
#ifndef NUM_LEDS
#define NUM_LEDS 15
#endif

#pragma endregion defines

const int buzzerPin = 5;
const int ledcChannel = 0;
const int resolution = 8;
const int freq = 2000;

// Add these to your global defines
const int vibChannel = 1;     // Use a different channel than the buzzer!
const int vibFreq = 200;     // Hz
const int vibRes = 8;         // 8-bit resolution (0-255)
int vibrationStrength = 180;  // Your desired strength (0 to 255)


bool OTAUpdateInProgress = false;

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

unsigned long apiCheckInterval = 5000;

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

#pragma region webServer

// HTML tempate:
const char* htmlTemplate = R"=====(
<!DOCTYPE html>
<html dir='rtl' lang='he'>
<head>
  <meta charset='UTF-8'>
  <meta name='viewport' content='width=device-width, initial-scale=1.0'>
  <title>הגדרות צופר עורף</title>
  <style>
    body { font-family: Arial, sans-serif; background-color: #f4f4f9; padding: 20px; }
    div.container { background: white; padding: 20px; border-radius: 10px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); max-width: 400px; margin: auto; }
    h2 { color: #333; text-align: center; }
    label { font-weight: bold; margin-top: 10px; display: block; }
    input[type='text'], input[type='password'], input[type='number'] { width: 100%; padding: 8px; margin: 5px 0 15px 0; border: 1px solid #ccc; border-radius: 5px; box-sizing: border-box; }
    input[type='checkbox'] { transform: scale(1.5); margin: 10px; }
    button { width: 100%; background-color: #28a745; color: white; padding: 10px; border: none; border-radius: 5px; font-size: 16px; cursor: pointer; }
    button:hover { background-color: #218838; }
  </style>
</head>
<body>
  <div class='container'>
    <h2>הגדרות מערכת</h2>
    <form action='/save' method='POST'>
      <label>שם רשת WiFi (SSID):</label>
      <input type='text' name='ssid' value='{SSID_VAL}'>
      
      <label>סיסמת WiFi:</label>
      <input type='password' name='password' value='{PASS_VAL}'>
      
      <label>אזור התרעה (למשל: טל - אל):</label>
      <input type='text' name='area' value='{AREA_VAL}'>
      
      <label>עוצמת רטט (0-255):</label>
      <input type='number' name='vibStrength' min='0' max='255' value='{VIB_VAL}'>
      
      <label><input type='checkbox' name='useVib' {USE_VIB_CHK}> הפעל מנוע רטט</label><br>
      <label><input type='checkbox' name='useBuzzer' {USE_BUZ_CHK}> הפעל זמזם</label><br><br>
      
      <button type='submit'>שמור והפעל מחדש</button>
    </form>
  </div>
</body>
</html>
)=====";

void handleRoot() {
  // מעתיקים את תבנית ה-HTML לתוך אובייקט String שניתן לעריכה
  String html = String(htmlTemplate);
  
  // מחליפים את שומרי המקום בערכים האמיתיים מהמשתנים הגלובליים שלך
  html.replace("{SSID_VAL}", wifiSsid);
  html.replace("{PASS_VAL}", wifiPassword);
  html.replace("{AREA_VAL}", area);
  html.replace("{VIB_VAL}", String(vibrationStrength));
  
  // עבור צ'קבוקסים, אם זה true נשתול את המילה 'checked', אחרת נשאיר ריק
  html.replace("{USE_VIB_CHK}", useVibrations ? "checked" : "");
  html.replace("{USE_BUZ_CHK}", useBuzzer ? "checked" : "");
  
  // שולחים את הדף המוכן לדפדפן
  server.send(200, "text/html", html);
}

void handleSave() {
  Serial.println("Received new settings from Web UI!");
  
  File file = LittleFS.open("/config.json", "r");
  JsonDocument doc;
  if (file) {
    deserializeJson(doc, file);
    file.close();
  }

  // שמירת הנתונים ל-JSON
  if (server.hasArg("ssid")) doc["wifi_ssid"] = server.arg("ssid");
  if (server.hasArg("password")) doc["wifi_password"] = server.arg("password");
  if (server.hasArg("area")) doc["area"] = server.arg("area"); 
  if (server.hasArg("vibStrength")) doc["vibrationStrength"] = server.arg("vibStrength").toInt();
  
  doc["use_vibrations"] = server.hasArg("useVib") ? true : false;
  doc["use_buzzer"] = server.hasArg("useBuzzer") ? true : false;

  // כתיבה חזרה ל-LittleFS
  file = LittleFS.open("/config.json", "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
    Serial.println("Config saved successfully.");
  }

  // דף תגובה מהיר עם Raw String
  String response = R"=====(
    <html dir='rtl' lang='he'>
    <body style='font-family: Arial; text-align: center; margin-top: 50px;'>
      <h2>ההגדרות נשמרו בהצלחה!</h2>
      <p>המכשיר מבצע הפעלה מחדש. אם שינית הגדרות רשת, אנא התחבר מחדש.</p>
    </body>
    </html>
  )=====";
  
  server.send(200, "text/html", response);
  
  delay(2000); // מחכים 2 שניות כדי שהדפדפן יספיק לטעון את דף התגובה
  ESP.restart(); // הפעלה מחדש מבוקרת
}


void setupWebServerRoutes() {
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  Serial.println("HTTP server started");
}

#pragma endregion webServer html stuff

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
  int rawStrength = doc["vibrationStrength"] | 180; 
  // Safety: ensure the value is within 8-bit PWM limits (0-255)
  vibrationStrength = constrain(rawStrength, 0, 255);

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
    if (currentState == STATE_UNCONNECTED){
      changeEvent(STATE_NO_ALERTS);
      DEBUG_PRINTF("\n\n>>> small json :\n%s\n(length: %d)-> no alerts (Moving to STATE_NO_ALERTS).", jsonPayload.c_str(), jsonPayload.length());
    }
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


#ifndef LOCALHOST_TESTING
    client.setInsecure(); // דילוג על אימות תעודת SSL
#endif

    HTTPClient http;
    http.begin(client, SERVER_URL);
    http.setTimeout(2000);
    http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    http.addHeader("Accept", "application/json, text/plain, */*");
    http.addHeader("Referer", "https://www.oref.org.il/");
    http.addHeader("X-Requested-With", "XMLHttpRequest");
    //http.addHeader("Connection", "close");
    http.addHeader("Connection", "keep-alive");

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
              currentObject.clear();
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
    //client.stop(); // <--- משחרר את חיבור הרשת ומונע קריסת DNS!
  }

  //Serial.printf(">>> fetchAlertJson - received payload: %s\n\n", payload.c_str());
  return payload;
}


void operateLEDs()
{
  static uint32_t lastTargetColor = 0;
  static unsigned long lastLedUpdate = 0;
  static bool ledState = false;

  // קצב הבהוב משתנה בהתאם למצב
  unsigned long blinkInterval = 2000;
  if (currentState == STATE_ALERT_ROCKETS || currentState == STATE_ALERT_AIRCRAFT)
    blinkInterval = 500;
  else if (currentState == STATE_UNCONNECTED)
    blinkInterval = 1000;

  if (millis() - lastLedUpdate >= blinkInterval)
  {
    lastLedUpdate = millis();
    ledState = !ledState;
  }

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
  if(lastTargetColor!=targetColor){
    strip.fill(targetColor);
    strip.show();
    lastTargetColor = targetColor;
  }
  
}


void operateBuzzer()
{
  static unsigned long lastBuzzerUpdate = 0;
  static bool patternState = false;
  static AlertState lastHandledState = STATE_UNCONNECTED;
  static uint32_t lastAppliedFreq = 0; // Keep track of the current hardware output

  // 1. IMMEDIATE TRIGGER: Detect state change instantly
  if (currentState != lastHandledState) {
    lastHandledState = currentState;
    patternState = true;         // Start with the sound ON
    lastBuzzerUpdate = millis(); // Reset the pulse timer
  }

  unsigned long buzzInterval = 0;
  uint32_t activeFreq = 1000;    // Standard 1000Hz tone

  // 2. Set the rhythm based on the alert type
  switch (currentState) {
    case STATE_ALERT_ROCKETS:
      buzzInterval = 500;  // Fast beep: 500ms ON / 500ms OFF
      break;
    case STATE_ALERT_AIRCRAFT:
      buzzInterval = 1000; // Slower beep: 1000ms ON / 1000ms OFF
      break;
    case STATE_WARNING:
      buzzInterval = 1500; // Very slow warning pulse
      break;
    default:
      buzzInterval = 0;    // Silence for routine, unconnected, or ended events
      break;
  }

  uint32_t targetFreq = 0;

  // 3. Update the pulse pattern if an alert is active
  if (buzzInterval > 0) {
    if (millis() - lastBuzzerUpdate >= buzzInterval) {
      lastBuzzerUpdate = millis();
      patternState = !patternState; // Toggle between ON and OFF
    }
    // If patternState is true, play the tone. Otherwise, silence (0Hz)
    targetFreq = patternState ? activeFreq : 0; 
  } else {
    patternState = false; // Reset so it starts fresh next time
    targetFreq = 0;       // Force silence
  }

  // 4. HARDWARE WRITE: Only send the command if the frequency actually needs to change
  if (targetFreq != lastAppliedFreq) {
    ledcWriteTone(ledcChannel, targetFreq);
    lastAppliedFreq = targetFreq;
  }
}


void operateVibrations()
{
  static int lastAppliedDuty = -1;
  static unsigned long lastVibrationUpdate = 0;
  static unsigned long alertStartTime = 0; // טיימר חדש למעקב אחר משך האזעקה
  static bool patternState = false;
  static AlertState lastHandledState = STATE_UNCONNECTED;

  // 1. זיהוי שינוי מצב - איפוס כל הטיימרים
  if (currentState != lastHandledState) {
    lastHandledState = currentState;
    patternState = true; 
    lastVibrationUpdate = millis(); 
    alertStartTime = millis(); // שומרים את הרגע המדויק שבו החלה ההתרעה
  }

  unsigned long vibrationInterval = 0;
  
  switch (currentState) {
    case STATE_ALERT_ROCKETS:
    case STATE_ALERT_AIRCRAFT:
      vibrationInterval = 400; 
      break;
    case STATE_WARNING:
      vibrationInterval = 1000; 
      break;
    default:
      vibrationInterval = 0; 
      break;
  }

  int targetDuty = 0;

  // 2. בדיקה: האם אנחנו עדיין בתוך 30 השניות הראשונות של האזעקה?
  bool isWithinTimeLimit = (millis() - alertStartTime <= 20000);

  // 3. לוגיקת הפולסים (רק אם יש התרעה פעילה, ורק אם טרם עברו 30 שניות)
  if (vibrationInterval > 0 && isWithinTimeLimit) {
    if (millis() - lastVibrationUpdate >= vibrationInterval) {
      lastVibrationUpdate = millis();
      patternState = !patternState;
    }
    // אם הסטטוס דלוק, ניתן את העוצמה. אחרת 0.
    targetDuty = patternState ? vibrationStrength : 0;
  } else {
    // אם עברו 30 שניות (או שאין התרעה), כופים כיבוי של המנוע
    patternState = false;
    targetDuty = 0;
  }

  // 4. כתיבה לחומרה והשהיה (הטריק שלך)
  if (targetDuty != lastAppliedDuty) {
    
    // מדפיס רק כשיש שינוי
    Serial.printf("vibration is %s, target duty: %d, vibrationInterval=%d \n", patternState ? "on" : "off", targetDuty, vibrationInterval);
    
    ledcWrite(vibChannel, targetDuty);
    lastAppliedDuty = targetDuty;

    // אם הרגע הדלקנו את המנוע - עוצרים את התוכנית כדי לתת לו מקסימום מתח
    if (targetDuty > 0) {
      esp_task_wdt_reset();      // "מאכילים" את כלב השמירה כדי שלא יעשה ריסטרט בגלל ה-delay
      delay(vibrationInterval+10);  // חוסמים את ה-WiFi ומרעידים בעוצמה!
      
      // אין צורך לאפס את lastVibrationUpdate כאן.
      // מכיוון שעשינו delay, ברגע שהפונקציה תרוץ שוב בלופ הבא, 
      // הפער בזמנים (millis) יהיה גדול מה-Interval, והקוד יכבה את המנוע אוטומטית.
    }
  }
}


// void operateVibrations()
// {
//   static int lastAppliedDuty = -1; // Track duty cycle to avoid spamming
//   static unsigned long lastVibrationUpdate = 0;
//   static bool patternState = false;
//   static AlertState lastHandledState = STATE_UNCONNECTED;

//   // 1. IMMEDIATE TRIGGER
//   if (currentState != lastHandledState) {
//     lastHandledState = currentState;
//     patternState = true; 
//     lastVibrationUpdate = millis(); 
//   }

//   unsigned long vibrationInterval = 0;
  
//   switch (currentState) {
//     case STATE_ALERT_ROCKETS:
//     case STATE_ALERT_AIRCRAFT:
//       vibrationInterval = 600; 
//       break;
//     case STATE_WARNING:
//       vibrationInterval = 1000; 
//       break;
//     default:
//       vibrationInterval = 0; 
//       break;
//   }

//   int targetDuty = 0;

//   // 2. Pulse Logic
//   if (vibrationInterval > 0) {
//     if (millis() - lastVibrationUpdate >= vibrationInterval) {
//       lastVibrationUpdate = millis();
//       patternState = !patternState;
//     }
//     // If ON, use our strength value. If OFF, use 0.
//     targetDuty = patternState ? vibrationStrength : 0;
//   } else {
//     patternState = false;
//     targetDuty = 0;
//   }

//   // 3. PWM Hardware Write
//   if (targetDuty != lastAppliedDuty) {
//     Serial.printf("vibration is %s, target duty: %d, vibrationInterval=%d \n", patternState?"on":"off" , targetDuty , vibrationInterval);
//     ledcWrite(vibChannel, targetDuty);
//     lastAppliedDuty = targetDuty;
//     if (targetDuty>0) delay(vibrationInterval);
//   }
// }

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
      networkErrorCount++;

      WiFi.disconnect();
      delay(100);       // השהייה קטנטנה שמאפשרת לאנטנה להתנתק באמת
      WiFi.reconnect(); // פקודה נקייה יותר מ-begin שמאלצת חיבור מחדש
      delay(1000);
    }
  }
}

void handleAlertsPoling(){
  static unsigned long lastApiCheck = 0;
  apiCheckInterval = (currentState == STATE_NO_ALERTS) ? 5000 : 1000;
  if (millis() - lastApiCheck >= apiCheckInterval) //did enough time passed from last check?
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
        }
      }
      else
      {
        networkErrorCount = 0;
        parseAlertJsonAndUpdateState(jsonPayload);
      }
    } else { //not connected
      networkErrorCount++;
      if (networkErrorCount > 10 || MajorNetworkErrorRecoveryCount > 5)
      {
        ESP.restart();
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
                     { 
                      OTAUpdateInProgress = true;
                      Serial.println("\n--- OTA Update Starting ---"); 
                    });
  ArduinoOTA.onEnd([]()
                   { 
                    OTAUpdateInProgress = false;
                    Serial.println("\n--- OTA Update Complete ---"); 
                  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        { 
                          esp_task_wdt_reset();
                          Serial.printf("Progress: %u%%\r", (progress / (total / 100))); 
                        });
  ArduinoOTA.onError([](ota_error_t error)
                     { 
                      OTAUpdateInProgress=false;
                      Serial.printf("Error[%u]: ", error); 
                    });

  // Start the OTA service
  ArduinoOTA.begin();
  Serial.println("OTA Service Started. Ready for remote uploads.");
}


void setup()
{
  esp_task_wdt_init(10, true); 
  esp_task_wdt_add(NULL);

  Serial.begin(115200);

  if (!getDataFromFile())
  {
    Serial.println(">>> Failed to read config from file. Using default values.");
  }
  else
  {
    Serial.println(">>> Config loaded successfully:");
  }

  if(useBuzzer){
    // TODO: see that useBuzzer and useVibrations have effect here
    // TODO: move code to hardwareSetup() function?
    // define buzzer
    ledcSetup(ledcChannel, freq, resolution);
    ledcAttachPin(buzzerPin, ledcChannel);

      
    ledcWriteTone(ledcChannel, 1000);
    delay(1000);
    ledcWriteTone(ledcChannel, 0);
  }

  if (useVibrations){
    //define vibration motor
    Serial.printf("vibration pin is %d\n",VIBRATION_PIN);
    ledcSetup(vibChannel, vibFreq, vibRes);
    ledcAttachPin(VIBRATION_PIN, vibChannel);

    //TODO: move test to handle function (if(firstTime)...)
    //test buzzer and vibration motor:

    ledcWrite(vibChannel, 30);
    delay(200);
    ledcWrite(vibChannel, 0);
  }


  strip.begin();
  strip.setPixelColor(2, strip.Color(255, 0, 0));
  strip.setPixelColor(1, strip.Color(0, 255, 0));
  strip.setPixelColor(0, strip.Color(0, 0, 255));
  strip.show();
  
  Serial.printf("Connecting to %s WiFi \n", wifiSsid.c_str());
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  
  // נסיון חיבור למשך 15 שניות
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 5) {
    delay(1000);
    Serial.print(".");
    retries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected! IP Address: %s , MAC address: %s \n",WiFi.localIP().toString().c_str(),WiFi.macAddress());
    isAPMode = false; 
  } else {
    // נכשל בחיבור -> פותח רשת עצמאית (Access Point)
    Serial.println("WiFi Failed. Starting Access Point mode...");
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Alert_Setup", "12345678"); // שם הרשת והסיסמה שלה
    Serial.print("AP IP Address: ");
    Serial.println(WiFi.softAPIP()); // בדרך כלל יהיה 192.168.4.1
    isAPMode = true;
    
    // אופציונלי: אפשר לצבוע את הלדים בצבע ספציפי (למשל סגול) כדי שתדע שזה במצב הגדרות
    strip.fill(strip.Color(100, 0, 100));
    strip.show();
  }

  // בכל מקרה מפעילים את שרת הרשת! (כדי שאפשר יהיה לקנפג גם ברשת המקומית וגם ב-AP)
  setupWebServerRoutes();

  // ... (המשך ה-setup: OTA, זמן וכו' - רק אם אנחנו לא ב-AP Mode) ...
  if (!isAPMode) {
    setupOTA();
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  }
}
//   Serial.printf("connect to %s WiFi \n\n", wifiSsid.c_str());
//   WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str);



//   Serial.print("ESP32 IP Address: ");
//   Serial.println(WiFi.localIP());


//   // to have most fastest wifi. power hungry - disable if on battery use:
//   setupOTA();

//   Serial.printf("area of interest -  %s\n", area.c_str());

//   configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
//   Serial.print("setup complete at ");
//   printLocalTime();
//   Serial.println("----------------------------------------------------------\n");
// }

void loop(){
  esp_task_wdt_reset();
  
  server.handleClient(); 

  if (!isAPMode) {
    ArduinoOTA.handle();

    if (!OTAUpdateInProgress){
      connectToWifiIfNeeded();
      handleAlertsPoling();
      handleAlertEndedStateTimout();

      operateLEDs();
      if (useBuzzer) operateBuzzer();
      if (useVibrations) operateVibrations();
    }

  }else {
    // אם אנחנו במצב הגדרות, נהבהב את הלדים בסגול כדי שנזכור שצריך להתחבר דרך הטלפון
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 1000) {
      lastBlink = millis();
      static bool ledState = false;
      ledState = !ledState;
      strip.fill(ledState ? strip.Color(100, 0, 100) : strip.Color(0, 0, 0));
      strip.show();
    }
  }
}