#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h> 
#include <Adafruit_NeoPixel.h>

// ==========================================
// הגדרות כלליות ורשת
// ==========================================
const char* WIFI_SSID     = "Harari";
const char* WIFI_PASSWORD = "10203040";

// שם האיזור 
const String MY_AREA = "טל - אל"; 

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

// משתנה חדש: שמירת תאריך ההתראה האחרונה כדי לא להפעיל אזעקות על אירועי עבר
String lastAlertDate = ""; 

// ==========================================
// 1. פונקציית משיכת ה-JSON מהשרת
// ==========================================
const char* SERVER_URL = "https://www.oref.org.il/warningMessages/alert/History/AlertsHistory.json";

String fetchAlertJson() {
  String payload = "ERROR"; 
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure(); // דילוג על אימות תעודת SSL
    
    HTTPClient http;
    http.begin(client, SERVER_URL);
    
    http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    http.addHeader("Accept", "application/json, text/plain, */*");
    http.addHeader("Referer", "https://www.oref.org.il/");
    http.addHeader("X-Requested-With", "XMLHttpRequest");
    http.addHeader("Connection", "close"); 

    int httpCode = http.GET();
    if (httpCode == 200) {
      payload = http.getString();
    } else {
      Serial.printf("HTTP GET Failed, error code: %d\n", httpCode);
    } 
    
    http.end();
    client.stop(); // <--- התיקון החשוב: משחרר את חיבור הרשת ומונע קריסת DNS!
  }

  // --- ניקוי המחרוזת מתווים נסתרים (BOM) ---
  payload.trim(); 
  if (payload.startsWith("\xEF\xBB\xBF")) {
    payload = payload.substring(3); 
  }
  
  //debug
  if (payload != "" && payload != "ERROR") {
    Serial.printf("Fetched JSON Payload: %s\n", payload.c_str());
  } else {
    Serial.print("{}");
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

  // אם התשובה קצרה מאוד (לרוב קובץ ריק או רק "[]"), משמעותה הצלחה בתקשורת אך אין נתונים
  if (payload.length() <= 10) {
    if (currentState == STATE_UNCONNECTED) {
      currentState = STATE_NO_ALERTS;
      Serial.println(">>> Initial fetch successful (Empty history). Moved to STATE_NO_ALERTS.");
    }
    return;
  }

    //--------------------------------------------------------------------------------
    //debug
    Serial.printf("Fetched JSON Payload: %s\n", payload.c_str());
    //--------------------------------------------------------------------------------


  // פירסור ה-JSON (מותאם לגרסה 7 - הספרייה מנהלת את הזיכרון לבד)
  JsonDocument doc; 
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    // טיפול ב-EmptyInput מצד הפירסר (מקביל לקובץ ריק)
    if (error == DeserializationError::EmptyInput) {
      if (currentState == STATE_UNCONNECTED) {
        currentState = STATE_NO_ALERTS;
        Serial.println(">>> Initial fetch successful (EmptyInput). Moved to STATE_NO_ALERTS.");
      }
      return;
    }
    
    Serial.print("Failed to parse JSON: ");
    Serial.println(error.c_str());
    return;
  }

  JsonArray alerts = doc.as<JsonArray>();
  bool areaFound = false;

  // מעבר על מערך ההיסטוריה (הקובץ מסודר מההתראה החדשה ביותר לישנה ביותר)
  for (JsonObject alert : alerts) {
    String dataField = alert["data"].as<String>();
    
    // בודק אם השדה מתחיל בשם היישוב שהגדרנו (תופס גם הרחבות כמו "טל - אל")
    if (dataField.startsWith(MY_AREA)) {
      areaFound = true;
      String currentAlertDate = alert["alertDate"].as<String>();
      
      // אם אנחנו בהפעלה ראשונה - רק מסנכרנים את התאריך ועוברים לשגרה
      if (currentState == STATE_UNCONNECTED) {
        lastAlertDate = currentAlertDate;
        currentState = STATE_NO_ALERTS;
        Serial.println(">>> Initial fetch successful. Synced alert history. Moved to STATE_NO_ALERTS.");
      } 
      // זיהוי התראה *חדשה* לפי התאריך (אם התאריך שונה ממה ששמרנו בפעם הקודמת)
      else if (currentAlertDate != lastAlertDate) {
        lastAlertDate = currentAlertDate;
        
        int category = alert["category"].as<int>();
        String title = alert["title"].as<String>();
        
        Serial.println("\n--- NEW RELEVANT ALERT FOUND! ---");
        Serial.println("Time: " + currentAlertDate);
        Serial.println("Title: " + title);
        Serial.println("---------------------------------");

        if (category == 1 || title.indexOf("רקטות") >= 0) {
          currentState = STATE_ALERT_ROCKETS;
        } else if (category == 13 || title == "האירוע הסתיים") {
          currentState = STATE_EVENT_ENDED;
          eventEndedStartTime = millis();
          Serial.println(">>> Event Ended alert received. Transitioning to EVENT_ENDED state.");
        } else {
          currentState = STATE_ALERT_GENERAL;
        }
      }
      
      // מצאנו את ההתראה האחרונה ביותר ליישוב שלנו בהיסטוריה - אפשר לעצור את הסריקה
      break; 
    }
  }

  // אם המערך נסרק לחלוטין והיישוב שלנו לא הופיע בו בכלל, וזו הפעלה ראשונה:
  if (!areaFound && currentState == STATE_UNCONNECTED) {
      currentState = STATE_NO_ALERTS;
      Serial.println(">>> Initial fetch successful (Area not in history). Moved to STATE_NO_ALERTS.");
  }
}

