/**
 * stage2_kobae_monitor.ino
 *
 * 【Stage 2】stage1(温湿度表示)に以下を追加したフル機能版。
 *   - キャラクター表示（危険度に応じて表情が変化）
 *   - 危険度の4段階表示（SAFE / CAUTION / WARNING / DANGER）
 *   - タッチで「洗い物/ゴミを片付けた」を報告する演出（アニメーション・音・振動）
 *
 * 【危険度の考え方（コバエ・菌の発生リスクを想定した簡易モデル）】
 *   「最後に片付けてからの経過時間」×「気温・湿度による倍率」でリスクポイントを積算する。
 *   気温・湿度が高いほど倍率が上がり、同じ放置時間でもリスクが早く上がる。
 *   画面右下のボタンをタップする＝「片付けた」宣言で、経過時間がリセットされ、
 *   キャラが喜ぶ演出（アニメーション・音・振動）が入る。
 *   ※ しきい値は仮の値。実データやチームでの合意に応じて下の定数を調整してください。
 *
 * 【必要なライブラリ】
 *   - M5Unified
 *   - M5Unit-ENV
 *
 * 【配線】ENV III Unit を Core2 の Port A (Grove) に接続（SDA=G32, SCL=G33）
 * 【ボード設定】Arduino IDE: ツール > ボード > M5Stack-Core2
 */

#include <M5Unified.h>
#include "M5UnitENV.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE
#include <FirebaseClient.h>
#include "secrets.h"

// ---------- センサー ----------
SHT3X sht3x;
static const int PIN_SDA = 32;
static const int PIN_SCL = 33;

float g_temperature = 0.0f;
float g_humidity    = 0.0f;

// ---------- Firebase ----------
WiFiClientSecure g_firebaseSsl;
AsyncClientClass g_firebaseClient(g_firebaseSsl);
UserAuth g_firebaseUser(FIREBASE_API_KEY, FIREBASE_USER_EMAIL, FIREBASE_USER_PASSWORD, 3000);
FirebaseApp g_firebaseApp;
RealtimeDatabase g_database;

bool g_firebaseEnabled = false;
bool g_forceFirebaseUpload = true;
unsigned long g_lastFirebaseUploadMs = 0;
unsigned long g_cleanCount = 0;
static const unsigned long FIREBASE_INTERVAL_MS = 10000;

// ---------- 危険度モデルの調整用パラメータ ----------
// 気温/湿度がこの値以上だと「高リスク環境」とみなし、リスク蓄積スピードを上げる
static const float TEMP_HIGH_C   = 28.0f;
static const float HUM_HIGH_PCT  = 80.0f;
// 気温/湿度がこの値以上だと「やや注意な環境」
static const float TEMP_MID_C    = 23.0f;
static const float HUM_MID_PCT   = 65.0f;

// リスクポイントのしきい値（片付けてからの経過時間×倍率の積算値）
static const float RISK_TH_CAUTION = 2.0f;   // これ未満は SAFE
static const float RISK_TH_WARNING = 6.0f;   // これ未満は CAUTION
static const float RISK_TH_DANGER  = 12.0f;  // これ未満は WARNING、以上は DANGER

// ---------- 状態 ----------
unsigned long g_lastCleanMillis = 0;   // 最後に「片付けた」時刻
int  g_dangerLevel   = 0;              // 0=SAFE,1=CAUTION,2=WARNING,3=DANGER
bool g_celebrating   = false;          // お祝い演出中フラグ
unsigned long g_celebrateUntil = 0;
bool g_prevTouchDown = false;
bool g_needsRedraw   = true;

unsigned long g_lastSensorReadMs = 0;
static const unsigned long SENSOR_INTERVAL_MS = 1000;

// ---------- 画面レイアウト（Core2横向き 320x240 想定）----------
static const int SCREEN_W = 320;
static const int SCREEN_H = 240;

static const int FACE_CX = 235;
static const int FACE_CY = 118;
static const int FACE_R  = 55;

