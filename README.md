# M5Stack Core2 + ENV III 実機コード（コバエ・菌リスク可視化）

温湿度をもとに片付けリスク（菌・コバエの発生しやすさ）を4段階表示し、
タッチで「片付けた」を報告するとキャラが喜ぶ、というプロトタイプの実機側コードです。
段階的に進められるよう、2つのスケッチに分けてあります。

## スマホWeb画面（仮データ版）

`kitchen-hazard-monitor.html` をブラウザで開くと、スマホ向けの見守り画面を確認できます。

- 現在の温度・湿度を表示
- 最後に生ごみを捨ててからの時間と温湿度から、危険度を4段階で表示
- 危険度に合わせてキャラクターの表情とメッセージを変更
- 「生ごみを捨てた」ボタンで最終廃棄時刻を更新
- Firebase接続前は仮のセンサー値で動作し、最終廃棄時刻だけブラウザ内に保存

Firebaseへ接続するときは、HTML内の `window.kobaeDataSource` と同じ形で、次の2機能を用意します。

1. `subscribe(onData, onError)`：`temperature`、`humidity`、`lastDisposedAt`、`updatedAt` を画面へ渡す
2. `markDisposed()`：「捨てた」操作の現在時刻をFirebaseへ保存する

画面本体はこの接続方法に依存しないため、仮データ部分だけをFirebase用に差し替えられます。

- `stage1_env_display/` … センサーと画面表示だけの最小構成（まずこれで動作確認）
- `stage2_kobae_monitor/` … キャラ・4段階危険度・タッチ演出・音・振動を追加したフル版
- `stage3_firebase_monitor/` … Stage 2にWi-Fi/Firebase送信を追加したスマホ連携版

スマホ表示は `kitchen-hazard-monitor.html` をFirebase Hostingで公開して使用します。
Firebaseの準備と書き込み方法は `FIREBASE_SETUP.md` を参照してください。

## 必要なもの

- M5Stack Core2
- ENV III Unit（SHT30 + QMP6988。今回はSHT30の温湿度のみ使用）
- Grove(Port A)ケーブルで Core2 と ENV III を接続（SDA=G32, SCL=G33 固定）

## Arduino IDE セットアップ

1. ボードマネージャで `esp32 by Espressif Systems` をインストール
2. ツール > ボード から `M5Stack-Core2` を選択
3. ライブラリマネージャで以下をインストール
   - `M5Unified`（M5Stack公式）
   - `M5Unit-ENV`（M5Stack公式）
4. `stage1_env_display/stage1_env_display.ino` を開いて書き込み → 画面に温度・湿度が出れば配線・センサーはOK
5. 問題なければ `stage2_kobae_monitor/stage2_kobae_monitor.ino` に進む

## 危険度（4段階）の考え方

コバエ・菌は「暖かい・湿度が高い場所に、洗い物や生ゴミを放置する時間が長いほど」発生しやすい、
という前提で、下記のような簡易モデルにしています。

```
リスクポイント = (最後に「片付けた」タップをしてからの経過時間[h]) × 環境倍率

環境倍率:
  気温 >= 28℃ または 湿度 >= 80%  → ×2.0（高リスク環境）
  気温 >= 23℃ または 湿度 >= 65%  → ×1.5（やや注意）
  それ以外                        → ×1.0

判定:
  リスクポイント <  2  → SAFE（緑）
  リスクポイント <  6  → CAUTION（黄）
  リスクポイント < 12  → WARNING（橙）
  リスクポイント >= 12 → DANGER（赤）
```

しきい値・倍率は `stage2_kobae_monitor.ino` 冒頭の定数（`TEMP_HIGH_C` など）にまとめてあるので、
実データやチームの合意に応じて調整してください。あくまで発表用プロトタイプとしての仮モデルです。

なお、経過時間は `millis()` ベースのため **電源を切ると0にリセット**されます。
長時間の連続稼働や電源断をまたいで記録したい場合は、`Preferences`（NVS）に
`g_lastCleanMillis` の代わりに実時刻を保存する形に拡張してください。

## タッチ操作（ゴミを捨てる演出）

画面右下の「CLEANED!」ボタンをタップすると:

1. Core2本体が短く振動（`M5.Power.setVibration`）
2. 「ピロン」という3音の効果音（`M5.Speaker.tone`）
3. お皿がキャラの位置からゴミ箱アイコンへ飛んでいくアニメーション
4. キャラが約1.5秒間、喜び顔（^ ^）になり、バナーが「GOOD JOB!」表示に変わる
5. 経過時間がリセットされ、危険度がSAFEから再計算される

ボタン領域だけをタップ判定にしているので、誤タップ（画面のどこを触ってもリセットされる）は防いでいます。

## 画面表示について（日本語フォントに関する注意）

M5GFX標準の内蔵フォントは英数字のみのため、今回は画面上の文字表示はすべて英語にしています
（コード中のコメントは日本語です）。日本語をキャラ周りやボタンに出したい場合は、
M5GFXの日本語フォント（`lgfx::v1::efont` 系）や、Google Fonts等から生成した `.vlw`/`.ttf` を
SPIFFS/LittleFSに入れて `M5.Display.loadFont()` する方法があります。発表までの時間次第で対応してください。

## 動作確認について

このサンドボックス環境ではネットワークポリシーにより PlatformIO のライブラリレジストリに
アクセスできず、実機ビルドでの自動コンパイル検証はできませんでした。
コード中で使用している主要API（`M5Unified`のDisplay/Touch/Speaker/Power、`M5Unit-ENV`のSHT3X）は
M5Stack公式サンプル・公式ソースコードを直接確認した上で実装していますが、
初回はArduino IDEでのコンパイル・実機書き込みでの動作確認をお願いします。
もしコンパイルエラーが出た場合は、エラーメッセージを共有してもらえればすぐに修正します。

## 次の拡張候補

- `stage3_firebase_monitor/` とWebアプリを使ったFirebaseリアルタイム連携（実装済み、Firebaseプロジェクト設定が必要）
- Web Push / Firebase Cloud Messagingによる危険度通知
- QMP6988の気圧データの活用
- 電源断をまたいだ経過時間の永続化（Preferences/NVS）
- 日本語フォント対応
