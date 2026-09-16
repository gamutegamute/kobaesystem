/*
  ぬりかえす(ゲーム型・インクメタファー) — 洗い場サイネージ プロトタイプ
  対象: M5Stack Core2 + ENV III ユニット(SHT30, Port A: G32=SDA / G33=SCL)

  必要ライブラリ(Arduino IDE ライブラリマネージャで検索してインストール):
    - M5Unified
    - M5GFX            (M5Unifiedに同梱されます)
  ※ SHT30(ENV III)は外部ライブラリに頼らず、Wireで直接I2Cを叩いて読み出しています。
    "M5_ENV.h" のようなライブラリ依存で詰まりやすい部分なので、あえて自前実装にしています。

  設計方針:
    - 画面の一番下には「キャラクター」と「温湿度の数値」を常に描いておく(素の情報)
    - その上に、放置時間に応じてマス目状のインクが少しずつ広がり、素の情報を覆っていく
      (マスが「出現する順番」だけをあらかじめシャッフルしてあるので、塗り面積(占有率)が
       スコアにほぼ比例する = 加速しない直線的な関係)
    - 掃除の操作は「横方向のワイパー」。指を画面上で左右にスライドさせると、その指のX座標を
      中心にした縦1列分(画面の上から下まで)が帯状にインクごと拭き取られる。指を動かした分だけ
      帯の位置も一緒に動く(前フレームの位置との間も塗りつぶして補間するので、素早くなぞっても
      列が飛ばない)。画面の横幅の大部分(既定80%)を拭き終えたところで「掃除した」と認定し、
      タイマー(g_lastCleanEpoch)をリセットする。実際のワイパーが画面を横に払うのと同じ感覚で、
      1〜2往復なぞるだけで綺麗になる
    - ワイプ中断(指を離して既定時間タッチが無い)場合は、拭きかけの範囲は汚れに戻る
      (=中途半端に触れただけの状態を「掃除した」とみなさない)
*/

#include <M5Unified.h>
#include <Preferences.h>
#include <time.h>
#include <Wire.h>

// ---------------- 基本設定 ----------------
static const float MAX_HOURS = 72.0f;
static const int   SENSOR_INTERVAL_MS = 5000;
static const int   COLS = 12, ROWS = 16;   // 縦向き 240x320 前提のマス目数
static const int   NCELLS = COLS * ROWS;
static const uint8_t SHT30_ADDR = 0x44;    // ENV III のデフォルトアドレス

// ---------------- 開発用: インクの変化速度をブーストする設定 ----------------
// テスト時だけ値を上げると、実時間の流れを速めた状態で変化を確認できます。
// 例: 20.0f なら20倍速(72時間 → 実質3.6時間相当)、100.0f なら100倍速(72時間 → 実質約43分)。
// 本番リリース前に必ず 1.0f に戻してください。
static const float DEV_SPEED_MULTIPLIER = 20.0f;

// ---------------- ワイプ判定の設定 ----------------
static const float    WIPE_COMPLETE_RATIO = 0.80f;   // 汚れているマスの何%を拭いたら「掃除完了」とみなすか
static const uint32_t WIPE_IDLE_RESET_MS  = 1200;    // これだけ触れなければ、拭きかけの範囲は汚れに戻る
static const int      WIPE_BAND_COLS = 1;             // ワイパーの帯の太さ(タッチ列の左右に何列分広げるか)
static const bool     SHOW_DEBUG_HUD = true;          // 開発中は進捗を数値で確認。本番配布時は false に

Preferences prefs;
M5Canvas canvas(&M5.Display);

float g_temp = 25.0f, g_hum = 50.0f;
uint32_t g_lastSensorMs = 0;
uint32_t g_lastCleanEpoch = 0;

bool     g_wiperActive = false;
uint32_t g_wiperStartMs = 0;
bool     g_toastActive = false;
uint32_t g_toastStartMs = 0;

// マスごとの出現しきい値・サイズ・位置ゆらぎ
float   g_cellTh[NCELLS];
float   g_cellSizeMul[NCELLS];
float   g_cellJx[NCELLS], g_cellJy[NCELLS];

