/**
 * stage3_ink_firebase_monitor.ino
 *
 * 【Stage 3】stage2(温湿度大表示・危険度4段階・キャラ)をベースに、
 *   - nurikaesu_ink_3 の「時間経過に比例してインクがマス目状に広がる」演出
 *   - nurikaesu_ink_3 の「横方向のワイパーでなぞって拭き取る」お掃除操作
 *   - Wi-Fi 経由で Firebase Realtime Database にアップロードし、Webアプリと繋がる
 * を統合したフル機能版。
 *
 * 【統合方針・意図的に見送った点】
 *   - 危険度モデルは stage2 の computeRiskPoints()/computeDangerLevel() に一本化。
 *     インクの広がり具合(0〜100%)もこの値から換算しているので、危険度4段階・
 *     キャラの色・インクの色・Webアプリに送る数値が、すべて同じ基準で揃っている。
 *   - 「片付けた」の操作は、CLEANED!ボタンのタップから、nurikaesu と同じ
 *     「指で画面を左右になぞると、その列を中心に帯状にインクが拭き取られる」
 *     ワイパー方式に変更した。汚れているマスの80%(WIPE_COMPLETE_RATIO)を
 *     拭いたところで「きれいになった」と判定し、そのタイミングで
 *     音・振動・白い斜めのフラッシュ演出を鳴らす(=インクが消える瞬間に音が鳴る)。
 *     指を離して1.2秒(WIPE_IDLE_RESET_MS)以上経つと、拭きかけの範囲は汚れに
 *     戻る(中途半端な操作を「掃除した」とみなさないため)。
 *   - nurikaesu の Preferences+RTC によるリセット時刻の永続化(再起動をまたいで
 *     覚えておく仕組み)は今回は見送って stage2/3 と同じ millis() ベースのままに
 *     した。RTCの時刻設定が必要になり、そこがうまくいかないと逆に不安定要素が
 *     増えるため。余裕があれば別途対応可能です。
 *   - インクは危険度バナー(上部)の上には乗らないようにしてある(常にステータスが
 *     読めるように)。温湿度の数字とキャラは、時間が経つとインクの下に隠れていく
 *     (=「放置すると見えなくなる」という演出)。ボタンが無くなった分、画面全体が
 *     ワイプ操作の対象になる。
 *
 * 【必要なライブラリ】
 *   - M5Unified
 *   - M5Unit-ENV
 *   - FirebaseClient (mobizt)
 *
 * 【配線】ENV III Unit を Core2 の Port A (Grove) に接続（SDA=G32, SCL=G33）
 * 【ボード設定】Arduino IDE: ツール > ボード > M5Stack-Core2
 * 【設定ファイル】secrets.example.h を secrets.h にコピーして、自分の値を入れてください。
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
 
// インクの広がり(0〜100%)は、リスクポイントをDANGERしきい値で正規化して求める。
// つまりDANGERに達したとき、ちょうどインクが100%(画面ほぼ全面)になる。
static const float INK_STOP_CAUTION = RISK_TH_CAUTION / RISK_TH_DANGER * 100.0f;
static const float INK_STOP_WARNING = RISK_TH_WARNING / RISK_TH_DANGER * 100.0f;
 
// 「1時間」とみなすミリ秒数。
// 本番運用: 3600000.0f (実際の1時間)
// 開発・デモ確認用: 60000.0f にすると「1分=1時間」相当になり、数分でDANGERまで確認できる
static const float MS_PER_HOUR_UNIT = 60000.0f;  // 今はデモ用に短縮中。発表直前に3600000.0fへ戻す
 
// ---------- ワイプ(お掃除)判定の設定 ----------
static const float    WIPE_COMPLETE_RATIO = 0.80f;  // 汚れているマスの何%を拭いたら「掃除完了」とみなすか
static const uint32_t WIPE_IDLE_RESET_MS  = 1200;   // これだけ触れなければ、拭きかけの範囲は汚れに戻る
static const int      WIPE_BAND_COLS = 1;           // ワイパーの帯の太さ(タッチ列の左右に何列分広げるか)
 
// ---------- 状態 ----------
unsigned long g_lastCleanMillis = 0;   // 最後に「片付けた(拭き終えた)」時刻
int  g_dangerLevel   = 0;              // 0=SAFE,1=CAUTION,2=WARNING,3=DANGER
bool g_celebrating   = false;          // お祝い演出中フラグ
unsigned long g_celebrateUntil = 0;
 
bool g_soundEnabled = true;            // サウンドアイコンで切替(振動は常にON)
bool g_soundIconTouchActive = false;   // 今のタッチがサウンドアイコン操作として開始されたか(ワイプに使わないため)
 
int g_debugTouchX = -1, g_debugTouchY = -1; // 今どの座標をタッチしているか(調整用にデバッグ表示する)
 
unsigned long g_lastSensorReadMs = 0;
static const unsigned long SENSOR_INTERVAL_MS = 1000;
 
// ワイパー演出(掃除完了の瞬間、斜めの白いフラッシュを1回だけ掃く)
bool     g_wiperActive = false;
uint32_t g_wiperStartMs = 0;
 
// ---------- 画面レイアウト（Core2横向き 320x240 想定）----------
static const int SCREEN_W = 320;
static const int SCREEN_H = 240;
 
static const int FACE_CX = 235;
static const int FACE_CY = 118;
static const int FACE_R  = 55;
 
// バナー下の隅に置く充電残量表示
static const int BATT_ICON_X = 170, BATT_ICON_Y = 40, BATT_ICON_W = 26, BATT_ICON_H = 13;
 
// 右下の隅に置く、サウンドON/OFFの大きめのタップ領域(押し間違い防止のため広めに取ってある)
static const int SND_ICON_X = 240, SND_ICON_Y = 175, SND_ICON_W = 80, SND_ICON_H = 55;
 
// ---------- 色（RGB565）----------
static const uint16_t COLOR_BG       = 0x0000; // 黒
static const uint16_t COLOR_TEXT     = 0xFFFF; // 白
static const uint16_t COLOR_SAFE     = 0x07E0; // 緑
static const uint16_t COLOR_CAUTION  = 0xFFE0; // 黄
static const uint16_t COLOR_WARNING  = 0xFD20; // 橙
static const uint16_t COLOR_DANGER   = 0xF800; // 赤
static const uint16_t COLOR_OUTLINE  = 0x0000; // 輪郭（黒）
 
static const uint16_t LEVEL_COLOR[4] = {COLOR_SAFE, COLOR_CAUTION, COLOR_WARNING, COLOR_DANGER};
static const char* LEVEL_LABEL[4]    = {"SAFE", "CAUTION", "WARNING", "DANGER"};
 
// ---------- 描画先(ちらつき防止のため、いったんcanvasに描いてから一括で画面へ転送) ----------
M5Canvas canvas(&M5.Display);
 
// ---------- インクのマス目 ----------
static const int COLS = 16, ROWS = 12;     // 横向き 320x240 前提のマス目数
static const int NCELLS = COLS * ROWS;
 
float g_cellTh[NCELLS];       // このマスが「汚れ始める」スコアのしきい値(0-100、シャッフル済み)
float g_cellSizeMul[NCELLS];  // マスごとのインクの大きさのばらつき
float g_cellJx[NCELLS], g_cellJy[NCELLS]; // マスごとの位置のばらつき
 
// ワイプ状態(今の拭き取りセッション中に、そのマスが拭かれたかどうか)
bool     g_cellWiped[NCELLS];
uint32_t g_lastWipeTouchMs = 0;
float    g_lastWipeRatio = 0.0f;
bool     g_wasTouching = false;   // 直前フレームでタッチされていたか(列の補間に使う)
float    g_lastTouchX = -1.0f;    // 直前フレームのタッチX座標
 
float g_cellW = 0, g_cellH = 0; // 1マスの幅・高さ(setup()で画面サイズから算出)
float g_fullInkR = 0;           // インクの最大半径
 
// ============================================================
// 危険度・インクスコア計算
// ============================================================
float computeRiskPoints() {
  unsigned long elapsedMs = millis() - g_lastCleanMillis;
  float hours = elapsedMs / MS_PER_HOUR_UNIT;
 
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
 
// リスクポイントを 0〜100 のインク占有率に正規化(DANGERしきい値で100%になる)
float computeInkScore() {
  float riskPoints = computeRiskPoints();
  float score = riskPoints / RISK_TH_DANGER * 100.0f;
  return constrain(score, 0.0f, 100.0f);
}
 
// ============================================================
// インクの色(緑→黄→橙→赤。危険度のしきい値と同じ位置で切り替わる)
// ============================================================
uint16_t lerpInkColor(float score) {
  struct Stop { float at; uint8_t r, g, b; };
  static const Stop pal[4] = {
    {0.0f,               0x00, 0xE0, 0x00}, // SAFE寄り: 緑
    {INK_STOP_CAUTION,   0xFF, 0xE0, 0x00}, // CAUTION: 黄
    {INK_STOP_WARNING,   0xFF, 0xA6, 0x00}, // WARNING: 橙
    {100.0f,             0xF8, 0x00, 0x00}, // DANGER : 赤
  };
  for (int i = 0; i < 3; i++) {
    if (score >= pal[i].at && score <= pal[i + 1].at) {
      float span = pal[i + 1].at - pal[i].at;
      float t = span > 0.0f ? (score - pal[i].at) / span : 0.0f;
      uint8_t r = pal[i].r + (int)((pal[i + 1].r - pal[i].r) * t);
      uint8_t g = pal[i].g + (int)((pal[i + 1].g - pal[i].g) * t);
      uint8_t b = pal[i].b + (int)((pal[i + 1].b - pal[i].b) * t);
      return canvas.color565(r, g, b);
    }
  }
  return canvas.color565(pal[3].r, pal[3].g, pal[3].b);
}
 
// ============================================================
// キャラクター描画(stage2と同じ。描画先をcanvasに変更しただけ)
// ============================================================
void drawMouth(int cx, int cy, int halfWidth, int amplitude, bool smile, uint16_t color) {
  const int steps = 10;
  int prevX = cx - halfWidth;
  int prevY = cy;
  for (int i = 1; i <= steps; i++) {
    float t = (float)i / steps;
    float curve = 4.0f * (t - 0.5f) * (t - 0.5f) - 1.0f;
    int y = smile ? (cy - (int)(amplitude * curve))
                  : (cy + (int)(amplitude * curve));
    int x = cx - halfWidth + (int)(2 * halfWidth * t);
    canvas.drawLine(prevX, prevY, x, y, color);
    canvas.drawLine(prevX, prevY + 1, x, y + 1, color);
    prevX = x;
    prevY = y;
  }
}
 
void drawFace(int level, bool celebrate) {
  int cx = FACE_CX, cy = FACE_CY, r = FACE_R;
 
  // 顔ベース(危険度に応じて色を変える。SAFE=緑, CAUTION=黄, WARNING=橙, DANGER=赤)
  canvas.fillCircle(cx, cy, r, LEVEL_COLOR[level]);
  canvas.drawCircle(cx, cy, r, COLOR_OUTLINE);
 
  int eyeOffsetX = r / 2;
  int eyeY = cy - r / 5;
  int leftEyeX  = cx - eyeOffsetX;
  int rightEyeX = cx + eyeOffsetX;
 
  if (celebrate) {
    canvas.drawLine(leftEyeX - 8, eyeY,  leftEyeX,     eyeY - 8, COLOR_OUTLINE);
    canvas.drawLine(leftEyeX,     eyeY - 8, leftEyeX + 8, eyeY, COLOR_OUTLINE);
    canvas.drawLine(rightEyeX - 8, eyeY,  rightEyeX,     eyeY - 8, COLOR_OUTLINE);
    canvas.drawLine(rightEyeX,     eyeY - 8, rightEyeX + 8, eyeY, COLOR_OUTLINE);
    drawMouth(cx, cy + r / 3, r / 2, r / 4, true, COLOR_OUTLINE);
    return;
  }
 
  switch (level) {
    case 0:
      canvas.fillCircle(leftEyeX,  eyeY, 5, COLOR_OUTLINE);
      canvas.fillCircle(rightEyeX, eyeY, 5, COLOR_OUTLINE);
      drawMouth(cx, cy + r / 3, r / 2, r / 4, true, COLOR_OUTLINE);
      break;
 
    case 1:
      canvas.fillCircle(leftEyeX,  eyeY, 5, COLOR_OUTLINE);
      canvas.fillCircle(rightEyeX, eyeY, 5, COLOR_OUTLINE);
      drawMouth(cx, cy + r / 3, r / 2, r / 8, true, COLOR_OUTLINE);
      break;
 
    case 2:
      canvas.fillCircle(leftEyeX,  eyeY, 5, COLOR_OUTLINE);
      canvas.fillCircle(rightEyeX, eyeY, 5, COLOR_OUTLINE);
      canvas.drawLine(leftEyeX - 10, eyeY - 14, leftEyeX + 6, eyeY - 8, COLOR_OUTLINE);
      canvas.drawLine(rightEyeX + 10, eyeY - 14, rightEyeX - 6, eyeY - 8, COLOR_OUTLINE);
      canvas.drawLine(cx - r / 2, cy + r / 3, cx + r / 2, cy + r / 3, COLOR_OUTLINE);
      break;
 
    case 3:
    default:
      canvas.fillCircle(leftEyeX,  eyeY, 6, COLOR_OUTLINE);
      canvas.fillCircle(rightEyeX, eyeY, 6, COLOR_OUTLINE);
      canvas.drawLine(leftEyeX - 10, eyeY - 16, leftEyeX + 8, eyeY - 6, COLOR_OUTLINE);
      canvas.drawLine(rightEyeX + 10, eyeY - 16, rightEyeX - 8, eyeY - 6, COLOR_OUTLINE);
      drawMouth(cx, cy + r / 2, r / 2, r / 6, false, COLOR_OUTLINE);
      canvas.fillCircle(cx + r - 6, cy - r + 10, 5, 0x2D7F);
      canvas.fillTriangle(cx + r - 11, cy - r + 10, cx + r - 1, cy - r + 10, cx + r - 6, cy - r - 2, 0x2D7F);
      break;
  }
}
 
// ============================================================
// インクのマス目(出現順をシャッフルして初期化)
// ============================================================
void setupGrid() {
  int order[NCELLS];
  for (int i = 0; i < NCELLS; i++) order[i] = i;
  randomSeed(20260914);
  for (int i = NCELLS - 1; i > 0; i--) {
    int j = random(i + 1);
    int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
  }
  for (int pos = 0; pos < NCELLS; pos++) {
    int cell = order[pos];
    float base = (float)pos / (float)(NCELLS - 1) * 100.0f;
    float jitter = ((float)random(0, 1000) / 1000.0f - 0.5f) * 6.0f;
    g_cellTh[cell] = constrain(base + jitter, 0.0f, 100.0f);
    g_cellSizeMul[cell] = 0.82f + (float)random(0, 1000) / 1000.0f * 0.32f;
    g_cellJx[cell] = ((float)random(0, 1000) / 1000.0f - 0.5f) * 0.3f;
    g_cellJy[cell] = ((float)random(0, 1000) / 1000.0f - 0.5f) * 0.3f;
  }
  memset(g_cellWiped, 0, sizeof(g_cellWiped));
}
 
// バナー(上部)・充電残量表示・サウンドON/OFFボタンの上にはインクを乗せない
// (常にステータス・電池残量が読め、サウンドの切替がいつでもできるように)
bool cellIsProtected(int col, int row) {
  float cx0 = col * g_cellW, cx1 = cx0 + g_cellW;
  float cy0 = row * g_cellH, cy1 = cy0 + g_cellH;
 
  bool overlapsBanner   = !(cy1 < 0 || cy0 > 34);
  bool overlapsBattery  = !(cx1 < 160 || cy1 < 34 || cy0 > 64);
  bool overlapsSoundBtn = !(cx1 < (SND_ICON_X - 10) || cy1 < (SND_ICON_Y - 10));
  return overlapsBanner || overlapsBattery || overlapsSoundBtn;
}
 
void drawInkOverlay() {
  float score = computeInkScore();
  uint16_t color = lerpInkColor(score);
 
  for (int cell = 0; cell < NCELLS; cell++) {
    int col = cell % COLS, row = cell / COLS;
    if (cellIsProtected(col, row)) continue;
    if (g_cellTh[cell] > score) continue;
    if (g_cellWiped[cell]) continue; // 今のワイプで拭き取り済み → 素の情報を見せる
 
    float localGrow = constrain((score - g_cellTh[cell]) / 6.0f, 0.0f, 1.0f);
    float r = g_fullInkR * g_cellSizeMul[cell] * localGrow;
    if (r < 1.0f) continue;
 
    float cx = (col + 0.5f) * g_cellW + g_cellJx[cell] * g_cellW;
    float cy = (row + 0.5f) * g_cellH + g_cellJy[cell] * g_cellH;
    canvas.fillCircle((int)cx, (int)cy, (int)r, color);
  }
}
 
// なぞっている最中、今どの列を拭いているかを白い縦線で示す
void drawWipeIndicator() {
  if (!g_wasTouching || g_soundIconTouchActive) return;
  int H = canvas.height();
  int col = colFromX((int)g_lastTouchX); // colFromX()はこの下で定義(Arduinoが自動でプロトタイプを生成)
  int c0 = constrain(col - WIPE_BAND_COLS, 0, COLS - 1);
  int c1 = constrain(col + WIPE_BAND_COLS, 0, COLS - 1);
  int x0 = (int)(c0 * g_cellW);
  int x1 = (int)((c1 + 1) * g_cellW);
  canvas.drawFastVLine(x0, 0, H, COLOR_TEXT);
  canvas.drawFastVLine(x1, 0, H, COLOR_TEXT);
}
 
// 充電残量アイコン(電池の形+パーセント表示。充電中は"CHG"を添える)
void drawBatteryIndicator() {
  int level = M5.Power.getBatteryLevel(); // 0〜100。取得できない場合は負数
  bool charging = (M5.Power.isCharging() == M5.Power.is_charging);
 
  canvas.drawRect(BATT_ICON_X, BATT_ICON_Y, BATT_ICON_W, BATT_ICON_H, COLOR_TEXT);
  canvas.fillRect(BATT_ICON_X + BATT_ICON_W, BATT_ICON_Y + 3, 3, BATT_ICON_H - 6, COLOR_TEXT);
 
  if (level >= 0) {
    int innerW = BATT_ICON_W - 4;
    int fillW = (int)(innerW * constrain(level, 0, 100) / 100.0f);
    uint16_t fillColor = (level <= 20 && !charging) ? COLOR_DANGER : COLOR_TEXT;
    if (fillW > 0) canvas.fillRect(BATT_ICON_X + 2, BATT_ICON_Y + 2, fillW, BATT_ICON_H - 4, fillColor);
  }
 
  canvas.setTextColor(COLOR_TEXT, COLOR_BG);
  canvas.setTextSize(1);
  canvas.setCursor(BATT_ICON_X + BATT_ICON_W + 8, BATT_ICON_Y + 3);
  if (level >= 0) {
    canvas.printf(charging ? "%d%%CHG" : "%d%%", level);
  } else {
    canvas.print("--");
  }
}
 
// サウンドON/OFFボタン(タップで切替。押し間違えないよう、はっきりした色+文字のボタンにしてある)
bool isInSoundIcon(int x, int y) {
  return x >= SND_ICON_X && x <= SND_ICON_X + SND_ICON_W &&
         y >= SND_ICON_Y && y <= SND_ICON_Y + SND_ICON_H;
}
 
void drawSoundIcon() {
  uint16_t btnColor = g_soundEnabled ? COLOR_SAFE : COLOR_DANGER;
 
  canvas.fillRoundRect(SND_ICON_X, SND_ICON_Y, SND_ICON_W, SND_ICON_H, 8, btnColor);
  canvas.drawRoundRect(SND_ICON_X, SND_ICON_Y, SND_ICON_W, SND_ICON_H, 8, COLOR_OUTLINE);
 
  canvas.setTextColor(COLOR_OUTLINE, btnColor);
  canvas.setTextSize(2);
  canvas.setCursor(SND_ICON_X + 8, SND_ICON_Y + 8);
  canvas.print(g_soundEnabled ? "SOUND" : "MUTE");
  canvas.setTextSize(1);
  canvas.setCursor(SND_ICON_X + 8, SND_ICON_Y + 32);
  canvas.print(g_soundEnabled ? "ON (tap)" : "OFF (tap)");
}
 
// 掃除完了の瞬間、斜めの白いフラッシュを画面いっぱいに1回だけ掃く演出
void drawWiperFlash() {
  if (!g_wiperActive) return;
  float t = (millis() - g_wiperStartMs) / 550.0f;
  if (t >= 1.0f) {
    g_wiperActive = false;
    return;
  }
  int W = canvas.width(), H = canvas.height();
  int barW = 26, slant = 40;
  float xOff = -barW - slant + t * (W + barW + slant * 2);
  int x1 = (int)xOff,        y1 = 0;
  int x2 = (int)xOff + barW, y2 = 0;
  int x3 = (int)xOff + barW + slant, y3 = H;
  int x4 = (int)xOff + slant,        y4 = H;
  canvas.fillTriangle(x1, y1, x2, y2, x3, y3, COLOR_TEXT);
  canvas.fillTriangle(x1, y1, x3, y3, x4, y4, COLOR_TEXT);
}
 
// ============================================================
// 画面全体の描画
// ============================================================
void drawScreen() {
  canvas.fillScreen(COLOR_BG);
 
  // --- 上部: 危険度バナー ---
  canvas.fillRect(0, 0, SCREEN_W, 34, LEVEL_COLOR[g_dangerLevel]);
  canvas.setTextColor(COLOR_OUTLINE, LEVEL_COLOR[g_dangerLevel]);
  canvas.setTextSize(3);
  canvas.setCursor(10, 5);
  if (g_celebrating) {
    canvas.print("GOOD JOB!");
  } else {
    canvas.printf("STATUS: %s", LEVEL_LABEL[g_dangerLevel]);
  }
 
  // --- 左側: 温度・湿度を大きな数字で表示 ---
  canvas.setTextColor(COLOR_TEXT, COLOR_BG);
 
  canvas.setTextSize(1);
  canvas.setCursor(10, 40);
  canvas.print("TEMP");
  canvas.setTextSize(4);
  canvas.setCursor(10, 50);
  canvas.printf("%.1fC", g_temperature);
 
  canvas.setTextSize(1);
  canvas.setCursor(10, 95);
  canvas.print("HUM");
  canvas.setTextSize(4);
  canvas.setCursor(10, 105);
  canvas.printf("%.1f%%", g_humidity);
 
  // --- デバッグ用の生数値 ---
  float hoursSinceClean = (millis() - g_lastCleanMillis) / MS_PER_HOUR_UNIT;
  canvas.setTextSize(1);
  canvas.setCursor(10, 150);
  canvas.printf("Since cleaned: %.1f h", hoursSinceClean);
  canvas.setCursor(10, 162);
  canvas.printf("Risk: %.1f  Ink: %.0f%%  Wipe: %.0f%%", computeRiskPoints(), computeInkScore(), g_lastWipeRatio * 100.0f);
  canvas.setCursor(10, 174);
  if (g_debugTouchX >= 0) {
    canvas.printf("Touch: x=%d y=%d", g_debugTouchX, g_debugTouchY);
  } else {
    canvas.print("Slide sideways to wipe clean");
  }
 
  // --- 右側: キャラクター ---
  drawFace(g_dangerLevel, g_celebrating);
 
  // --- 隅の表示: 充電残量とサウンドON/OFF(常にインクの下に隠れない) ---
  drawBatteryIndicator();
  drawSoundIcon();
 
  // --- 最後に、時間経過に応じたインクをここまでの内容の上に重ねる ---
  drawInkOverlay();
  drawWipeIndicator();
  drawWiperFlash();
}
 
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
 
  float hoursSinceClean = (now - g_lastCleanMillis) / MS_PER_HOUR_UNIT;
  String json = "{";
  json += "\"temperature\":" + String(g_temperature, 2) + ",";
  json += "\"humidity\":" + String(g_humidity, 2) + ",";
  json += "\"riskPoints\":" + String(computeRiskPoints(), 3) + ",";
  json += "\"inkScore\":" + String(computeInkScore(), 1) + ",";
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
// 「拭き終えた」瞬間の演出（音・振動・フラッシュ）
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
 
// ワイプで汚れているマスの80%を拭き終えた瞬間に呼ばれる。
// タイマーをリセットし、インクを消し、ここで音・振動・フラッシュを鳴らす
// (=「インクがなくなるとき」に合わせて鳴らす、という要望に対応する場所)
void onCleaned() {
  g_lastCleanMillis = millis();
  g_cleanCount++;
  g_forceFirebaseUpload = true;
  g_dangerLevel = computeDangerLevel(computeRiskPoints());
 
  memset(g_cellWiped, 0, sizeof(g_cellWiped));
  g_lastWipeTouchMs = 0;
  g_lastWipeRatio = 0.0f;
  g_wasTouching = false;
  g_lastTouchX = -1.0f;
 
  g_wiperActive = true;
  g_wiperStartMs = millis();
 
  g_celebrating = true;
  g_celebrateUntil = millis() + 1500;
 
  vibrateShort();               // 振動は常にON
  if (g_soundEnabled) playCleanedSound(); // 音はアイコンでOFFにしていれば鳴らさない
 
  Serial.println("[INK] wipe complete -> score reset");
}
 
// ============================================================
// ワイプ(お掃除)処理(横方向のワイパー)
// ============================================================
int colFromX(int x) {
  return constrain((int)(x / g_cellW), 0, COLS - 1);
}
 
// 指定した列(とその左右WIPE_BAND_COLS列)を、画面の上から下まで「拭かれた」状態にする
void markWipedColumnBand(int col) {
  int c0 = constrain(col - WIPE_BAND_COLS, 0, COLS - 1);
  int c1 = constrain(col + WIPE_BAND_COLS, 0, COLS - 1);
  for (int c = c0; c <= c1; c++) {
    for (int row = 0; row < ROWS; row++) {
      g_cellWiped[c + row * COLS] = true;
    }
  }
}
 
// 現在汚れているマスのうち、何%が拭かれたかを判定。しきい値を超えたら掃除完了とみなす
void checkWipeCompletion() {
  float score = computeInkScore();
  int inked = 0, wipedInked = 0;
  for (int cell = 0; cell < NCELLS; cell++) {
    if (g_cellTh[cell] > score) continue; // このマスはまだ汚れていない(判定対象外)
    inked++;
    if (g_cellWiped[cell]) wipedInked++;
  }
  g_lastWipeRatio = (inked > 0) ? (float)wipedInked / inked : 0.0f;
 
  if (inked > 0 && g_lastWipeRatio >= WIPE_COMPLETE_RATIO) {
    onCleaned();
  }
}
 
// 一定時間タッチが無ければ、拭きかけの範囲を汚れに戻す(=中途半端な操作は無効)
void resetWipeProgress() {
  memset(g_cellWiped, 0, sizeof(g_cellWiped));
  g_lastWipeTouchMs = 0;
  g_lastWipeRatio = 0.0f;
  g_wasTouching = false;
  g_lastTouchX = -1.0f;
}
 
void handleWipeTouch() {
  auto t = M5.Touch.getDetail();
  uint32_t now = millis();
  bool isDown = t.isPressed();
 
  if (isDown) {
    g_debugTouchX = t.x; // 画面下部のデバッグ表示用(タップが実際どこに反応しているか確認できる)
    g_debugTouchY = t.y;
  }
 
  if (isDown && !g_wasTouching) {
    // タッチ開始の瞬間だけ、サウンドボタンへのタップかどうかを判定する
    if (isInSoundIcon(t.x, t.y)) {
      g_soundEnabled = !g_soundEnabled;
      g_soundIconTouchActive = true;
 
      // タップした瞬間に必ず振動で反応を返す(音がOFFの状態でも分かるように)。
      // ONに切り替えたときだけ短いビープも鳴らして「音が出る状態になった」ことを耳でも確認できるようにする
      vibrateShort();
      if (g_soundEnabled) {
        M5.Speaker.setVolume(180);
        M5.Speaker.tone(1200, 90);
        delay(100);
      }
    } else {
      g_soundIconTouchActive = false;
    }
  }
 
  if (isDown) {
    if (g_soundIconTouchActive) {
      // アイコン操作中の指の動きはワイプに使わない(指がずれても掃除が始まらないように)
      g_wasTouching = true;
      g_lastWipeTouchMs = now;
      return;
    }
 
    int col = colFromX(t.x);
    if (g_wasTouching) {
      // 前フレームの位置との間も塗りつぶす(素早くなぞっても列が飛ばないように補間)
      int prevCol = colFromX((int)g_lastTouchX);
      int c0 = min(prevCol, col), c1 = max(prevCol, col);
      for (int c = c0; c <= c1; c++) markWipedColumnBand(c);
    } else {
      markWipedColumnBand(col);
    }
    g_lastTouchX = (float)t.x;
    g_wasTouching = true;
    g_lastWipeTouchMs = now;
    checkWipeCompletion();
  } else {
    g_wasTouching = false;
    g_soundIconTouchActive = false;
    if (g_lastWipeTouchMs != 0 && (now - g_lastWipeTouchMs > WIPE_IDLE_RESET_MS)) {
      resetWipeProgress();
    }
  }
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
 
  canvas.setColorDepth(16);
  canvas.createSprite(M5.Display.width(), M5.Display.height());
 
  g_cellW = (float)canvas.width() / COLS;
  g_cellH = (float)canvas.height() / ROWS;
  g_fullInkR = min(g_cellW, g_cellH) * 0.68f;
  setupGrid();
 
  g_lastCleanMillis = millis();
  setupFirebase();
}
 
void loop() {
  if (g_firebaseEnabled) g_firebaseApp.loop();
  M5.update();
  handleWipeTouch();
 
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
  }
 
  if (g_celebrating && now > g_celebrateUntil) {
    g_celebrating = false;
  }
 
  drawScreen();
  canvas.pushSprite(0, 0);
 
  delay(33); // 約30fps。インクが滑らかに広がって見えるよう、センサー取得とは別に毎フレーム再描画する
}
 
