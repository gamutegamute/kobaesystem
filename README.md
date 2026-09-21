# ぬりかえす — 台所の菌・コバエリスク可視化システム

M5Stack Core2と温湿度センサーを使い、台所の生ごみや洗い物の放置リスクを直感的に伝えるプロトタイプです。

放置時間と温湿度から簡易的な危険度を計算し、時間が経つほど画面を「インク」が覆います。利用者が画面を横になぞってインクを拭き取ると、掃除・片付けをした操作として記録されます。

## 実績

- **100program ファイナル進出**
- M5Stack Core2、ENV III、Firebaseを組み合わせた実動プロトタイプを制作

## 主な体験

- ENV IIIで台所周辺の温度・湿度を測定
- 経過時間と温湿度から、菌・コバエ発生リスクを4段階で表示
- リスクの上昇に合わせてインクが画面を覆う
- 指で画面を横になぞり、一定量を拭くと「掃除完了」
- 掃除完了時に音、振動、アニメーションでフィードバック
- Firebase Realtime Databaseへセンサーデータを送信
- スマホ向けWeb画面から温湿度と状態を確認

公開Web画面: https://kobaesystem.web.app

## 使用技術

- M5Stack Core2
- ENV III Unit（SHT30温湿度センサー）
- Arduino / C++
- M5Unified / M5GFX / M5Unit-ENV
- Firebase Authentication / Realtime Database / Hosting
- HTML / CSS / JavaScript

## リポジトリ内の構成

| ファイル・フォルダ | 内容 |
| --- | --- |
| `stage3_ink_firebase_monitor_2.ino` | インク演出、ワイプ操作、温湿度、Firebase送信を統合した最終候補 |
| `nurikaesu_ink_3.ino` | Firebaseなしでインク・ワイプ体験を確認する発展版 |
| `stage3_firebase_monitor/` | インク統合前のFirebase連携版 |
| `stage2_kobae_monitor/` | キャラクター、危険度、タッチ、音、振動の実機版 |
| `stage1_env_display/` | ENV IIIの温湿度取得を確認する最小版 |
| `kitchen-hazard-monitor.html` | スマホ向けWeb画面 |
| `firebase-live.js` | Firebaseログイン、データ監視、片付け記録 |
| `HANDOFF.md` | 現在の状態、既知の制約、再開方法 |
| `SETUP_GUIDE.md` | Arduino IDEと実機の初期セットアップ |
| `FIREBASE_SETUP.md` | Firebase連携の設定方法 |
| `PROJECT_SUMMARY.md` | ポートフォリオ・実績紹介用の要約 |

## 最終候補を動かすときの注意

`stage3_ink_firebase_monitor_2.ino`は`secrets.h`を必要とします。Arduino IDEで使用するときは、同名のスケッチフォルダを作り、その中へ以下をまとめてください。

- `stage3_ink_firebase_monitor_2.ino`
- `secrets.h`（`stage3_firebase_monitor/secrets.example.h`を参考に各自で作成）

Wi-FiパスワードやFirebaseユーザーのパスワードはGitHubへコミットしないでください。

現在の最終候補はデモ用に、コード内の`MS_PER_HOUR_UNIT`が`60000.0f`、つまり「実時間1分をシステム上の1時間」として設定されています。通常速度で使う場合は`3600000.0f`へ変更してください。

## 現在の位置づけ

本リポジトリは、100programで制作した発表用プロトタイプの記録です。医療・食品衛生上の正確な判定装置ではなく、片付けを促す体験設計とIoT連携を検証するものです。

再開・引き継ぎ時は、最初に[HANDOFF.md](HANDOFF.md)を確認してください。
