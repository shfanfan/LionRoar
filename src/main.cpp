#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h> // מומלץ גרסה 6 ומעלה
#include <Adafruit_NeoPixel.h>

// ==========================================
// הגדרות כלליות ורשת
// ==========================================
const char* WIFI_SSID     = "<your_wifi_ssid>"; // שנה ל-SSID של הרשת שלך
const char* WIFI_PASSWORD = "<your_wifi_password>"; // שנה ל-סיסמה של הרשת שלך

// שם האיזור (אפשר לתת רק תחילית, למשל "תל אביב" יתפוס גם "תל אביב - מזרח")
const String MY_AREA = "טל - אל"; // שנה לשם האיזור שלך "נוף הגליל"

// ==========================================
// הגדרת מכונת המצבים (State Machine)
// ==========================================
enum AlertState {
  STATE_UNCONNECTED,    // מצב התחלתי - ממתין לנתונים ראשונים מהשרת
  STATE_NO_ALERTS,      // מצב שגרה - אין התרעות
  STATE_ALERT_ROCKETS,  // התרעת ירי רקטות וטילים
  STATE_ALERT_GENERAL,  // התרעה כללית (חדירת כלי טיס, רעידת אדמה וכו')
  STATE_EVENT_ENDED     // סיום אירוע (מצב זמני שמוצג אחרי שההתרעה יורדת)
};

AlertState currentState = STATE_UNCONNECTED;
unsigned long eventEndedStartTime = 0; // טיימר למצב סיום אירוע

// ==========================================
// 1. פונקציית משיכת ה-JSON מהשרת
// ==========================================
const char* SERVER_URL = "https://www.oref.org.il/WarningMessages/alert/alerts.json";

String fetchAlertJson() {
  String payload = "ERROR"; // שינוי: ברירת מחדל היא שגיאה, ולא מחרוזת ריקה
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure(); // דילוג על אימות תעודת SSL
    
    HTTPClient http;
    http.begin(client, SERVER_URL);
    
    http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    http.addHeader("Accept", "application/json, text/plain, */*");
    http.addHeader("Referer", "https://www.oref.org.il/");
    http.addHeader("X-Requested-With", "XMLHttpRequest");
    http.addHeader("Connection", "close"); // מניעת "נזילת" שקעי תקשורת

    int httpCode = http.GET();
    if (httpCode == 200) {
      payload = http.getString();
    } else {
      Serial.printf("HTTP GET Failed, error code: %d\n", httpCode);
    }
    http.end();
  }


    // --- ניקוי המחרוזת מתווים נסתרים (BOM) ---
  payload.trim(); 
  if (payload.startsWith("\xEF\xBB\xBF")) {
    payload = payload.substring(3); 
  }
  
  //debug
  if (payload != "" && payload != "ERROR") {
    Serial.printf("Fetched JSON Payload: %s\n", payload.c_str());
  }

  return payload;
}


// ==========================================
// 2. פונקציית פענוח ה-JSON ועדכון המצב
// ==========================================
// הפונקציה מחפשת את התחילית של האיזור המוגדר.
// אם היא מוצאת, היא מדפיסה את המידע הרלוונטי ומשנה מצב.
void parseAlertJsonAndUpdateState(String payload) {
  // אם קיבלנו שגיאה מפורשת ממשיכת הנתונים, לא משנים מצב מ-UNCONNECTED

  if (payload == "ERROR") {
    return;
  }

  // אם התשובה קצרה מאוד (לרוב קובץ ריק או רק "{}"), אין התרעות פעילות בארץ
  if (payload.length() <= 10) {
    if (currentState == STATE_UNCONNECTED) {
      currentState = STATE_NO_ALERTS;
      Serial.println(">>> Initial fetch successful (No alerts). Moved to STATE_NO_ALERTS.");
    } else if (currentState == STATE_ALERT_ROCKETS || currentState == STATE_ALERT_GENERAL) {
      currentState = STATE_EVENT_ENDED;
      eventEndedStartTime = millis();
      Serial.println(">>> Event Ended (No alerts nationwide). Transitioning to EVENT_ENDED state.");
    }
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Serial.println("Failed to parse JSON!");
    return;
  }

  JsonArray data = doc["data"].as<JsonArray>();
  String title = doc["title"].as<String>();
  String cat = doc["cat"].as<String>();
  
  bool areaFound = false;

  for (JsonVariant v : data) {
    String city = v.as<String>();
    if (city.startsWith(MY_AREA)) {
      areaFound = true;
      break; 
    }
  }

  if (areaFound) {
    Serial.println("\n--- RELEVANT ALERT FOUND! ---");
    serializeJsonPretty(doc, Serial);
    Serial.println("\n-----------------------------");

    if (cat == "1" || title.indexOf("רקטות") >= 0) {
      currentState = STATE_ALERT_ROCKETS;
    } else {
      currentState = STATE_ALERT_GENERAL;
    }
  } else {
    // יש התרעות בארץ, אבל לא באיזור שלנו
    if (currentState == STATE_UNCONNECTED) {
      currentState = STATE_NO_ALERTS;
      Serial.println(">>> Initial fetch successful (Alerts elsewhere). Moved to STATE_NO_ALERTS.");
    } else if (currentState == STATE_ALERT_ROCKETS || currentState == STATE_ALERT_GENERAL) {
      currentState = STATE_EVENT_ENDED;
      eventEndedStartTime = millis();
      Serial.println(">>> Alert for our area cleared. Transitioning to EVENT_ENDED state.");
    }
  }
}

