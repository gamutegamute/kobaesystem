# 引き継ぎメモ

## 結論

発表用プロトタイプの主要機能は実装済みです。Core2単体の動作とWebアプリの公開までは確認できています。

残っている必須作業は、`stage3_firebase_monitor`をCore2へ書き込み、実機の温湿度がWeb画面へ届くことを確認する「通し試験」です。通知機能は未実装です。

## 完成・確認済み

- M5Stack Core2をPCから認識して書き込める
- ENV IIIから温度・湿度を取得できる
- Core2に危険度、キャラクター、片付けボタンを表示できる
- タッチ、音、振動、簡単なアニメーションが動く
- Firebase Realtime Database、Authentication、Hostingを作成済み
- スマホ向けWeb画面を公開済み
- Web画面にFirebaseログインとリアルタイム監視を実装済み
- Web画面の「生ごみを捨てた」をFirebaseへ記録できる

公開URL: https://kobaesystem.web.app

## 未確認・未実装

### 引き継ぎ後に最初に行うこと

1. Arduino IDEへ`FirebaseClient` by Mobiztをインストールする
2. `stage3_firebase_monitor/secrets.example.h`をコピーして`secrets.h`を作る
3. `secrets.h`へWi-Fi名、Wi-Fiパスワード、Firebaseの試作用メールアドレスとパスワードを記入する
4. `stage3_firebase_monitor/stage3_firebase_monitor.ino`をCore2へ書き込む
5. シリアルモニターでWi-Fi接続とFirebaseエラーの有無を確認する
6. スマホで公開URLを開いてログインし、実際の温度・湿度へ変わることを確認する
7. Core2とスマホの両方で「片付けた」を試す

### 未実装

- 危険度が高くなったときのスマホへのプッシュ通知
- 電源を切っても片付け時刻を保持する仕組み
- スマホで押した「片付けた」をCore2側の表示へ即時反映する双方向同期
- 複数ユーザー・複数端末を安全に管理する本番向け権限設定

## 重要なファイル

- `README.md`: 全体概要と危険度の計算方法
- `SETUP_GUIDE.md`: Arduino IDE、ドライバー、Stage 1・2の手順
- `FIREBASE_SETUP.md`: FirebaseとStage 3の設定手順
- `stage1_env_display/`: センサーだけを確認する最小コード
- `stage2_kobae_monitor/`: Core2単体で動く画面・タッチ・演出
- `stage3_firebase_monitor/`: Wi-FiとFirebase送信を加えた最終候補
- `kitchen-hazard-monitor.html`: スマホ画面
- `firebase-live.js`: ログイン、データ取得、「捨てた」記録
- `database.rules.json`: Realtime Databaseの試作用ルール

## 引き継ぐ相手へ安全に渡すもの

GitHubのリポジトリはそのまま共有して構いません。ただし、次の情報はGitHub、Slackの公開チャンネル、資料へ貼らず、信頼できる相手に個別で渡してください。

- Firebase Consoleへ入るための権限
- 試作用Firebaseユーザーのメールアドレスとパスワード
- 使用場所のWi-Fi名とパスワード

`stage3_firebase_monitor/secrets.h`はGitの対象外です。各自のPCで作成してください。

Firebase Consoleは個人のGoogleアカウントを貸すのではなく、Firebaseプロジェクトの「ユーザーと権限」から相手のGoogleアカウントを追加する方法を推奨します。

## 引き継ぎ時に伝える短い説明

> Core2とENV IIIで温湿度を測り、経過時間と合わせてコバエ・菌のリスクを4段階表示する試作です。Core2単体のセンサー、画面、タッチ演出は確認済みで、Web画面もFirebase Hostingへ公開済みです。残作業はStage 3を実機へ書き込み、Firebaseへ実データが届く通し確認です。通知と完全な双方向同期はまだ未実装です。詳しくはHANDOFF.mdとFIREBASE_SETUP.mdを見てください。

## 困ったときの切り分け順

1. Stage 1で温湿度が出るか
2. Stage 2でタッチと表示が動くか
3. Stage 3のシリアルモニターでWi-Fiへ接続できるか
4. Firebase ConsoleのRealtime Databaseに`devices/core2-01/latest`が作られるか
5. Web画面へログインできるか
6. Web画面の値が約10秒ごとに更新されるか

前の段階が動かなければ、次へ進まずその段階の配線・設定・エラーを確認してください。
