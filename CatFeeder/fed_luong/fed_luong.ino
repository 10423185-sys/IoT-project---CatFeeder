
#inlcude "secrets.h"
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <RTClib.h>
#include <HX711.h>


bool lowFoodNotified = false;
const char* auth[]    = BLYNK_AUTH_TOKEN;
const char* ssid []    = WIFI_SSID;
const char* password[] = WIFI_PASS;

const int MAX_FEEDS = 10;

int feedHours[MAX_FEEDS];
int feedMinutes[MAX_FEEDS];

int feedCount = 0;

const int SERVO_PIN  = 4;
const int TRIG_PIN   = 33;
const int ECHO_PIN   = 32;
const int HX711_DT   = 18;
const int HX711_SCK  = 19;

Servo      myServo;
HX711      scale;
RTC_DS3231 rtc;

const float CALIBRATION_FACTOR = 420.0;
const float WEIGHT_THRESHOLD_G = 100.0;
const float DEADBAND_G         = 2.0;
const int   AVG_SAMPLES        = 10;

// SRF04/05: tốc độ âm thanh 343m/s = 0.0343 cm/µs
// Khoảng cách = duration * 0.0343 / 2
// Timeout 30ms = ~5m tối đa (SRF05 max range ~4m)
const float  SOUND_SPEED_CM_US = 0.0343;
const long   ECHO_TIMEOUT_US   = 30000;   // 30ms
const float  MIN_DIST_CM       = 2.0;     // SRF04/05 blind zone ~2cm
const float  MAX_DIST_CM       = 400.0;   // SRF05 max ~4m

// Lấy trung bình nhiều lần đo để ổn định
const int    SRF_SAMPLES       = 5;
const int    SRF_SAMPLE_DELAY  = 50;      // ms giữa các lần đo

bool  servoState   = false;
float stableWeight = 0;
int lastFeedMinute = -1;

// ── SRF04/05: trigger pulse >= 10µs, đợi echo ──────────────────────────────
float measureOnce() {
  // Xóa chân TRIG trước
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(5);              // SRF cần LOW rõ ràng trước khi trigger

  // Gửi pulse trigger 10µs
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Đợi echo (HIGH) với timeout
  long duration = pulseIn(ECHO_PIN, HIGH, ECHO_TIMEOUT_US);

  if (duration == 0) return -1.0;   // Timeout → không nhận được echo

  float dist = duration * SOUND_SPEED_CM_US / 2.0;

  // Lọc ngoài vùng hợp lệ của SRF04/05
  if (dist < MIN_DIST_CM || dist > MAX_DIST_CM) return -2.0;

  return dist;
}

// ── Lấy trung bình nhiều mẫu, bỏ giá trị lỗi ──────────────────────────────
float measureAverage() {
  float sum   = 0;
  int   count = 0;

  for (int i = 0; i < SRF_SAMPLES; i++) {
    float d = measureOnce();
    if (d > 0) {
      sum += d;
      count++;
    }
    delay(SRF_SAMPLE_DELAY);
  }

  if (count == 0) return -1.0;      // Tất cả mẫu đều lỗi
  return sum / count;
}

void parseSchedule(String schedule)
{
  feedCount = 0;

  while(schedule.length() > 0 &&
        feedCount < MAX_FEEDS)
  {
    int commaPos = schedule.indexOf(',');

    String item;

    if(commaPos == -1)
    {
      item = schedule;
      schedule = "";
    }
    else
    {
      item = schedule.substring(0, commaPos);
      schedule = schedule.substring(commaPos + 1);
    }

    int colonPos = item.indexOf(':');

    if(colonPos > 0)
    {
      feedHours[feedCount] =
        item.substring(0, colonPos).toInt();

      feedMinutes[feedCount] =
        item.substring(colonPos + 1).toInt();

      Serial.printf(
        "Feed %d = %02d:%02d\n",
        feedCount + 1,
        feedHours[feedCount],
        feedMinutes[feedCount]
      );

      feedCount++;
    }
  }
}

bool isFeedTime()
{
  DateTime now = rtc.now();

  for(int i = 0; i < feedCount; i++)
  {
    if(now.hour() == feedHours[i] &&
       now.minute() == feedMinutes[i])
    {
      return true;
    }
  }

  return false;
}
// ── Gửi kết quả SRF lên Blynk + Serial với thông báo rõ ràng ──────────────


void closeServoAndMeasure(const char* reason)
{
  myServo.write(0);
  servoState = false;
  stableWeight = 0;

  Blynk.virtualWrite(V0, 0);

  Serial.print("Servo: DONG | ");
  Serial.println(reason);

  delay(500); // đợi servo đóng xong

  float dist = measureAverage();

  if(dist > 0)
  {
    Serial.printf("[SRF] Khoang cach: %.1f cm\n", dist);

    Blynk.virtualWrite(V2, dist);

    if(dist > 6.0)
    {
      Serial.println("[SRF] THUC AN SAP HET!");

      Blynk.logEvent(
        "ket_qua",
        "Luong thuc an sap het, vui long them thuc an!"
      );
    }
  }
  else
  {
    Serial.println("[SRF] Loi doc khoang cach");
  }
}

