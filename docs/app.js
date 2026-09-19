// ============================================================================
// app.js — ShutterLink Bench Console (Web Serial)
// ============================================================================
// Talks to the firmware's serial_config.cpp line-oriented JSON protocol over
// the same USB port used for flashing and DBG() logging (115200 baud).
// DBG() lines never start with '{', so every line read from the port is
// logged raw, and only '{'-prefixed lines are treated as protocol replies.
//
// Requires a secure context (https:// or http://localhost) — Web Serial is
// unavailable on file:// or plain http://.
// ============================================================================

const OSD_SLOT_NAMES = [
  "Off",
  "Cam status",
  "Rec time",
  "Battery",
  "Link state",
  "FC battery",
  "Arm state",
];

let port = null;
let reader = null;
let writer = null;
let readLoopPromise = null;
let connected = false;

// Serialized command queue — the protocol expects one request in flight at
// a time, so every sendCommand() call is chained onto the previous one.
let cmdChain = Promise.resolve();
let pendingResolve = null;
let pendingReject = null;
let pendingTimer = null;

let statusPollTimer = null;
let formsPopulated = false;

// ──────────────────────────────────────────────────────────────────────────
// DOM helpers
// ──────────────────────────────────────────────────────────────────────────

const $ = (id) => document.getElementById(id);

function logLine(text, cls) {
  const log = $("rawLog");
  const line = document.createElement("div");
  if (cls) line.className = cls;
  line.textContent = text;
  log.appendChild(line);
  if ($("autoScroll").checked) log.scrollTop = log.scrollHeight;
}

function setConnected(isConnected) {
  connected = isConnected;
  $("connDot").classList.toggle("connected", isConnected);
  $("connLabel").textContent = isConnected ? "Connected" : "Not connected";
  $("btnConnect").hidden = isConnected;
  $("btnDisconnect").hidden = !isConnected;
  $("app").setAttribute("aria-disabled", isConnected ? "false" : "true");
  if (!isConnected) {
    formsPopulated = false;
    if (statusPollTimer) { clearInterval(statusPollTimer); statusPollTimer = null; }
  }
}

// ──────────────────────────────────────────────────────────────────────────
// Web Serial plumbing
// ──────────────────────────────────────────────────────────────────────────

async function connect() {
  if (!("serial" in navigator)) {
    $("unsupportedNotice").hidden = false;
    return;
  }
  try {
    port = await navigator.serial.requestPort();
    await port.open({ baudRate: 115200 });
  } catch (err) {
    if (err.name !== "NotFoundError") logLine(`[console] connect failed: ${err.message}`, "err");
    return;
  }

  writer = port.writable.getWriter();
  setConnected(true);
  logLine("[console] connected");

  port.addEventListener("disconnect", handleUnexpectedDisconnect);

  readLoopPromise = readLoop().catch((err) => {
    if (connected) logLine(`[console] read error: ${err.message}`, "err");
  });

  statusPollTimer = setInterval(pollStatus, 1000);
  pollStatus();
}

async function handleUnexpectedDisconnect() {
  logLine("[console] device disconnected", "err");
  await teardown();
}

async function disconnect() {
  logLine("[console] disconnecting…");
  await teardown();
}

async function teardown() {
  setConnected(false);
  try { if (reader) { await reader.cancel(); reader.releaseLock(); } } catch (_) {}
  try { if (writer) { writer.releaseLock(); } } catch (_) {}
  try { if (port) await port.close(); } catch (_) {}
  reader = null;
  writer = null;
  port = null;
  rejectPending(new Error("disconnected"));
}

async function readLoop() {
  const decoder = new TextDecoderStream();
  const inputDone = port.readable.pipeTo(decoder.writable).catch(() => {});
  reader = decoder.readable.getReader();

  let buf = "";
  try {
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      buf += value;
      let idx;
      while ((idx = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, idx).replace(/\r$/, "");
        buf = buf.slice(idx + 1);
        if (line.length > 0) handleLine(line);
      }
    }
  } finally {
    try { reader.releaseLock(); } catch (_) {}
    await inputDone;
  }
}