// ==========================================
// 3. פונקציית ניהול הלדים (Addressable LED)
// ==========================================
#define LED_PIN     17
#define NUM_LEDS    10
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

void operateLEDs() {
  static unsigned long lastLedUpdate = 0;
  static bool ledState = false;
  
  // קצב הבהוב משתנה בהתאם למצב
  unsigned long blinkInterval = 500;
  if (currentState == STATE_ALERT_ROCKETS) blinkInterval = 200;
  else if (currentState == STATE_UNCONNECTED) blinkInterval = 1000; 

  if (millis() - lastLedUpdate >= blinkInterval) {
    lastLedUpdate = millis();
    ledState = !ledState; 

    for (int i = 0; i < NUM_LEDS; i++) {
      switch (currentState) {
        case STATE_UNCONNECTED:
          if (ledState) strip.setPixelColor(i, strip.Color(0, 0, 255));
          else strip.setPixelColor(i, strip.Color(0, 0, 0));
          break;

        case STATE_NO_ALERTS:
          strip.setPixelColor(i, strip.Color(0, 0, 0)); 
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

const int buzzerPin = 5;
const int ledcChannel = 0;
const int resolution = 8;
const int freq = 2000;


void operateBuzzer() {
  static unsigned long lastBuzzerUpdate = 0;
  static bool buzzerState = false;
  
  unsigned long buzzInterval = (currentState == STATE_ALERT_ROCKETS) ? 500 : 1000;

  if (currentState == STATE_NO_ALERTS || currentState == STATE_EVENT_ENDED || currentState == STATE_UNCONNECTED) {
    ledcWriteTone(ledcChannel, 0);
    return;
  }

  if (millis() - lastBuzzerUpdate >= buzzInterval) {
    lastBuzzerUpdate = millis();
    buzzerState = !buzzerState;
    ledcWriteTone(ledcChannel, buzzerState ? 1000 : 0);
  }
}


// ==========================================
// SETUP & LOOP
// ==========================================

void setup() {
  Serial.begin(115200);
  
  //define buzzer
  ledcSetup(ledcChannel, freq, resolution);
  ledcAttachPin(buzzerPin, ledcChannel);
  //test buzzer
  ledcWriteTone(ledcChannel, 1000);
  delay(1000);
  ledcWriteTone(ledcChannel, 0);

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

// ==========================================
// משתני סימולציה
// ==========================================
bool simulation = false;
String jsonStr = "";
int networkErrorCount = 0; // מונה שגיאות רשת ברצף


void loop() {
  // ==========================================
  // קוד סימולציה להזרקת JSON דרך Serial Monitor
  // ==========================================
  if (Serial.available() > 0) {
    char inChar = Serial.read();
    
    if (inChar >= '0' && inChar <= '4') {
      simulation = true;
      Serial.println("\n==================================");
      Serial.printf(">>> SIMULATION MODE ON: Injected JSON #%c\n", inChar);
      Serial.println("==================================\n");
      
      String fakeTime = String(millis()); 
      
      switch (inChar) {
        case '0': jsonStr = "[]"; break;
        case '1': jsonStr = "[{\"alertDate\":\"" + fakeTime + "\",\"title\":\"ירי רקטות וטילים\",\"data\":\"טל - אל דרום\",\"category\":1}]"; break;
        case '2': jsonStr = "[{\"alertDate\":\"" + fakeTime + "\",\"title\":\"ירי רקטות וטילים\",\"data\":\"טל - אל\",\"category\":1}]"; break;
        case '3': jsonStr = "[{\"alertDate\":\"" + fakeTime + "\",\"title\":\"חדירת כלי טיס עוין\",\"data\":\"טל - אל\",\"category\":2}]"; break;
        case '4': jsonStr = "[{\"alertDate\":\"" + fakeTime + "\",\"title\":\"האירוע הסתיים\",\"data\":\"טל - אל\",\"category\":13}]"; break;
      }
    } 
    else if (inChar == '9') {
      simulation = false;
      Serial.println("\n==================================");
      Serial.println(">>> SIMULATION MODE OFF: Returning to Live API");
      Serial.println("==================================\n");
    }
  }

  // --- משתני זמן גלובליים בתוך הלופ ---
  static unsigned long lastWiFiAttempt = 0;
  static unsigned long lastApiCheck = 0;
  const unsigned long API_CHECK_INTERVAL = 10000; // 10 seconds delay

  // ==========================================
  // מנגנון התאוששות רשת (Auto-Recovery)
  // ==========================================
  if (WiFi.status() != WL_CONNECTED && !simulation) {
    if (millis() - lastWiFiAttempt >= 10000) {
      lastWiFiAttempt = millis();
      Serial.println("WiFi connection lost! Attempting to reconnect...");
      currentState = STATE_UNCONNECTED; 
      
      WiFi.disconnect();
      delay(100); // השהייה קטנטנה שמאפשרת לאנטנה להתנתק באמת
      WiFi.reconnect(); // פקודה נקייה יותר מ-begin שמאלצת חיבור מחדש
    }
  }

  // --- 1. טיימר למשיכת נתונים כל כמה שניות ---
  if (millis() - lastApiCheck >= API_CHECK_INTERVAL) {
    lastApiCheck = millis();
    
    if (WiFi.status() == WL_CONNECTED || simulation) {
      String jsonPayload = simulation ? jsonStr : fetchAlertJson();
      
      if (jsonPayload == "ERROR" && !simulation) {
        networkErrorCount++;
        Serial.printf("Network error count: %d\n", networkErrorCount);
        
        if (networkErrorCount >= 3) {
          Serial.println(">>> Too many network errors. Forcing HARD WiFi Reset...");
          currentState = STATE_UNCONNECTED;
          
          WiFi.disconnect(true); // ניתוק אגרסיבי כולל מחיקת הגדרות זמניות
          delay(200); 
          WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
          
          networkErrorCount = 0; 
          lastWiFiAttempt = millis(); 
          // אנחנו נאפס גם את זמן בדיקת ה-API כדי שלא ינסה לבדוק מיד לפני שהרשת קמה
          lastApiCheck = millis(); 
        }
      } else {
        networkErrorCount = 0;
        parseAlertJsonAndUpdateState(jsonPayload);
      }
    }
  }

  // --- 2. טיפול במצב סיום אירוע ---
  if (currentState == STATE_EVENT_ENDED) {
    if (millis() - eventEndedStartTime > 5000) { 
      currentState = STATE_NO_ALERTS;
      Serial.println(">>> Return to Normal Routine (STATE_NO_ALERTS).");
    }
  }

  // --- 3. עדכון החומרה ---
  operateLEDs();
  operateBuzzer();
}