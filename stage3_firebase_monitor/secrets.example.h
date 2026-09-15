#pragma once

// このファイルを secrets.h という名前で同じフォルダにコピーし、
// 自分の値へ置き換えてください。secrets.h はGitに保存されません。

#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Firebase Console > プロジェクトの設定 > 全般 > ウェブAPIキー
#define FIREBASE_API_KEY "YOUR_WEB_API_KEY"

// Firebase Authenticationで作成したメール/パスワードユーザー
#define FIREBASE_USER_EMAIL "YOUR_FIREBASE_USER_EMAIL"
#define FIREBASE_USER_PASSWORD "YOUR_FIREBASE_USER_PASSWORD"

// Realtime Databaseの「データ」画面上部に表示されるURL
#define FIREBASE_DATABASE_URL "https://YOUR_DATABASE_NAME.firebasedatabase.app"

// 複数台使う場合は core2-01、core2-02 のように変える
#define DEVICE_ID "core2-01"