// ==========================================
// 3. פונקציית ניהול הלדים (Addressable LED)
// ==========================================
#define LED_PIN     37
#define NUM_LEDS    2
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

void operateLEDs() {
  static unsigned long lastLedUpdate = 0;
  static bool ledState = false;
  
  // קצב הבהוב משתנה בהתאם למצב
  unsigned long blinkInterval = 500;
  if (currentState == STATE_ALERT_ROCKETS) blinkInterval = 200;
  else if (currentState == STATE_UNCONNECTED) blinkInterval = 1000; // הבהוב כחול איטי בזמן המתנה לנתונים

  if (millis() - lastLedUpdate >= blinkInterval) {
    lastLedUpdate = millis();
    ledState = !ledState; // החלפת מצב דלוק/כבוי

    for (int i = 0; i < NUM_LEDS; i++) {
      switch (currentState) {
        case STATE_UNCONNECTED:
          // אור כחול מהבהב לאט - מסמן נסיון התחברות לשרת להבאת נתונים ראשוניים
          if (ledState) strip.setPixelColor(i, strip.Color(0, 0, 255));
          else strip.setPixelColor(i, strip.Color(0, 0, 0));
          break;

        case STATE_NO_ALERTS:
          strip.setPixelColor(i, strip.Color(0, 0, 0)); // כבוי בשגרה
          break;
          
        case STATE_ALERT_ROCKETS:
          if (ledState) strip.setPixelColor(i, strip.Color(255, 0, 0));
          else strip.setPixelColor(i, strip.Color(0, 0, 0));
          break;
          
        case STATE_ALERT_GENERAL:
          if (ledState) strip.setPixelColor(i, strip.Color(255, 165, 0));
          else strip.setPixelColor(i, strip.Color(0, 0, 0));
          break;

        case STATE_EVENT_ENDED:
          strip.setPixelColor(i, strip.Color(0, 255, 0));
          break;
      }
    }
    strip.show();
  }
}


// ==========================================
// 4. פונקציית ניהול הזמזם (Buzzer)
// ==========================================
#define BUZZER_PIN 25

void operateBuzzer() {
  static unsigned long lastBuzzerUpdate = 0;
  static bool buzzerState = false;
  
  unsigned long buzzInterval = (currentState == STATE_ALERT_ROCKETS) ? 500 : 1000;

  if (currentState == STATE_NO_ALERTS || currentState == STATE_EVENT_ENDED || currentState == STATE_UNCONNECTED) {
    // השתקה בשגרה, בסיום אירוע או בהמתנה לחיבור
    digitalWrite(BUZZER_PIN, LOW);
    return;
  }

  if (millis() - lastBuzzerUpdate >= buzzInterval) {
    lastBuzzerUpdate = millis();
    buzzerState = !buzzerState;
    digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
  }
}


// ==========================================
// SETUP & LOOP
// ==========================================

void setup() {
  Serial.begin(115200);
  
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  
  strip.begin();
  strip.show();

  Serial.println("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.printf("attempting to connect wifi (%s)...", WIFI_SSID);
  }
  Serial.println("\nWiFi Connected!");
}

void loop() {
  // --- 1. טיימר למשיכת נתונים כל כמה שניות ---
  static unsigned long lastApiCheck = 0;
  const unsigned long API_CHECK_INTERVAL = 10000; //  milliseconds delay between API checks
  if (millis() - lastApiCheck >= API_CHECK_INTERVAL) {
    lastApiCheck = millis();
    
    String jsonPayload = fetchAlertJson();
    parseAlertJsonAndUpdateState(jsonPayload);
  }

  // --- 2. טיפול במצב סיום אירוע (חזרה לשגרה אחרי זמן מסוים) ---
  if (currentState == STATE_EVENT_ENDED) {
    // נשארים במצב 'סיום אירוע' (למשל אור ירוק) למשך 10 שניות ואז חוזרים לשגרה
    if (millis() - eventEndedStartTime > 10000) { 
      currentState = STATE_NO_ALERTS;
      Serial.println(">>> Return to Normal Routine (STATE_NO_ALERTS).");
    }
  }

  // --- 3. עדכון החומרה לפי מכונת המצבים הנוכחית ---
  operateLEDs();
  operateBuzzer();
}