function handleLine(line) {
  const isJson = line.charAt(0) === "{";
  logLine(line, isJson ? "log-json" : "log-dbg");
  if (!isJson) return;

  let obj;
  try {
    obj = JSON.parse(line);
  } catch (_) {
    return; // partial/garbled JSON — ignore, don't crash the console
  }

  if (pendingResolve) {
    clearTimeout(pendingTimer);
    const resolve = pendingResolve;
    pendingResolve = null;
    pendingReject = null;
    resolve(obj);
  }
}

function rejectPending(err) {
  if (pendingReject) {
    clearTimeout(pendingTimer);
    const reject = pendingReject;
    pendingResolve = null;
    pendingReject = null;
    reject(err);
  }
}

/// Send one JSON command and wait for the next '{'-prefixed reply line.
/// Calls are serialized — the protocol only supports one in-flight request.
function sendCommand(obj, timeoutMs = 4000) {
  const run = () => new Promise((resolve, reject) => {
    if (!connected || !writer) { reject(new Error("not connected")); return; }
    pendingResolve = resolve;
    pendingReject = reject;
    pendingTimer = setTimeout(() => {
      pendingResolve = null;
      pendingReject = null;
      reject(new Error(`timeout waiting for reply to ${obj.path}`));
    }, timeoutMs);

    const line = JSON.stringify(obj) + "\n";
    writer.write(new TextEncoder().encode(line)).catch((err) => {
      clearTimeout(pendingTimer);
      pendingResolve = null;
      pendingReject = null;
      reject(err);
    });
  });

  const result = cmdChain.then(run, run);
  // Swallow so one failed command doesn't wedge the chain for later ones.
  cmdChain = result.catch(() => {});
  return result;
}

// ──────────────────────────────────────────────────────────────────────────
// Status polling + rendering
// ──────────────────────────────────────────────────────────────────────────

function fmtSeconds(total) {
  const s = Math.max(0, total | 0);
  const m = Math.floor(s / 60);
  const r = s % 60;
  return `${m}:${String(r).padStart(2, "0")}`;
}

function fmtUptime(total) {
  const s = Math.max(0, total | 0);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  return h > 0 ? `${h}h${String(m).padStart(2, "0")}m` : `${m}m`;
}

async function pollStatus() {
  try {
    const st = await sendCommand({ path: "status" });
    renderStatus(st);
  } catch (err) {
    logLine(`[console] status poll failed: ${err.message}`, "err");
  }
}

function renderStatus(st) {
  $("stCamState").textContent = st.cam?.stateName ?? "—";
  $("stCamName").textContent = st.cam?.name || "(none)";
  $("stCamBatt").textContent = st.cam?.batt >= 0 ? `${st.cam.batt}%` : "—";
  $("stRecTime").textContent = st.cam?.valid ? fmtSeconds(st.cam.recTime) : "—";
  $("stDesired").textContent = st.rec?.desired ? "ON" : "off";
  $("stRcValue").textContent = st.rec ? `${st.rec.rcValue} us (ch ${st.rec.auxCh})` : "—";
  $("stFc").textContent = st.fc?.alive
    ? `${st.fc.armed ? "ARMED" : "disarmed"} · ${(st.fc.vbat10 / 10).toFixed(1)}V · ${st.fc.board || ""}`
    : "no link";
  $("stWifi").textContent = st.wifiOn ? `up (${st.sys?.ip || "?"})` : "off";
  $("stHeap").textContent = st.heap ? `${(st.heap / 1024).toFixed(1)} KB` : "—";
  $("stUptime").textContent = st.sys?.uptime != null ? fmtUptime(st.sys.uptime) : "—";
  $("stVersion").textContent = st.sys?.version || "—";
  $("stLastError").textContent = st.lastError || "none";

  renderCameraLists(st);

  if (!formsPopulated) {
    populateForms(st);
    formsPopulated = true;
  }
}

