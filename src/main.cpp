#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>

// ==========================================
// הגדרות כלליות ורשת
// ==========================================
String wifiSsid = "empty ssid";
String wifiPassword = "wrong password";
// --- NTP Server Settings ---
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;     // Replace with your timezone offset in seconds (e.g., -18000 for EST)
const int daylightOffset_sec = 0; // Set to 3600 if your timezone observes daylight saving time

// שם האיזור
String area = "הוגוורטס";

enum AlertState
{
  STATE_UNCONNECTED,   // מצב התחלתי - ממתין לנתונים ראשונים מהשרת
  STATE_NO_ALERTS,     // מצב שגרה - אין התרעות
  STATE_WARNING,       // בדקות הקרובות יתקבלו התראות באיזורך (מצב זמני שמוצג לפני ההתרעה עצמה)
  STATE_ALERT_ROCKETS, // התרעת ירי רקטות וטילים
  STATE_ALERT_GENERAL, // התרעה כללית (חדירת כלי טיס, רעידת אדמה וכו')
  STATE_EVENT_ENDED    // סיום אירוע (מצב זמני שמוצג אחרי שההתרעה יורדת)
};

int networkErrorCount = 0; // מונה שגיאות רשת ברצף
int MajorNetworkErrorRecoveryCount = 0; // סף לשגיאה חמורה שתוביל לריסט
AlertState currentState = STATE_UNCONNECTED;
unsigned long eventEndedStartTime = 0; // טיימר למצב סיום אירוע

// משתנה חדש: שמירת תאריך ההתראה האחרונה כדי לא להפעיל אזעקות על אירועי עבר
String lastAlertDate = "";

bool getDataFromFile()
{
  Serial.println("Initializing File System and reading config...");

  // 1. Mount LittleFS
  if (!LittleFS.begin(true))
  {
    Serial.println("Error: Failed to mount LittleFS.");
    return false; // Return false if it fails
  }

  // 2. Open the config file
  File file = LittleFS.open("/config.json", "r");
  if (!file)
  {
    Serial.println("Error: Failed to open config.json.");
    return false;
  }

  // 3. Parse the JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);

  // 4. Handle parsing errors
  if (error)
  {
    Serial.print("Error: Failed to parse config file: ");
    Serial.println(error.f_str());
    file.close();
    return false;
  }

  // 5. Extract values into global variables with safe fallbacks
  area = doc["area"] | "Unknown_Area";
  wifiSsid = doc["wifi_ssid"] | "Your_SSID";
  wifiPassword = doc["wifi_password"] | "your_password";

  // Close the file to free up memory
  file.close();

  // If we made it this far, everything worked perfectly!
  return true;
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

bool parseAlertJsonAndUpdateState(String jsonPayload)
{
  // Serial.printf("parseAlertJsonAndUpdateState - parsing JSON: %s\n\n", jsonPayload.c_str());

  bool res = false;
  // 1. טיפול בשגיאת תקשורת
  if (jsonPayload == "ERROR")
  {
    currentState = STATE_UNCONNECTED;
    return res;
  }
  else
  { // good json (though could be empty...
    if (currentState == STATE_UNCONNECTED)
    {
      currentState = STATE_NO_ALERTS;
      // Serial.println("\n>>> connection to pikud ha'oref server confirmed! (Moving to STATE_NO_ALERTS)");
    }
  }

  // 2. טיפול בקובץ ריק (הכל תקין, פשוט אין כרגע התראות בארץ)
  if (jsonPayload.length() < 10)
  {
    Serial.print(".");
    currentState = STATE_NO_ALERTS;
    return res;
  }

  // הקצאת זיכרון לפארסר
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonPayload);

  if (error)
  {
    Serial.printf("parseAlertJsonAndUpdateState - parsing JSON: %s\n\n", jsonPayload.c_str());
    Serial.print("JSON Parse failed: ");
    Serial.println(error.c_str());
    return res;
  }
  else
  {
    // Serial.println("serializing JSON to Serial Monitor for debugging:");
    // serializeJsonPretty(doc, Serial);
    // Serial.println();
  }

  // סריקת המערך מפיקוד העורף (האיבר הראשון הוא החדש ביותר)
  // for (JsonObject alert : doc.as<JsonObject>()) {
  String city = doc["data"].as<String>();

  // Serial.printf("\n>>> Checking alert for city: %s - %s \n", city.c_str(), city.startsWith(MY_AREA) ? "MATCH!" : "no match");

  if (city.startsWith(area.c_str()))
  {
    String currentAlertDate = doc["alertDate"].as<String>();
    int category = doc["category"].as<int>();

    // Serial.printf("category: %d, alertDate: %s, city: %s\n", category, currentAlertDate.c_str(), city.c_str());

    // מניעת טיפול כפול: האם זו אותה התראה שכבר טיפלנו בה?
    if (currentAlertDate == lastAlertDate)
    {
      return res; // break; // יוצאים מיד, אין צורך להמשיך לסרוק
    }

    // מצאנו התראה חדשה (או שהמערכת עשתה ריסט)
    lastAlertDate = currentAlertDate;

    res = true;

    // ניתוב למצב המתאים
    if (category == 13)
    {
      // סיום אירוע / חזרה לשגרה
      currentState = STATE_EVENT_ENDED;
      eventEndedStartTime = millis();
      printLocalTime();
      Serial.println("Action: Event Ended (Moving to All-Clear sequence).");
      serializeJsonPretty(doc, Serial);
      Serial.println();
    }
    else if (category == 1)
    {
      // ירי רקטות וטילים
      currentState = STATE_ALERT_ROCKETS;
      printLocalTime();
      Serial.println("Action: ROCKET ALARM TRIGGERED! (Moving to STATE_ALERT_ROCKETS).");
      serializeJsonPretty(doc, Serial);
      Serial.println();
    }
    else if (category == 14)
    {
      // ירי רקטות וטילים
      currentState = STATE_WARNING;
      printLocalTime();
      Serial.println("Action: posibly alarm soon! (Moving to STATE_WARNING).");
      serializeJsonPretty(doc, Serial);
      Serial.println();
    }
    else
    {
      // כל איום אחר
      currentState = STATE_ALERT_GENERAL;
      printLocalTime();
      Serial.println("Action: GENERAL ALARM TRIGGERED! (Moving to STATE_ALERT_GENERAL).");
      serializeJsonPretty(doc, Serial);
      Serial.println();
    }

    // הגבנו לאירוע הכי עדכני ליישוב שלנו - יוצאים מסריקת ההיסטוריה
    // break;
  }
  //}
  return res;
}

