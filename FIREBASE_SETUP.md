# Firebase連携セットアップ

この構成では、Core2が約10秒ごとに温度・湿度・危険度をRealtime Databaseへ送り、
Firebase Hosting上のWebアプリが変更をリアルタイム表示します。Firebase HostingはHTTPSで公開されるため、外出先のスマホからも確認できます。

## 1. Firebase Console

1. Firebase Consoleでプロジェクトを作成する
2. 「プロジェクトの設定」からウェブアプリを追加する
3. Realtime Databaseを作成する
4. Authenticationの「ログイン方法」で「メール/パスワード」を有効にする
5. Authenticationの「ユーザー」で、この試作用ユーザーを1人作る

## 2. Webアプリの設定

Firebase Consoleに表示される `firebaseConfig` の値を、ルートの `firebase-config.js` に転記します。

特に次の値が必要です。

- `apiKey`
- `authDomain`
- `databaseURL`
- `projectId`
- `appId`

## 3. Core2の設定

1. `stage3_firebase_monitor/secrets.example.h` を同じフォルダへコピーする
2. コピーしたファイル名を `secrets.h` にする
3. Wi-Fi、Firebase、端末IDを記入する
4. Arduino IDEのライブラリマネージャーで `FirebaseClient` by Mobiztをインストールする
5. `stage3_firebase_monitor/stage3_firebase_monitor.ino`を開いて書き込む

`secrets.h` は `.gitignore` 対象なので、Wi-Fiパスワード等はGitHubへ入りません。

## 4. ルールとWebの公開

Firebase CLIを使用します。

```sh
npx firebase-tools login
npx firebase-tools use --add
npx firebase-tools deploy --only hosting,database
```

公開後に表示される `https://プロジェクトID.web.app` をスマホで開き、Authenticationで作ったメールアドレスとパスワードでログインします。

## データ構造

```text
devices/
  core2-01/
    latest/
      temperature
      humidity
      riskPoints
      dangerLevel
      dangerLabel
      hoursSinceClean
      cleanCount
      updatedAt
```

## セキュリティについて

付属ルールは「Firebase Authenticationでログイン済みのユーザー」だけに読み書きを許可する試作用です。同じプロジェクトへ不特定ユーザーを追加する運用には向きません。本番化する場合は、ユーザーと端末の所有関係をルールへ追加してください。