// 「片付けた」ボタン
static const int BTN_X = 190, BTN_Y = 190, BTN_W = 120, BTN_H = 42;
// 画面内のゴミ箱アイコン位置（ボタンの左側）
static const int BIN_X = 205, BIN_Y = 195;

// ---------- 色（RGB565）----------
static const uint16_t COLOR_BG       = 0x0000; // 黒
static const uint16_t COLOR_TEXT     = 0xFFFF; // 白
static const uint16_t COLOR_SAFE     = 0x07E0; // 緑
static const uint16_t COLOR_CAUTION  = 0xFFE0; // 黄
static const uint16_t COLOR_WARNING  = 0xFD20; // 橙
static const uint16_t COLOR_DANGER   = 0xF800; // 赤
static const uint16_t COLOR_FACE     = 0xFFE0; // キャラの顔（黄色）
static const uint16_t COLOR_OUTLINE  = 0x0000; // 輪郭（黒）

static const uint16_t LEVEL_COLOR[4] = {COLOR_SAFE, COLOR_CAUTION, COLOR_WARNING, COLOR_DANGER};
static const char* LEVEL_LABEL[4]    = {"SAFE", "CAUTION", "WARNING", "DANGER"};

// ============================================================
// Wi-Fi / Firebase
// ============================================================
bool firebaseSettingsAreFilled() {
  return String(WIFI_SSID) != "YOUR_WIFI_SSID" &&
         String(FIREBASE_API_KEY) != "YOUR_WEB_API_KEY" &&
         String(FIREBASE_DATABASE_URL).startsWith("https://");
}

void firebaseCallback(AsyncResult &result) {
  if (!result.isResult()) return;
  if (result.isError()) {
    Serial.printf("Firebase error: %s (%d)\n",
                  result.error().message().c_str(), result.error().code());
  }
}

void setupFirebase() {
  if (!firebaseSettingsAreFilled()) {
    Serial.println("Firebase settings are not filled. Running offline.");
    return;
  }

  M5.Display.fillScreen(COLOR_BG);
  M5.Display.setTextColor(COLOR_TEXT, COLOR_BG);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 20);
  M5.Display.println("Connecting Wi-Fi...");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 15000) {
    M5.update();
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi connection timed out. Running offline.");
    return;
  }

  g_firebaseSsl.setInsecure();
  g_firebaseSsl.setConnectionTimeout(1000);
  g_firebaseSsl.setHandshakeTimeout(5);

  initializeApp(g_firebaseClient, g_firebaseApp, getAuth(g_firebaseUser), firebaseCallback, "authTask");
  g_firebaseApp.getApp<RealtimeDatabase>(g_database);
  g_database.url(FIREBASE_DATABASE_URL);
  g_firebaseEnabled = true;
  Serial.printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
}

void uploadLatestToFirebase() {
  if (!g_firebaseEnabled || !g_firebaseApp.ready()) return;

  unsigned long now = millis();
  if (!g_forceFirebaseUpload && now - g_lastFirebaseUploadMs < FIREBASE_INTERVAL_MS) return;
  g_forceFirebaseUpload = false;
  g_lastFirebaseUploadMs = now;

  float hoursSinceClean = (now - g_lastCleanMillis) / 3600000.0f;
  String json = "{";
  json += "\"temperature\":" + String(g_temperature, 2) + ",";
  json += "\"humidity\":" + String(g_humidity, 2) + ",";
  json += "\"riskPoints\":" + String(computeRiskPoints(), 3) + ",";
  json += "\"dangerLevel\":" + String(g_dangerLevel) + ",";
  json += "\"dangerLabel\":\"" + String(LEVEL_LABEL[g_dangerLevel]) + "\",";
  json += "\"hoursSinceClean\":" + String(hoursSinceClean, 3) + ",";
  json += "\"cleanCount\":" + String(g_cleanCount) + ",";
  json += "\"updatedAt\":{\".sv\":\"timestamp\"}";
  json += "}";

  String path = "/devices/" + String(DEVICE_ID) + "/latest";
  g_database.set<object_t>(
    g_firebaseClient,
    path,
    object_t(json),
    firebaseCallback,
    "sensorUpload"
  );
}