// ワイプ状態(セルごとに「今のワイプ中に拭かれたか」を保持)
bool     g_cellWiped[NCELLS];
uint32_t g_lastWipeTouchMs = 0;
float    g_lastWipeRatio = 0.0f;
bool     g_wasTouching = false;   // 直前フレームでタッチされていたか(列の補間に使う)
float    g_lastTouchX = -1.0f;    // 直前フレームのタッチX座標

// 画面サイズから算出するレイアウト値(setup()で計算)
float g_cellW = 0, g_cellH = 0;
float g_fullInkR = 0;      // インクの最大半径

// ---------------- 段階の定義 ----------------
struct Stage { const char* label; };
Stage STAGES[4] = { {"平常"}, {"兆候"}, {"警戒"}, {"危険"} };
int stageIndexFor(int score) {
  if (score < 25) return 0;
  if (score < 50) return 1;
  if (score < 75) return 2;
  return 3;
}

// ---------------- インクの色(シアン→黄→橙→赤紫) ----------------
struct ColorStop { float at; uint8_t r, g, b; };
ColorStop INK_PAL[5] = {
  {0,   0x2F,0xB6,0xC4}, {25, 0xF2,0xC2,0x30}, {50, 0xF2,0x84,0x3C},
  {75,  0xE2,0x3B,0x6E}, {100,0xB0,0x20,0x5A},
};
uint16_t lerpInkColor(float score) {
  for (int i = 0; i < 4; i++) {
    ColorStop a = INK_PAL[i], b = INK_PAL[i+1];
    if (score >= a.at && score <= b.at) {
      float t = (score - a.at) / (b.at - a.at);
      return M5.Display.color565(a.r+(b.r-a.r)*t, a.g+(b.g-a.g)*t, a.b+(b.b-a.b)*t);
    }
  }
  ColorStop last = INK_PAL[4];
  return M5.Display.color565(last.r, last.g, last.b);
}

// ---------------- センサー & スコア(そだてる版と同一ロジック) ----------------
float envMultiplier(float tempC, float hum) {
  float tf = constrain((tempC - 15.0f) / 15.0f, 0.0f, 1.0f);
  float hf = constrain((hum   - 30.0f) / 40.0f, 0.0f, 1.0f);
  return 0.5f + 1.3f * (tf * 0.5f + hf * 0.5f);
}

uint32_t nowEpoch() {
  auto dt = M5.Rtc.getDateTime();
  struct tm t = {};
  t.tm_year = dt.date.year - 1900;
  t.tm_mon  = dt.date.month - 1;
  t.tm_mday = dt.date.date;
  t.tm_hour = dt.time.hours;
  t.tm_min  = dt.time.minutes;
  t.tm_sec  = dt.time.seconds;
  return (uint32_t)mktime(&t);
}

int computeScore() {
  // DEV_SPEED_MULTIPLIER をかけることで、実時間の流れを速めた状態で
  // インクの変化スピードを確認できます(本番では 1.0f に戻してください)。
  float elapsedHours = (nowEpoch() - g_lastCleanEpoch) / 3600.0f * DEV_SPEED_MULTIPLIER;
  float mult = envMultiplier(g_temp, g_hum);
  float s = elapsedHours / MAX_HOURS * 100.0f * mult;
  return (int)round(constrain(s, 0.0f, 100.0f));
}

// SHT30に直接コマンドを送って温湿度を読む(ライブラリ非依存)
// コマンド 0x2C06 = high repeatability, clock stretching無効
bool readSHT30(float &tempOut, float &humOut) {
  Wire.beginTransmission(SHT30_ADDR);
  Wire.write(0x2C);
  Wire.write(0x06);
  if (Wire.endTransmission() != 0) return false;

  delay(15); // 測定完了待ち

  uint8_t n = Wire.requestFrom((int)SHT30_ADDR, 6);
  if (n != 6) return false;

  uint8_t d[6];
  for (int i = 0; i < 6; i++) d[i] = Wire.read();

  uint16_t rawTemp = (d[0] << 8) | d[1];
  uint16_t rawHum  = (d[3] << 8) | d[4];
  tempOut = -45.0f + 175.0f * ((float)rawTemp / 65535.0f);
  humOut  = 100.0f * ((float)rawHum / 65535.0f);
  return true; // CRC(d[2], d[5])は簡略化のため未検証。厳密にやるならCRC8チェックを追加してください
}