function renderCameraLists(st) {
  const savedEl = $("savedCamList");
  const cams = st.cams || [];
  savedEl.innerHTML = "";
  if (cams.length === 0) {
    savedEl.innerHTML = '<li class="muted">No saved cameras</li>';
  } else {
    cams.forEach((c, i) => {
      const li = document.createElement("li");
      if (c.a) li.classList.add("active");
      const typeName = c.t === 1 ? "GoPro" : "DJI";
      li.innerHTML = `
        <span>
          <span class="cam-name">${escapeHtml(c.n)}</span>
          <span class="cam-meta"> · ${typeName} · ${c.m} · ${c.on ? "online" : "offline"}${c.a ? " · active" : ""}</span>
        </span>
        <span class="cam-actions">
          <button class="btn small" data-action="select" data-idx="${i}">Select</button>
          <button class="btn small danger" data-action="remove" data-idx="${i}">Remove</button>
        </span>`;
      savedEl.appendChild(li);
    });
  }

  const pendingEl = $("pendingCamList");
  const pending = st.pending_cams || [];
  $("scanStatus").textContent = st.scanning ? "scanning…" : "";
  pendingEl.innerHTML = "";
  if (pending.length === 0) {
    pendingEl.innerHTML = `<li class="muted">${st.scanning ? "Scanning…" : "No scan results yet"}</li>`;
  } else {
    pending.forEach((p) => {
      const li = document.createElement("li");
      li.innerHTML = `
        <span>
          <span class="cam-name">${escapeHtml(p.n)}</span>
          <span class="cam-meta"> · ${p.t} · ${p.mac} · ${p.r} dBm</span>
        </span>
        <span class="cam-actions">
          <button class="btn small" data-action="pair" data-mac="${p.mac}" data-type="${p.t === "GoPro" ? 1 : 0}">Pair &amp; Save</button>
        </span>`;
      pendingEl.appendChild(li);
    });
  }
}

function escapeHtml(s) {
  return String(s ?? "").replace(/[&<>"']/g, (c) => (
    { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]
  ));
}

function populateForms(st) {
  $("camType").value = String(st.cam?.type ?? 0);

  $("auxChannel").value = st.rec?.auxCh ?? "";
  $("threshold").value = st.rec?.thr ?? "";
  $("debounce").value = st.rec?.deb ?? "";
  $("recordOnArm").checked = !!st.rec?.roa;
  $("stopOnDisarm").checked = !!st.rec?.sod;
  $("stopOnDisarmDelay").value = st.rec?.sodDelay ?? 0;

  const osdSlots = $("osdSlots");
  osdSlots.innerHTML = "";
  const slots = st.slots || [0, 0, 0, 0];
  const osdText = st.osd || ["", "", "", ""];
  slots.forEach((val, i) => {
    const wrap = document.createElement("div");
    wrap.className = "osd-slot";
    const opts = OSD_SLOT_NAMES.map((name, idx) =>
      `<option value="${idx}" ${idx === val ? "selected" : ""}>${name}</option>`
    ).join("");
    wrap.innerHTML = `
      <label>Custom Message ${i + 1}
        <select data-slot="${i}">${opts}</select>
      </label>
      <div class="preview">${escapeHtml(osdText[i] || "")}</div>`;
    osdSlots.appendChild(wrap);
  });

  $("apSsid").value = "";
  $("apPass").value = "";
  $("wifiSwitch").value = st.wifiSwitch ?? -1;
  $("scanAll").checked = !!st.scanAll;
}

// ──────────────────────────────────────────────────────────────────────────
// Actions
// ──────────────────────────────────────────────────────────────────────────

async function runAction(btn, fn) {
  const original = btn.textContent;
  btn.disabled = true;
  try {
    await fn();
  } catch (err) {
    logLine(`[console] action failed: ${err.message}`, "err");
  } finally {
    btn.disabled = false;
    btn.textContent = original;
  }
}