// ============================================================
// 危険度計算
// ============================================================
float computeRiskPoints() {
  unsigned long elapsedMs = millis() - g_lastCleanMillis;
  float hours = elapsedMs / 3600000.0f;

  float factor = 1.0f;
  if (g_temperature >= TEMP_HIGH_C || g_humidity >= HUM_HIGH_PCT) {
    factor = 2.0f;
  } else if (g_temperature >= TEMP_MID_C || g_humidity >= HUM_MID_PCT) {
    factor = 1.5f;
  }
  return hours * factor;
}

int computeDangerLevel(float riskPoints) {
  if (riskPoints < RISK_TH_CAUTION) return 0;
  if (riskPoints < RISK_TH_WARNING) return 1;
  if (riskPoints < RISK_TH_DANGER)  return 2;
  return 3;
}

// ============================================================
// キャラクター描画
// ============================================================

// なめらかな口を「点をつないだ線」で描く。
// smile=true: 口角が上がる（U字、SAFE寄り）/ false: 口角が下がる（∩字、DANGER寄り）
void drawMouth(int cx, int cy, int halfWidth, int amplitude, bool smile, uint16_t color) {
  const int steps = 10;
  int prevX = cx - halfWidth;
  int prevY = cy;
  for (int i = 1; i <= steps; i++) {
    float t = (float)i / steps;          // 0..1
    float curve = 4.0f * (t - 0.5f) * (t - 0.5f) - 1.0f; // -1(中央)..0(端)
    int y = smile ? (cy - (int)(amplitude * curve))
                  : (cy + (int)(amplitude * curve));
    int x = cx - halfWidth + (int)(2 * halfWidth * t);
    M5.Display.drawLine(prevX, prevY, x, y, color);
    M5.Display.drawLine(prevX, prevY + 1, x, y + 1, color); // 少し太らせる
    prevX = x;
    prevY = y;
  }
}

void drawFace(int level, bool celebrate) {
  int cx = FACE_CX, cy = FACE_CY, r = FACE_R;

  // 顔ベース
  M5.Display.fillCircle(cx, cy, r, COLOR_FACE);
  M5.Display.drawCircle(cx, cy, r, COLOR_OUTLINE);

  int eyeOffsetX = r / 2;
  int eyeY = cy - r / 5;
  int leftEyeX  = cx - eyeOffsetX;
  int rightEyeX = cx + eyeOffsetX;

  if (celebrate) {
    // 喜び顔: ニコッと閉じた目 (^ ^)
    M5.Display.drawLine(leftEyeX - 8, eyeY,  leftEyeX,     eyeY - 8, COLOR_OUTLINE);
    M5.Display.drawLine(leftEyeX,     eyeY - 8, leftEyeX + 8, eyeY, COLOR_OUTLINE);
    M5.Display.drawLine(rightEyeX - 8, eyeY,  rightEyeX,     eyeY - 8, COLOR_OUTLINE);
    M5.Display.drawLine(rightEyeX,     eyeY - 8, rightEyeX + 8, eyeY, COLOR_OUTLINE);
    drawMouth(cx, cy + r / 3, r / 2, r / 4, true, COLOR_OUTLINE);
    return;
  }

  switch (level) {
    case 0: // SAFE: にっこり
      M5.Display.fillCircle(leftEyeX,  eyeY, 5, COLOR_OUTLINE);
      M5.Display.fillCircle(rightEyeX, eyeY, 5, COLOR_OUTLINE);
      drawMouth(cx, cy + r / 3, r / 2, r / 4, true, COLOR_OUTLINE);
      break;

    case 1: // CAUTION: 少し控えめな笑顔
      M5.Display.fillCircle(leftEyeX,  eyeY, 5, COLOR_OUTLINE);
      M5.Display.fillCircle(rightEyeX, eyeY, 5, COLOR_OUTLINE);
      drawMouth(cx, cy + r / 3, r / 2, r / 8, true, COLOR_OUTLINE);
      break;

    case 2: // WARNING: 眉をひそめた真顔
      M5.Display.fillCircle(leftEyeX,  eyeY, 5, COLOR_OUTLINE);
      M5.Display.fillCircle(rightEyeX, eyeY, 5, COLOR_OUTLINE);
      M5.Display.drawLine(leftEyeX - 10, eyeY - 14, leftEyeX + 6, eyeY - 8, COLOR_OUTLINE);
      M5.Display.drawLine(rightEyeX + 10, eyeY - 14, rightEyeX - 6, eyeY - 8, COLOR_OUTLINE);
      M5.Display.drawLine(cx - r / 2, cy + r / 3, cx + r / 2, cy + r / 3, COLOR_OUTLINE);
      break;

    case 3: // DANGER: 困り顔＋汗
    default:
      M5.Display.fillCircle(leftEyeX,  eyeY, 6, COLOR_OUTLINE);
      M5.Display.fillCircle(rightEyeX, eyeY, 6, COLOR_OUTLINE);
      M5.Display.drawLine(leftEyeX - 10, eyeY - 16, leftEyeX + 8, eyeY - 6, COLOR_OUTLINE);
      M5.Display.drawLine(rightEyeX + 10, eyeY - 16, rightEyeX - 8, eyeY - 6, COLOR_OUTLINE);
      drawMouth(cx, cy + r / 2, r / 2, r / 6, false, COLOR_OUTLINE);
      // 汗マーク
      M5.Display.fillCircle(cx + r - 6, cy - r + 10, 5, 0x2D7F);
      M5.Display.fillTriangle(cx + r - 11, cy - r + 10, cx + r - 1, cy - r + 10, cx + r - 6, cy - r - 2, 0x2D7F);
      break;
  }
}