BLYNK_WRITE(V5)
{
  String schedule = param.asStr();

  Serial.print("Lich moi: ");
  Serial.println(schedule);

  parseSchedule(schedule);
}

BLYNK_WRITE(V0) {
  int val = param.asInt();
  if (val == 1) {
    myServo.write(90);
    servoState   = true;
    stableWeight = 0;
    scale.tare();
    Serial.println("Servo: MO 90 | Cho thuc an...");
  } else {
    closeServoAndMeasure("Manual OFF");
  }
}

float readStableWeight() {
  if (!scale.is_ready()) return stableWeight;
  float newWeight = scale.get_units(AVG_SAMPLES);
  if (newWeight < 0) newWeight = 0;
  if (abs(newWeight - stableWeight) > DEADBAND_G) {
    stableWeight = newWeight;
  }
  return stableWeight;
}

void checkLoadCell() {
  float weight = readStableWeight();
  Serial.printf("[HX711] Can nang: %.1f g\n", weight);
  Blynk.virtualWrite(V1, weight);
  if (servoState && weight >= WEIGHT_THRESHOLD_G) {
    Serial.printf("[HX711] Dat %.1fg → Dong servo!\n", weight);
    closeServoAndMeasure("Can >= 100g");
  }
}

void setupRTC() {
  Wire.begin(21, 22);
  if (!rtc.begin()) {
    Serial.println("[RTC] Khong tim thay DS3231!");
    return;
  }
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  rtc.disableAlarm(1);
  rtc.disableAlarm(2);
  rtc.clearAlarm(1);
  rtc.clearAlarm(2);
  DateTime now = rtc.now();
  Serial.printf("[RTC] %04d/%02d/%02d %02d:%02d:%02d\n",
    now.year(), now.month(), now.day(),
    now.hour(), now.minute(), now.second());
}

void setupHX711() {
  scale.begin(HX711_DT, HX711_SCK);
  scale.set_scale(CALIBRATION_FACTOR);
  scale.tare();
  Serial.println("[HX711] San sang, da tare.");
}

// ── Kiểm tra SRF khi khởi động ─────────────────────────────────────────────
void setupSRF() {
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);
  delay(100);

  Serial.println("[SRF] Dang kiem tra cam bien...");
  float dist = measureAverage();

  if (dist < 0) {
    Serial.println("[SRF] CANH BAO: Khong doc duoc gia tri hop le khi khoi dong!");
    Serial.println("      → Kiem tra: day noi TRIG/ECHO, cap nguon 5V, khoang trong truoc sensor");
  } else {
    Serial.printf("[SRF] OK – Khoang cach ban dau: %.1f cm\n", dist);
  }
}


void checkFeedTime()
{
  DateTime now = rtc.now();

  Serial.printf(
    "NOW %02d:%02d:%02d | FeedCount=%d\n",
    now.hour(),
    now.minute(),
    now.second(),
    feedCount
  );

  int currentMinute =
      now.hour() * 60 +
      now.minute();

  if(isFeedTime() &&
     currentMinute != lastFeedMinute)
  {
    Serial.println("DEN GIO CHO AN");

    myServo.write(90);

    servoState = true;

    stableWeight = 0;

    scale.tare();

    lastFeedMinute = currentMinute;

    Blynk.virtualWrite(V0, 1);

    Blynk.logEvent(
      "ket_qua",
      "Bat dau cho an tu dong"
    );
  }
}

void checkFoodLevel()
{
  float dist = measureAverage();

  if(dist > 6.0)
  {
    if(!lowFoodNotified)
    {
      Serial.println("[SRF] LUONG THUC AN SAP HET!");

      Blynk.logEvent(
        "ket_qua",
        "Luong thuc an sap het, vui long them thuc an!"
      );

      lowFoodNotified = true;
    }
  }
  else
  {
    lowFoodNotified = false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  myServo.attach(SERVO_PIN);
  myServo.write(0);
  Blynk.begin(auth, ssid, password);
  Serial.println("[Blynk] Ket noi OK!");
  Blynk.virtualWrite(V2, 0);
  setupRTC();
  setupHX711();
  setupSRF();  
  
  delay(5000);

Blynk.logEvent(
  "ket_qua",
  "TEST THONG BAO"
);         // Chuyển sau cùng, sau pinMode
}

void loop() {
  Blynk.run();
  checkLoadCell();
  checkFeedTime();

  static bool sent = false;

if(!sent)
{
    Blynk.logEvent(
      "ket_qua",
      "TEST EVENT"
    );

    Serial.println("Da gui event");

    sent = true;
}
}