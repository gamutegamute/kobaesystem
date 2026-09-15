/**
 * stage1_env_display.ino
 *
 * 【Stage 1】M5Stack Core2 + ENV III(SHT30) で温湿度を取得し、画面に数値表示するだけの版。
 * まずはセンサーと画面表示が正しく動くことを実機で確認するための最小構成。
 * この後の stage2_kobae_monitor でキャラ表示・4段階の危険度・タッチ操作・音・振動を追加する。
 *
 * 【必要なライブラリ】（Arduino IDE の ライブラリマネージャ で検索してインストール）
 *   - M5Unified        (M5Stack公式)
 *   - M5Unit-ENV        (M5Stack公式。SHT30/QMP6988用)
 *
 * 【配線】
 *   ENV III Unit を Core2 の Port A (Grove, 黄/白のコネクタ) に挿すだけ。
 *   Port A は SDA=G32, SCL=G33 に固定配線されている。
 *
 * 【ボード設定】
 *   Arduino IDE: ツール > ボード > M5Stack-Core2 を選択して書き込む。
 */

#include <M5Unified.h>
#include "M5UnitENV.h"

// ENV III の温湿度センサー(SHT30)
SHT3X sht3x;

// Port A (Grove) の I2C ピン番号（Core2固定）
static const int PIN_SDA = 32;
static const int PIN_SCL = 33;

float g_temperature = 0.0f;  // 直近の気温(℃)
float g_humidity    = 0.0f;  // 直近の湿度(%)

void setup() {
  M5.begin();

  M5.Display.setRotation(1);        // 横向き
  M5.Display.setTextSize(3);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.fillScreen(TFT_BLACK);

  // ENV III (SHT30) 初期化。見つからない場合は画面にエラー表示して停止。
  if (!sht3x.begin(&Wire, SHT3X_I2C_ADDR, PIN_SDA, PIN_SCL, 400000U)) {
    M5.Display.setTextColor(TFT_RED, TFT_BLACK);
    M5.Display.setCursor(10, 10);
    M5.Display.println("ENV III (SHT30)");
    M5.Display.println("not found!");
    M5.Display.println("Check Port A wiring");
    while (true) {
      delay(1000);
    }
  }

  M5.Display.setCursor(10, 10);
  M5.Display.println("ENV III ready");
  delay(500);
}

void loop() {
  M5.update();  // ボタン/タッチ状態の更新（stage2で使用）

  if (sht3x.update()) {
    g_temperature = sht3x.cTemp;
    g_humidity    = sht3x.humidity;

    M5.Display.fillScreen(TFT_BLACK);

    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setCursor(10, 20);
    M5.Display.println("ENV III Monitor");

    M5.Display.setTextSize(5);
    M5.Display.setCursor(10, 70);
    M5.Display.printf("%.1f C\n", g_temperature);

    M5.Display.setCursor(10, 140);
    M5.Display.printf("%.1f %%\n", g_humidity);
    M5.Display.setTextSize(3);

    // シリアルにも出力（PCで確認用）
    Serial.printf("Temp: %.2f C, Hum: %.2f %%\n", g_temperature, g_humidity);
  }

  delay(1000);  // 1秒おきに更新
}