void readSensorIfDue() {
  uint32_t now = millis();
  if (now - g_lastSensorMs < SENSOR_INTERVAL_MS && g_lastSensorMs != 0) return;
  g_lastSensorMs = now;
  float t, h;
  if (readSHT30(t, h)) {
    g_temp = t;
    g_hum  = h;
  } else {
    Serial.println("[ENV] SHT30 read failed (配線・アドレスを確認してください)");
  }
  Serial.printf("[ENV] temp=%.1fC hum=%.1f%% score=%d\n", g_temp, g_hum, computeScore());
}

// ---------------- マス目の初期化(出現順をシャッフル) ----------------
void setupGrid() {
  int order[NCELLS];
  for (int i = 0; i < NCELLS; i++) order[i] = i;
  randomSeed(20260914); // 端末ごとに同じ見え方にしたいので固定シード。毎回変えたいなら esp_random() 等に
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
}

// ---------------- 素の情報(キャラクター & 温湿度)の描画 ----------------
// キャラクターは仮の丸顔です。将来的には pushImage() で実際のイラスト/スプライトに
// 置き換えることを想定しています(段階(stg)に応じて表情だけ変える設計にしています)。
// キャラクターの各パーツは r=64 を基準にデザインしたサイズなので、実際の画面の高さから
// 算出した r との比率(k)で全体を拡縮する。こうしておけば、想定と違う解像度(例: 320x240の
// 横向きなど)で動いていても、キャラクターが画面からはみ出したり潰れたりしない。
void drawCharacter(int score) {
  int stg = stageIndexFor(score);
  int W = canvas.width(), H = canvas.height();
  int cx = W / 2;
  int cy = (int)(H * 0.28f);
  int r  = (int)(H * 0.20f);
  float k = r / 64.0f;

  uint16_t faceColor = M5.Display.color565(0xF5, 0xE1, 0xC2); // 肌色(仮)
  canvas.fillCircle(cx, cy, r, faceColor);

  // 目
  int eyeDx = (int)(22 * k), eyeDy = (int)(-8 * k), eyeR = max(2, (int)(7 * k));
  canvas.fillCircle(cx - eyeDx, cy + eyeDy, eyeR, TFT_BLACK);
  canvas.fillCircle(cx + eyeDx, cy + eyeDy, eyeR, TFT_BLACK);

  // まゆ毛(段階が進むほど傾きを強めて心配そうな表情に)
  int browTilt = (int)(stg * 3 * k);
  int browSpan = (int)(14 * k);
  canvas.drawLine(cx - eyeDx - browSpan, cy + eyeDy - browSpan - browTilt, cx - eyeDx + browSpan, cy + eyeDy - browSpan + browTilt, TFT_BLACK);
  canvas.drawLine(cx + eyeDx - browSpan, cy + eyeDy - browSpan + browTilt, cx + eyeDx + browSpan, cy + eyeDy - browSpan - browTilt, TFT_BLACK);

  // 口(段階によって弧の向き・深さを変える)
  int mouthY = cy + (int)(30 * k);
  int arcR0 = max(2, (int)(17 * k)), arcR1 = max(3, (int)(22 * k));
  if (stg == 0) {
    canvas.drawArc(cx, mouthY - (int)(12 * k), arcR0, arcR1, 20, 160, TFT_BLACK);   // 笑顔
  } else if (stg == 1) {
    int half = (int)(20 * k);
    canvas.drawLine(cx - half, mouthY, cx + half, mouthY, TFT_BLACK);               // 真顔
  } else if (stg == 2) {
    canvas.drawArc(cx, mouthY + (int)(14 * k), arcR0, arcR1, 200, 340, TFT_BLACK);  // への字
  } else {
    canvas.drawArc(cx, mouthY + (int)(16 * k), max(2, (int)(19 * k)), max(3, (int)(25 * k)), 190, 350, TFT_BLACK); // 困り顔
    canvas.fillCircle(cx + (int)(42 * k), cy - (int)(16 * k), max(2, (int)(7 * k)), M5.Display.color565(0x6F, 0xC8, 0xE8)); // 汗
  }
}