// ============================================================
// 画面全体の描画
// ============================================================
void drawTrashIcon(int x, int y) {
  // 簡易ゴミ箱アイコン
  M5.Display.fillRect(x, y + 6, 20, 16, COLOR_TEXT);
  M5.Display.drawRect(x, y + 6, 20, 16, COLOR_OUTLINE);
  M5.Display.fillRect(x - 2, y + 2, 24, 4, COLOR_TEXT);
  M5.Display.fillRect(x + 6, y - 2, 8, 4, COLOR_TEXT);
}

void drawScreen() {
  M5.Display.fillScreen(COLOR_BG);

  // --- 上部: 危険度バナー ---
  M5.Display.fillRect(0, 0, SCREEN_W, 34, LEVEL_COLOR[g_dangerLevel]);
  M5.Display.setTextColor(COLOR_OUTLINE, LEVEL_COLOR[g_dangerLevel]);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(10, 5);
  if (g_celebrating) {
    M5.Display.print("GOOD JOB!");
  } else {
    M5.Display.printf("STATUS: %s", LEVEL_LABEL[g_dangerLevel]);
  }

  // --- 左側: 温湿度と経過時間 ---
  M5.Display.setTextColor(COLOR_TEXT, COLOR_BG);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 50);
  M5.Display.printf("Temp: %.1f C", g_temperature);
  M5.Display.setCursor(10, 75);
  M5.Display.printf("Hum : %.1f %%", g_humidity);

  float hoursSinceClean = (millis() - g_lastCleanMillis) / 3600000.0f;
  M5.Display.setCursor(10, 110);
  M5.Display.setTextSize(1);
  M5.Display.printf("Since cleaned: %.1f h", hoursSinceClean);
  M5.Display.setCursor(10, 125);
  M5.Display.printf("Risk pts: %.1f", computeRiskPoints());

  // --- 右側: キャラクター ---
  drawFace(g_dangerLevel, g_celebrating);

  // --- 下部: 「片付けた」ボタン ---
  M5.Display.fillRoundRect(BTN_X, BTN_Y, BTN_W, BTN_H, 8, COLOR_SAFE);
  M5.Display.drawRoundRect(BTN_X, BTN_Y, BTN_W, BTN_H, 8, COLOR_OUTLINE);
  M5.Display.setTextColor(COLOR_OUTLINE, COLOR_SAFE);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(BTN_X + 10, BTN_Y + 12);
  M5.Display.print("CLEANED!");

  drawTrashIcon(BTN_X - 30, BTN_Y + 8);
}

