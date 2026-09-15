import { initializeApp } from "https://www.gstatic.com/firebasejs/10.14.1/firebase-app.js";
import {
  browserLocalPersistence,
  getAuth,
  onAuthStateChanged,
  setPersistence,
  signInWithEmailAndPassword,
  signOut
} from "https://www.gstatic.com/firebasejs/10.14.1/firebase-auth.js";
import {
  getDatabase,
  onValue,
  ref,
  serverTimestamp,
  update
} from "https://www.gstatic.com/firebasejs/10.14.1/firebase-database.js";
import { defaultDeviceId, firebaseConfig } from "./firebase-config.js";

const app = initializeApp(firebaseConfig);
const auth = getAuth(app);
const database = getDatabase(app);
const deviceId = localStorage.getItem("kitchen-device-id") || defaultDeviceId;

let currentData = null;
let stopWatching = null;
let dataCallback = null;
let errorCallback = null;

function normalizeDevice(value = {}) {
  const latest = value.latest || {};
  const updatedAt = Number(latest.updatedAt) || Date.now();
  const derivedDisposedAt = updatedAt - (Number(latest.hoursSinceClean) || 0) * 3600000;
  return {
    temperature: Number(latest.temperature) || 0,
    humidity: Number(latest.humidity) || 0,
    lastDisposedAt: Number(value.lastDisposedAt) || derivedDisposedAt,
    updatedAt
  };
}

function createLoginPanel() {
  const panel = document.createElement("div");
  panel.id = "firebaseLoginPanel";
  panel.innerHTML = `
    <style>
      #firebaseLoginPanel{position:fixed;inset:0;z-index:1000;display:grid;place-items:center;padding:22px;background:rgba(36,37,31,.54);backdrop-filter:blur(8px)}
      #firebaseLoginPanel[hidden]{display:none}
      #firebaseLoginPanel form{width:min(100%,390px);padding:26px;border-radius:24px;background:#fffdf8;box-shadow:0 22px 70px rgba(0,0,0,.22);color:#24251f;font-family:-apple-system,BlinkMacSystemFont,"Hiragino Sans","Yu Gothic UI",sans-serif}
      #firebaseLoginPanel h2{margin:0 0 8px;font-size:1.35rem}
      #firebaseLoginPanel p{margin:0 0 18px;color:#6f7067;line-height:1.6}
      #firebaseLoginPanel label{display:block;margin:12px 0 5px;font-weight:700}
      #firebaseLoginPanel input{width:100%;padding:13px;border:1px solid #d8d3c8;border-radius:12px;font:inherit}
      #firebaseLoginPanel button{width:100%;margin-top:18px;padding:14px;border:0;border-radius:14px;background:#4f6a54;color:white;font:inherit;font-weight:800;cursor:pointer}
      #firebaseLoginPanel .login-error{min-height:1.4em;margin:12px 0 0;color:#b43f36;font-size:.86rem}
      #firebaseLogout{position:fixed;right:14px;bottom:calc(92px + env(safe-area-inset-bottom));z-index:20;padding:7px 10px;border:1px solid #d8d3c8;border-radius:999px;background:#fffdf8;color:#6f7067;font-size:.72rem}
    </style>
    <form id="firebaseLoginForm">
      <h2>コバエみまもりにログイン</h2>
      <p>Firebaseで作成したメールアドレスとパスワードを入力してください。</p>
      <label for="firebaseEmail">メールアドレス</label>
      <input id="firebaseEmail" type="email" autocomplete="username" required>
      <label for="firebasePassword">パスワード</label>
      <input id="firebasePassword" type="password" autocomplete="current-password" required>
      <button type="submit">ログイン</button>
      <p class="login-error" role="alert"></p>
    </form>`;
  document.body.append(panel);

  const logoutButton = document.createElement("button");
  logoutButton.id = "firebaseLogout";
  logoutButton.type = "button";
  logoutButton.textContent = "ログアウト";
  logoutButton.hidden = true;
  logoutButton.addEventListener("click", () => signOut(auth));
  document.body.append(logoutButton);

  panel.querySelector("form").addEventListener("submit", async event => {
    event.preventDefault();
    const button = panel.querySelector("button");
    const errorText = panel.querySelector(".login-error");
    button.disabled = true;
    button.textContent = "ログイン中…";
    errorText.textContent = "";
    try {
      await signInWithEmailAndPassword(
        auth,
        panel.querySelector("#firebaseEmail").value.trim(),
        panel.querySelector("#firebasePassword").value
      );
      panel.querySelector("#firebasePassword").value = "";
    } catch (error) {
      errorText.textContent = `ログインできませんでした（${error.code || "unknown"}）`;
    } finally {
      button.disabled = false;
      button.textContent = "ログイン";
    }
  });
  return { panel, logoutButton };
}

const loginUi = createLoginPanel();

function beginWatching() {
  if (!auth.currentUser || !dataCallback) return;
  if (stopWatching) stopWatching();
  stopWatching = onValue(
    ref(database, `devices/${deviceId}`),
    snapshot => {
      if (!snapshot.exists()) return;
      currentData = normalizeDevice(snapshot.val());
      dataCallback(currentData);
    },
    error => errorCallback?.(error)
  );
}

setPersistence(auth, browserLocalPersistence).catch(error => errorCallback?.(error));
onAuthStateChanged(auth, user => {
  loginUi.panel.hidden = Boolean(user);
  loginUi.logoutButton.hidden = !user;
  if (user) {
    beginWatching();
  } else if (stopWatching) {
    stopWatching();
    stopWatching = null;
  }
});

window.kobaeDataSource = {
  mode: "firebase",
  subscribe(onData, onError) {
    dataCallback = onData;
    errorCallback = onError;
    beginWatching();
    return () => {
      if (stopWatching) stopWatching();
      stopWatching = null;
      dataCallback = null;
      errorCallback = null;
    };
  },
  async markDisposed() {
    if (!auth.currentUser) throw new Error("ログインが必要です");
    const now = Date.now();
    await update(ref(database, `devices/${deviceId}`), { lastDisposedAt: serverTimestamp() });
    currentData = { ...(currentData || {}), lastDisposedAt: now, updatedAt: now };
    return currentData;
  }
};

if ("serviceWorker" in navigator && location.protocol === "https:") {
  navigator.serviceWorker.register("/service-worker.js").catch(console.warn);
}