// 温湿度の表示位置・文字サイズは画面の高さ(H)からの比率で決める。
// 以前は216px/262pxの絶対座標で固定していたが、実機の画面の高さが想定(320px)より
// 小さい場合にその座標が画面の外に出てしまい、湿度だけが完全に見えなくなっていた。
// 比率にしたことでどの高さでも画面内に収まる。
void drawStatsText() {
  int W = canvas.width(), H = canvas.height();
  char buf[32];

  int textSize = constrain((int)(H / 80), 2, 4);
  int y1 = (int)(H * 0.66f);           // 温度の行
  int y2 = (int)(H * 0.66f + H * 0.145f); // 湿度の行(温度の少し下)

  canvas.setTextDatum(middle_center);
  canvas.setTextColor(TFT_WHITE);
  canvas.setTextSize(textSize);

  snprintf(buf, sizeof(buf), "%.1f C", g_temp);
  canvas.drawString(buf, W / 2, y1);

  snprintf(buf, sizeof(buf), "%.0f %%", g_hum);
  canvas.drawString(buf, W / 2, y2);

  // 他の描画関数が使う設定に戻す
  canvas.setTextDatum(top_left);
  canvas.setTextSize(1);
}

void drawBaseLayer(int score) {
  uint16_t bg = M5.Display.color565(0x10, 0x1A, 0x24); // 濃紺の「素の画面」
  canvas.fillSprite(bg);
  drawCharacter(score);
  drawStatsText();
}

// ---------------- インクの描画(ワイプ済みマスは描かない=そこだけ見える) ----------------
void drawInkOverlay(int score) {
  uint16_t color = lerpInkColor(score);

  for (int cell = 0; cell < NCELLS; cell++) {
    if (g_cellTh[cell] > score) continue;   // まだ汚れていないマス
    if (g_cellWiped[cell]) continue;        // 今のワイプで拭き取り済み → 見せる

    float localGrow = constrain((score - g_cellTh[cell]) / 6.0f, 0.0f, 1.0f);
    float r = g_fullInkR * g_cellSizeMul[cell] * localGrow;
    if (r < 1.0f) continue;

    int col = cell % COLS, row = cell / COLS;
    float cx = (col + 0.5f) * g_cellW + g_cellJx[cell] * g_cellW;
    float cy = (row + 0.5f) * g_cellH + g_cellJy[cell] * g_cellH;
    canvas.fillCircle((int)cx, (int)cy, (int)r, color);
  }
}

// ---------------- デバッグHUD(開発中の確認用。本番はSHOW_DEBUG_HUD=falseで非表示) ----------------
void drawHud(int score) {
  if (!SHOW_DEBUG_HUD) return;
  int W = canvas.width(), H = canvas.height();
  int stg = stageIndexFor(score);
  uint16_t color = lerpInkColor(score);

  canvas.fillRoundRect(6, 6, 46, 20, 4, color);
  canvas.setTextDatum(top_left);
  canvas.setTextColor(TFT_BLACK);
  canvas.setTextSize(1);
  canvas.setCursor(12, 12);
  canvas.printf("LV.%d", stg + 1);

  canvas.setTextColor(TFT_WHITE);
  canvas.setCursor(W - 74, 12);
  canvas.printf("%d%% 占有", score);
  canvas.setCursor(W - 74, H - 34);
  canvas.printf("段階:%s", STAGES[stg].label);
  canvas.setCursor(W - 74, H - 20);
  canvas.printf("拭き%d%%", (int)(g_lastWipeRatio * 100));
}

// ---------------- ワイプ中のライブ表示(今なぞっている位置に薄い帯を出す) ----------------
void drawWipeIndicator() {
  if (!g_wasTouching) return;
  int H = canvas.height();
  int col = colFromX((int)g_lastTouchX);
  int c0 = constrain(col - WIPE_BAND_COLS, 0, COLS - 1);
  int c1 = constrain(col + WIPE_BAND_COLS, 0, COLS - 1);
  int x0 = (int)(c0 * g_cellW);
  int x1 = (int)((c1 + 1) * g_cellW);
  uint16_t edge = M5.Display.color565(255, 255, 255);
  canvas.drawFastVLine(x0, 0, H, edge);
  canvas.drawFastVLine(x1, 0, H, edge);
}