const char *SERVER_URL = "https://www.oref.org.il/warningMessages/alert/History/AlertsHistory.json";

String fetchAlertJson()
{
  String payload = "ERROR";

  if (WiFi.status() == WL_CONNECTED)
  {
    WiFiClientSecure client;
    client.setInsecure(); // דילוג על אימות תעודת SSL

    HTTPClient http;
    http.begin(client, SERVER_URL);
    http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    http.addHeader("Accept", "application/json, text/plain, */*");
    http.addHeader("Referer", "https://www.oref.org.il/");
    http.addHeader("X-Requested-With", "XMLHttpRequest");
    http.addHeader("Connection", "close");

    int count = 0;
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

      // Set a timeout limit (e.g., 50 milliseconds)
      const unsigned long TIMEOUT_MS = 50;
      unsigned long lastDataTime = millis();

      // Read the stream byte-by-byte
      while (stream.connected())
      {
        // Serial.printf("stream available %d , stream connected %d\n", stream.available(), stream.connected());
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
              count++;
              // We now have ONE complete object in the currentObject string.
              // Serial.print(currentObject);
              // Serial.println(";");
              // QUICK FILTER: Does this raw string even contain our target?
              // This saves us from running the JSON parser on every single object.
              if (currentObject.indexOf(area.c_str()) > 0)
              {
                Serial.println("\n>>> Found a JSON object containing our area! Parsing this object:");

                Serial.printf("------------------object #%05d--------------------\n", count);
                Serial.print(currentObject);
                Serial.println("--------------------------------------------------");
                // We have a match! Now we safely parse just this one small object.
                // StaticJsonDocument<512> doc;
                // DeserializationError error = deserializeJson(doc, currentObject);

                // if (!error)
                // {
                //   serializeJsonPretty(doc, Serial);
                //   //break; // Uncomment if you only want to process the first matching object
                // }

                // if (parseAlertJsonAndUpdateState(currentObject)) {
                //   Serial.println(">>> State updated based on this alert. No need to check older alerts in the history.");
                //   // If the function returns true, it means we found a relevant alert and updated the state.
                //   // We can break out of the loop since we only care about the most recent relevant alert.

                // }
                payload = currentObject;
                // Serial.printf(">>> found relevant jsonobject - object #: %d\n", count);
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
      Serial.printf("stream available %d , stream connected %d\n", stream.available(), stream.connected());
      //Serial.printf(">>> Finished reading stream. Total objects read: %d\n", count);
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

#ifndef LED_PIN
#define LED_PIN 48
#endif

#define NUM_LEDS 15
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

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

    for (int i = 0; i < NUM_LEDS; i++)
    {
      switch (currentState)
      {
      case STATE_UNCONNECTED:
        if (ledState)
          strip.setPixelColor(i, strip.Color(0, 0, 120));
        else
          strip.setPixelColor(i, strip.Color(0, 0, 60));
        break;

      case STATE_NO_ALERTS:
        strip.setPixelColor(i, strip.Color(0, 1, 0));
        break;

      case STATE_WARNING:
        strip.setPixelColor(i, strip.Color(255, 255, 0));
        break;
      case STATE_ALERT_ROCKETS:
        if (ledState)
          strip.setPixelColor(i, strip.Color(100, 0, 0));
        else
          strip.setPixelColor(i, strip.Color(20, 0, 0));
        break;

      case STATE_ALERT_GENERAL:
        if (ledState)
          strip.setPixelColor(i, strip.Color(30, 15, 30));
        else
          strip.setPixelColor(i, strip.Color(60, 0, 0));
        break;

      case STATE_EVENT_ENDED:
        if (ledState)
          strip.setPixelColor(i, strip.Color(0, 255, 0));
        else
          strip.setPixelColor(i, strip.Color(0, 50, 0));
        break;
      }
    }
    strip.show();
  }
}

const int buzzerPin = 5;
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

#ifndef DEVICE_HOSTNAME
#define DEVICE_HOSTNAME "lion-roar_test" // Fallback just in case
#endif

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

// משתני סימולציה
// bool simulation = false;
// String jsonStr = "";
// void doSimulationStep()
// {
//   // ==========================================
//   // קוד סימולציה להזרקת JSON דרך Serial Monitor
//   // ==========================================
//   if (Serial.available() > 0)
//   {
//     char inChar = Serial.read();
//     if (inChar >= '0' && inChar <= '4')
//     {
//       simulation = true;
//       Serial.println("\n==================================");
//       Serial.printf(">>> SIMULATION MODE ON: Injected JSON #%c\n", inChar);
//       Serial.println("==================================\n");
//       String fakeTime = String(millis());
//       switch (inChar)
//       {
//       case '0':
//         jsonStr = "[]";
//         break;
//       case '1':
//         jsonStr = "[{\"alertDate\":\"" + fakeTime + "\",\"title\":\"ירי רקטות וטילים\",\"data\":\"טל - אל דרום\",\"category\":1}]";
//         break;
//       case '2':
//         jsonStr = "[{\"alertDate\":\"" + fakeTime + "\",\"title\":\"ירי רקטות וטילים\",\"data\":\"טל - אל\",\"category\":1}]";
//         break;
//       case '3':
//         jsonStr = "[{\"alertDate\":\"" + fakeTime + "\",\"title\":\"חדירת כלי טיס עוין\",\"data\":\"טל - אל\",\"category\":2}]";
//         break;
//       case '4':
//         jsonStr = "[{\"alertDate\":\"" + fakeTime + "\",\"title\":\"האירוע הסתיים\",\"data\":\"טל - אל\",\"category\":13}]";
//         break;
//       }
//     }
//     else if (inChar == '9')
//     {
//       simulation = false;
//       Serial.println("\n==================================");
//       Serial.println(">>> SIMULATION MODE OFF: Returning to Live API");
//       Serial.println("==================================\n");
//     }
//   }
// }

unsigned long API_CHECK_INTERVAL = 5000;
static unsigned long lastApiCheck = 0;
void loop()
{
  API_CHECK_INTERVAL = (currentState == STATE_NO_ALERTS) ? 5000 : 1000;
  connectToWifiIfNeeded();
  ArduinoOTA.handle();

  // --- 1. טיימר למשיכת נתונים כל כמה שניות ---
  if (millis() - lastApiCheck >= API_CHECK_INTERVAL)
  {
    lastApiCheck = millis();

    if (WiFi.status() == WL_CONNECTED /*|| simulation*/)
    {
      String jsonPayload = fetchAlertJson();

      // Serial.printf("---> %s\n", jsonPayload.c_str());
      // comm error:
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

  // --- 2. טיפול במצב סיום אירוע ---
  if (currentState == STATE_EVENT_ENDED)
  {
    if (millis() - eventEndedStartTime > 30000)
    {
      currentState = STATE_NO_ALERTS;
      Serial.println(">>> Return to Normal Routine (STATE_NO_ALERTS).");
    }
  }

  // --- 3. עדכון החומרה ---
  operateLEDs();
  // operateBuzzer();
}