// ============================================================
// 「片付けた」タップ時の演出（アニメーション・音・振動）
// ============================================================
void playCleanedSound() {
  M5.Speaker.setVolume(180);
  M5.Speaker.tone(880, 120);
  delay(130);
  M5.Speaker.tone(1046, 120);
  delay(130);
  M5.Speaker.tone(1318, 220);
  delay(150);
}

void vibrateShort() {
  M5.Power.setVibration(200);
  delay(150);
  M5.Power.setVibration(0);
}

// キャラの位置からゴミ箱アイコンへ「お皿」が飛んでいくアニメーション
void playCleanedAnimation() {
  vibrateShort();
  playCleanedSound();

  int startX = FACE_CX, startY = FACE_CY;
  int endX   = BTN_X - 20, endY = BTN_Y + 8;
  const int frames = 12;

  for (int i = 0; i <= frames; i++) {
    float t = (float)i / frames;
    int x = startX + (int)((endX - startX) * t);
    int y = startY + (int)((endY - startY) * t);
    int size = 14 - (int)(10 * t); // だんだん小さくなる

    drawScreen(); // 背景・キャラ・ボタンを再描画してから
    M5.Display.fillRoundRect(x - size / 2, y - size / 2, size, size, 3, 0x8410); // 茶色っぽいお皿
    delay(25);
  }
}

// ============================================================
// タッチ処理
// ============================================================
void handleTouch() {
  auto t = M5.Touch.getDetail();
  bool isDown = t.isPressed();

  if (isDown && !g_prevTouchDown) {
    if (t.x >= BTN_X && t.x <= BTN_X + BTN_W &&
        t.y >= BTN_Y && t.y <= BTN_Y + BTN_H) {
      // 「片付けた」タップ成立
      g_lastCleanMillis = millis();
      g_cleanCount++;
      g_forceFirebaseUpload = true;
      g_dangerLevel = computeDangerLevel(computeRiskPoints());
      playCleanedAnimation();

      g_celebrating = true;
      g_celebrateUntil = millis() + 1500;
      g_needsRedraw = true;
    }
  }
  g_prevTouchDown = isDown;
}

// ============================================================
// setup / loop
// ============================================================
void setup() {
  M5.begin();

  M5.Display.setRotation(1);
  M5.Display.fillScreen(COLOR_BG);
  M5.Display.setTextColor(COLOR_TEXT, COLOR_BG);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 10);
  M5.Display.println("Initializing ENV III...");

  if (!sht3x.begin(&Wire, SHT3X_I2C_ADDR, PIN_SDA, PIN_SCL, 400000U)) {
    M5.Display.setTextColor(TFT_RED, COLOR_BG);
    M5.Display.println("ENV III not found!");
    M5.Display.println("Check Port A wiring");
    while (true) delay(1000);
  }

  g_lastCleanMillis = millis(); // 起動時刻を「片付けた」の基準にする
  setupFirebase();
  g_needsRedraw = true;
}

void loop() {
  if (g_firebaseEnabled) g_firebaseApp.loop();
  M5.update();
  handleTouch();

  unsigned long now = millis();
  if (now - g_lastSensorReadMs >= SENSOR_INTERVAL_MS) {
    g_lastSensorReadMs = now;
    if (sht3x.update()) {
      g_temperature = sht3x.cTemp;
      g_humidity    = sht3x.humidity;
    }
    if (!g_celebrating) {
      g_dangerLevel = computeDangerLevel(computeRiskPoints());
    }
    uploadLatestToFirebase();
    g_needsRedraw = true;
  }

  if (g_celebrating && now > g_celebrateUntil) {
    g_celebrating = false;
    g_needsRedraw = true;
  }

  if (g_needsRedraw) {
    drawScreen();
    g_needsRedraw = false;
  }

  delay(20); // タッチ反応を保つため短めのループ間隔にする
}
