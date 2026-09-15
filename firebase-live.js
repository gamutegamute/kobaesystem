import { initializeApp } from "https://www.gstatic.com/firebasejs/10.14.1/firebase-app.js";
import {
  getAuth,
  onAuthStateChanged,
  setPersistence,
  browserLocalPersistence,
  signInWithEmailAndPassword,
  signOut
} from "https://www.gstatic.com/firebasejs/10.14.1/firebase-auth.js";
import {
  getDatabase,
  ref,
  onValue
} from "https://www.gstatic.com/firebasejs/10.14.1/firebase-database.js";
import { firebaseConfig, defaultDeviceId } from "./firebase-config.js";

const byId = id => document.getElementById(id);
const state = {
  unsubscribe: null,
  lastValue: null,
  staleTimer: null
};

function configured() {
  return firebaseConfig.apiKey && !firebaseConfig.apiKey.startsWith("YOUR_") &&
    firebaseConfig.databaseURL && !firebaseConfig.databaseURL.includes("YOUR_");
}

function setConnection(status, label) {
  const dot = byId("liveDot");
  const text = byId("liveStatus");
  dot.dataset.status = status;
  text.textContent = label;
}

function formatUpdatedAt(value) {
  if (!Number.isFinite(value)) return "時刻不明";
  return new Intl.DateTimeFormat("ja-JP", {
    month: "numeric", day: "numeric", hour: "2-digit", minute: "2-digit", second: "2-digit"
  }).format(new Date(value));
}

function dangerLabel(level) {
  return ["SAFE", "CAUTION", "WARNING", "DANGER"][Number(level)] || "UNKNOWN";
}

function applyToSimulator(data) {
  const temp = byId("temp");
  const humidity = byId("humidity");
  const elapsed = byId("tElapsed");
  if (!temp || !humidity || !elapsed) return;

  temp.value = Math.max(Number(temp.min), Math.min(Number(temp.max), Math.round(Number(data.temperature))));
  humidity.value = Math.max(Number(humidity.min), Math.min(Number(humidity.max), Math.round(Number(data.humidity))));
  elapsed.value = Math.max(Number(elapsed.min), Math.min(Number(elapsed.max), Number(data.hoursSinceClean) || 0));
  temp.dispatchEvent(new Event("input", { bubbles: true }));
}

function renderLive(data) {
  state.lastValue = data;
  byId("liveTemp").textContent = Number.isFinite(Number(data.temperature)) ? `${Number(data.temperature).toFixed(1)} ℃` : "--.- ℃";
  byId("liveHumidity").textContent = Number.isFinite(Number(data.humidity)) ? `${Number(data.humidity).toFixed(1)} %` : "--.- %";
  byId("liveRisk").textContent = dangerLabel(data.dangerLevel);
  byId("liveElapsed").textContent = `${Number(data.hoursSinceClean || 0).toFixed(1)} 時間`;
  byId("liveUpdated").textContent = `最終更新: ${formatUpdatedAt(Number(data.updatedAt))}`;

  const age = Date.now() - Number(data.updatedAt || 0);
  setConnection(age < 120000 ? "online" : "stale", age < 120000 ? "Core2 接続中" : "更新が止まっています");

  if (byId("liveAutoApply").checked) applyToSimulator(data);
}

function watchDevice(database) {
  if (state.unsubscribe) state.unsubscribe();
  const deviceId = byId("liveDeviceId").value.trim() || defaultDeviceId;
  localStorage.setItem("kitchen-device-id", deviceId);
  setConnection("connecting", `${deviceId} を待機中`);

  state.unsubscribe = onValue(
    ref(database, `devices/${deviceId}/latest`),
    snapshot => {
      if (!snapshot.exists()) {
        setConnection("stale", "この端末のデータはまだありません");
        return;
      }
      renderLive(snapshot.val());
    },
    error => setConnection("error", `読込エラー: ${error.code}`)
  );
}

function showSignedIn(signedIn) {
  byId("liveLoginForm").hidden = signedIn;
  byId("liveLogout").hidden = !signedIn;
  byId("liveDeviceControls").hidden = !signedIn;
}

async function start() {
  byId("liveDeviceId").value = localStorage.getItem("kitchen-device-id") || defaultDeviceId;
  byId("liveAutoApply").checked = localStorage.getItem("kitchen-auto-apply") !== "false";
  byId("liveAutoApply").addEventListener("change", event => {
    localStorage.setItem("kitchen-auto-apply", String(event.target.checked));
    if (event.target.checked && state.lastValue) applyToSimulator(state.lastValue);
  });

  if ("serviceWorker" in navigator && location.protocol === "https:") {
    navigator.serviceWorker.register("/service-worker.js").catch(console.warn);
  }

  if (!configured()) {
    setConnection("setup", "Firebaseの設定が必要です");
    byId("liveSetupHint").hidden = false;
    return;
  }

  const app = initializeApp(firebaseConfig);
  const auth = getAuth(app);
  const database = getDatabase(app);
  await setPersistence(auth, browserLocalPersistence);

  byId("liveLoginForm").addEventListener("submit", async event => {
    event.preventDefault();
    const button = byId("liveLogin");
    button.disabled = true;
    setConnection("connecting", "ログイン中…");
    try {
      await signInWithEmailAndPassword(auth, byId("liveEmail").value.trim(), byId("livePassword").value);
      byId("livePassword").value = "";
    } catch (error) {
      setConnection("error", `ログイン失敗: ${error.code}`);
    } finally {
      button.disabled = false;
    }
  });

  byId("liveLogout").addEventListener("click", () => signOut(auth));
  byId("liveWatchDevice").addEventListener("click", () => watchDevice(database));

  onAuthStateChanged(auth, user => {
    showSignedIn(Boolean(user));
    if (user) watchDevice(database);
    else {
      if (state.unsubscribe) state.unsubscribe();
      state.unsubscribe = null;
      setConnection("setup", "ログインしてください");
    }
  });
}

start().catch(error => setConnection("error", `初期化エラー: ${error.message}`));