function wireStaticActions() {
  $("btnConnect").addEventListener("click", connect);
  $("btnDisconnect").addEventListener("click", disconnect);

  $("btnStart").addEventListener("click", (e) =>
    runAction(e.target, () => sendCommand({ path: "command", cmd: "start" })));
  $("btnStop").addEventListener("click", (e) =>
    runAction(e.target, () => sendCommand({ path: "command", cmd: "stop" })));
  $("btnReboot").addEventListener("click", (e) => {
    if (!confirm("Reboot the ShutterLink device?")) return;
    runAction(e.target, () => sendCommand({ path: "command", cmd: "reboot" }));
  });

  $("btnApplyCamType").addEventListener("click", (e) =>
    runAction(e.target, async () => {
      const r = await sendCommand({ path: "settings", camera: Number($("camType").value) });
      if (!r.ok) throw new Error(r.error || "failed");
    }));

  $("btnScan").addEventListener("click", (e) =>
    runAction(e.target, async () => {
      const r = await sendCommand({ path: "camera", scan: true });
      if (!r.ok) throw new Error(r.error || "failed");
    }));

  $("savedCamList").addEventListener("click", (e) => {
    const btn = e.target.closest("button[data-action]");
    if (!btn) return;
    const idx = Number(btn.dataset.idx);
    const action = btn.dataset.action;
    runAction(btn, async () => {
      const body = { path: "camera" };
      body[action] = idx;
      const r = await sendCommand(body);
      if (!r.ok) throw new Error(r.error || "failed");
      await pollStatus();
    });
  });

  $("pendingCamList").addEventListener("click", (e) => {
    const btn = e.target.closest("button[data-action='pair']");
    if (!btn) return;
    runAction(btn, async () => {
      const r = await sendCommand({
        path: "camera",
        pair: true,
        mac: btn.dataset.mac,
        type: Number(btn.dataset.type),
      });
      if (!r.ok) throw new Error(r.error || "failed");
      await pollStatus();
    });
  });

  $("btnSaveSwitch").addEventListener("click", (e) =>
    runAction(e.target, async () => {
      const r = await sendCommand({
        path: "settings",
        auxChannel: Number($("auxChannel").value),
        threshold: Number($("threshold").value),
        debounce: Number($("debounce").value),
        recordOnArm: $("recordOnArm").checked,
        stopOnDisarm: $("stopOnDisarm").checked,
        stopOnDisarmDelay: Number($("stopOnDisarmDelay").value),
      });
      if (!r.ok) throw new Error(r.error || "failed");
    }));

  $("btnSaveOsd").addEventListener("click", (e) =>
    runAction(e.target, async () => {
      const body = { path: "settings" };
      document.querySelectorAll("#osdSlots select[data-slot]").forEach((sel) => {
        body[`slot${sel.dataset.slot}`] = Number(sel.value);
      });
      const r = await sendCommand(body);
      if (!r.ok) throw new Error(r.error || "failed");
    }));

  $("btnSaveWifi").addEventListener("click", (e) =>
    runAction(e.target, async () => {
      const body = { path: "settings", wifiSwitch: Number($("wifiSwitch").value), scanAll: $("scanAll").checked };
      const ssid = $("apSsid").value.trim();
      const pass = $("apPass").value;
      if (ssid) body.ssid = ssid;
      if (pass || $("apPass").value === "") body.pass = pass;
      const r = await sendCommand(body);
      if (!r.ok) throw new Error(r.error || "failed");
      if (r.apRestart) logLine("[console] Wi-Fi AP restarting with new credentials");
    }));

  $("btnMsp").addEventListener("click", (e) =>
    runAction(e.target, async () => {
      const cmd = Number($("mspCmd").value);
      const r = await sendCommand({ path: "msp", cmd });
      $("mspResult").textContent = JSON.stringify(r, null, 2);
      if (!r.ok) throw new Error(r.error || "failed");
    }));

  $("btnClearLog").addEventListener("click", () => { $("rawLog").innerHTML = ""; });
}

if (!("serial" in navigator)) {
  $("unsupportedNotice").hidden = false;
}
wireStaticActions();