// ---------------- ワイパー演出 & トースト(掃除完了時の演出) ----------------
void drawEffects() {
  int W = canvas.width(), H = canvas.height();

  // ワイパー演出(掃除完了の瞬間だけ、斜めの明るい帯を1回だけ掃く)
  if (g_wiperActive) {
    float t = (millis() - g_wiperStartMs) / 550.0f;
    if (t < 1.0f) {
      int barW = 26, slant = 40;
      float xOff = -barW - slant + t * (W + barW + slant * 2);
      int x1 = (int)xOff,        y1 = 0;
      int x2 = (int)xOff + barW, y2 = 0;
      int x3 = (int)xOff + barW + slant, y3 = H;
      int x4 = (int)xOff + slant,        y4 = H;
      uint16_t glow = M5.Display.color565(255, 255, 255);
      canvas.fillTriangle(x1, y1, x2, y2, x3, y3, glow);
      canvas.fillTriangle(x1, y1, x3, y3, x4, y4, glow);
    } else {
      g_wiperActive = false;
    }
  }

  // トースト("WIPE OUT!")
  if (g_toastActive) {
    if (millis() - g_toastStartMs < 1100) {
      int tw = 130, th = 34;
      int tx = W / 2 - tw / 2, ty = H * 0.42;
      canvas.fillRoundRect(tx, ty, tw, th, 8, M5.Display.color565(0xE2, 0x3B, 0x6E));
      canvas.setTextColor(TFT_WHITE);
      canvas.setTextDatum(middle_center);
      canvas.setTextSize(2);
      canvas.drawString("WIPE OUT!", tx + tw / 2, ty + th / 2);
      canvas.setTextDatum(top_left);
      canvas.setTextSize(1);
    } else {
      g_toastActive = false;
    }
  }
}

// ---------------- 「お掃除した」処理(ワイプ完了時に呼ぶ) ----------------
void onCleaned() {
  g_lastCleanEpoch = nowEpoch();
  prefs.putUInt("lastClean", g_lastCleanEpoch);

  memset(g_cellWiped, 0, sizeof(g_cellWiped));
  g_lastWipeTouchMs = 0;
  g_lastWipeRatio = 0.0f;

  g_wiperActive = true; g_wiperStartMs = millis();
  g_toastActive = true; g_toastStartMs = millis();
  Serial.println("[INK] wipe complete -> score reset");
}

// ---------------- ワイプ関連処理(横方向のワイパー) ----------------
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
  int score = computeScore();
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

// 一定時間タッチが無ければ、拭きかけの範囲を汚れに戻す(=中途半端な覗きは無効)
void resetWipeProgress() {
  memset(g_cellWiped, 0, sizeof(g_cellWiped));
  g_lastWipeTouchMs = 0;
  g_lastWipeRatio = 0.0f;
  g_wasTouching = false;
  g_lastTouchX = -1.0f;
}

void handleWipeTouch() {
  if (!M5.Touch.isEnabled()) return;

  auto t = M5.Touch.getDetail();
  uint32_t now = millis();

  if (t.isPressed()) {
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
    if (g_lastWipeTouchMs != 0 && (now - g_lastWipeTouchMs > WIPE_IDLE_RESET_MS)) {
      resetWipeProgress();
    }
  }
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);

  Wire.begin(32, 33); // Core2 Port A

  prefs.begin("nurikaesu", false);
  g_lastCleanEpoch = prefs.getUInt("lastClean", nowEpoch());

  M5.Display.setRotation(1); // 240x320 縦向き想定。横向きで使うなら 0 に
  canvas.setColorDepth(16);
  canvas.createSprite(M5.Display.width(), M5.Display.height());

  // レイアウト値を画面サイズから算出
  g_cellW = (float)canvas.width() / COLS;
  g_cellH = (float)canvas.height() / ROWS;
  g_fullInkR = min(g_cellW, g_cellH) * 0.68f;

  setupGrid();
}

void loop() {
  M5.update();
  readSensorIfDue();
  handleWipeTouch();

  // 掃除完了直後の1フレームだけ、強制的にスコア0(=インク無し)を描く
  int score = g_wiperActive && (millis() - g_wiperStartMs < 40) ? 0 : computeScore();

  drawBaseLayer(score);     // キャラクター & 温湿度(素の情報)
  drawInkOverlay(score);    // その上に汚れ(インク)を重ねる
  drawWipeIndicator();      // 今なぞっているワイパーの位置を表示
  drawHud(score);           // 開発用デバッグ表示(SHOW_DEBUG_HUDで切替)
  drawEffects();            // 掃除完了時の演出

  canvas.pushSprite(0, 0);

  delay(33); // 約30fps
}
