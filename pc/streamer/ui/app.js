// RinCast Streamer UI. Talks to the native app through WebView2 messaging;
// outside WebView2 (design preview) a small mock backend is used.
// Strings live in i18n.js; static HTML text carries data-i18n attributes.

import { LANGUAGES, applyI18n, detectLanguage, getLanguage, getLanguageChoice, initLanguage, locale, setLanguage, t } from "./i18n.js";

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

initLanguage();
applyI18n();

// ---------------------------------------------------------------- bridge
const native = window.chrome?.webview;
const pending = new Map();
let nextId = 1;
const listeners = [];

function send(msg) {
  if (native) native.postMessage(JSON.stringify(msg));
  else mock(msg);
}
// Resolves with the reply, or with { ok: false } if the app doesn't answer in time.
function request(msg, timeoutMs = 30000, onTimeout) {
  const requestId = String(nextId++);
  return new Promise((resolve) => {
    const timer = setTimeout(() => {
      // A timeout does not cancel native work: keep a settings key in flight until its reply.
      if (onTimeout) { onTimeout(); return; }
      pending.delete(requestId);
      resolve({ ok: false, error: t("err.noResponse") });
    }, timeoutMs);
    pending.set(requestId, (r) => { clearTimeout(timer); resolve(r); });
    send({ ...msg, requestId });
  });
}
function receive(msg) {
  if (msg.requestId && pending.has(msg.requestId)) {
    pending.get(msg.requestId)(msg);
    pending.delete(msg.requestId);
    if (msg.type === "reply") return;
  }
  listeners.forEach((fn) => fn(msg));
}
if (native) native.addEventListener("message", (e) => receive(typeof e.data === "string" ? JSON.parse(e.data) : e.data));

// ---------------------------------------------------------------- state and notices
let state = null;
let confirmedSettings = null;
let settingsRevision = -1;
const settingIntents = new Map();
const history = [];

function acceptSettings(settings) {
  if (settings._revision < settingsRevision) return false;
  settingsRevision = settings._revision;
  confirmedSettings = settings;
  return true;
}
function overlaySettings() {
  if (!state) return;
  state.settings = { ...confirmedSettings };
  for (const [key, intent] of settingIntents) state.settings[key] = intent.value;
}

function toast(text) {
  const el = $("#toast");
  el.textContent = text;
  el.classList.add("show");
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => el.classList.remove("show"), 2600);
}
// Failures stay until dismissed: nobody should have to catch them in a toast.
function showError(text) {
  $("#bannerText").textContent = text;
  $("#banner").hidden = false;
  $("#main").scrollTop = 0;
}
$("#bannerClose").onclick = () => { $("#banner").hidden = true; };

const confirmDialog = $("#confirm");
function confirmAction(title, text, action, danger = true) {
  $("#confirmTitle").textContent = title;
  $("#confirmText").textContent = text;
  $("#confirmYes").textContent = action;
  $("#confirmYes").className = danger ? "btn danger" : "btn primary";
  confirmDialog.returnValue = "";
  confirmDialog.showModal();
  return new Promise((resolve) => {
    $("#confirmYes").onclick = () => confirmDialog.close("yes");
    $("#confirmNo").onclick = () => confirmDialog.close("no");
    confirmDialog.addEventListener("close", () => resolve(confirmDialog.returnValue === "yes"), { once: true });
  });
}

listeners.push((msg) => {
  switch (msg.type) {
    case "state":
      if (!acceptSettings(msg.settings)) break;
      state = msg;
      overlaySettings();
      renderAll();
      break;
    case "stats":
      if (state) state.session = msg.session;
      if (msg.session) countPoor(msg.session);
      if (state) renderDependents();
      renderHome();
      renderStatus();
      renderAutoscale();
      break;
    case "session":
      request({ type: "getState" });
      if (msg.connected) toast(t("toast.connected", { name: msg.deviceName }));
      else if (msg.reason !== "replaced") toast(t("toast.disconnected", { name: msg.deviceName }));
      break;
    case "approval":
      if (state) state.pendingApproval = msg;
      showApproval(msg);
      renderHome();
      break;
    case "inputState":
      if (state) state.system.inputBlocked = msg.blocked;
      renderElevate();
      break;
    case "firewall":
      if (state) state.system.firewall = msg.firewall;
      renderHome();
      renderFirewall();
      break;
    case "approvalEnded":
      // Not an answer: the headset stopped asking.
      if (state) state.pendingApproval = null;
      approvalFor = null;
      if ($("#approval").open) $("#approval").close();
      renderHome();
      renderStatus();
      break;
    case "vrState":
      if (state?.session) Object.assign(state.session, { vrState: msg.state, vrProducer: msg.producer, vrApp: msg.app });
      renderHome();
      renderStatus();
      break;
    case "oscEye":
      if (state) {
        state.oscEye = msg.oscEye;
        renderOscEyeStatus();
      }
      break;
  }
});

// ---------------------------------------------------------------- navigation
function documentTitle(page, heading) {
  return page === "home" ? t("app.title") : `${heading} - ${t("app.title")}`;
}
function goTo(page, moveFocus) {
  // A53: Account lives in the rail footer rather than the main list, so this looks at every
  // rail entry instead of only the ones inside <nav>.
  $$(".rail [data-page]").forEach((x) => {
    if (x.dataset.page === page) x.setAttribute("aria-current", "page");
    else x.removeAttribute("aria-current");
  });
  $$(".page").forEach((p) => p.classList.toggle("active", p.dataset.page === page));
  const active = $(".page.active");
  const title = $("h1", active);
  document.title = documentTitle(page, title.textContent);
  $("#main").scrollTop = 0;
  if (moveFocus) title.focus();
  if (page !== "home") request({ type: "getState" });
  if (state) renderStatus();
}
$$(".rail [data-page]").forEach((b) =>
  // detail is 0 for keyboard activation: then focus follows to the new page.
  b.addEventListener("click", (e) => goTo(b.dataset.page, e.detail === 0)),
);

// ---------------------------------------------------------------- settings binding
function bindSettings() {
  $$("[data-setting]").forEach((el) => {
    const v = state.settings[el.dataset.setting];
    if (el.type === "checkbox") el.checked = !!v;
    else if (el.type === "radio") el.checked = String(v) === el.value;
    else if (document.activeElement !== el) el.value = v ?? "";
    if (el.type === "range") updateRangeLabel(el);
  });
}
// A slider says what its number means. Bitrate ones show Mbps; the picture-priority
// slider has no unit at all - it names the stop you are on ("Balanced"), because "1 Mbps"
// was never what the three-tier choice was about. Dispatch by data-unit, so a control
// never has to know its own setting key.
// A86: the picture-priority slider (data-unit="profile") is gone, so no control on the
// page names a tier any more and this is the bitrate formatter only.
function rangeText(el) {
  return `${el.value} Mbps`;
}
function updateRangeLabel(el) {
  const text = rangeText(el);
  el.parentElement.querySelector("output").textContent = text;
  el.setAttribute("aria-valuetext", text);
  el.style.setProperty("--fill", `${((el.value - el.min) / (el.max - el.min)) * 100}%`);
}
// A setting that failed says so next to itself. One that worked just stays as you left it.
function settingError(el, text) {
  const host = el.closest(".check, .bar, .options") || el;
  let note = host.nextElementSibling;
  if (!note?.classList.contains("failed")) {
    if (!text) return;
    note = document.createElement("p");
    note.className = "failed";
    note.setAttribute("role", "alert");
    host.after(note);
  }
  note.textContent = text;
}
function commitSetting(el) {
  const key = el.dataset.setting;
  let value = el.type === "checkbox" ? el.checked : el.value;
  if (el.type === "range" || el.type === "number" || (el.type === "radio" && /^\d+$/.test(value))) value = Number(value);
  if (key === "externalEyeOscPort") {
    if (!Number.isFinite(value) || value < 1024 || value > 65535) value = 9945;
    value = Math.round(value);
    el.value = value;
  }
  if (key === "externalEyeOffsetX" || key === "externalEyeOffsetY") {
    if (!Number.isFinite(value)) value = 0;
    value = Math.max(-20, Math.min(20, value));
    el.value = value;
  }
  if (key === "externalEyeGainX" || key === "externalEyeGainY") {
    if (!Number.isFinite(value)) value = 1;
    value = Math.max(0.2, Math.min(3, value));
    el.value = value;
  }
  if (key === "pcName" && !String(value).trim()) {
    settingError(el, t("err.pcName"));
    el.value = state.settings.pcName;
    return;
  }
  if (key === "externalEyeTracking" && value && confirmedSettings) {
    confirmedSettings.foveatedEncoding = false;
    settingIntents.delete("foveatedEncoding");
  } else if (key === "foveatedEncoding" && value && confirmedSettings) {
    confirmedSettings.externalEyeTracking = false;
    settingIntents.delete("externalEyeTracking");
  }
  settingError(el, "");
  const intent = settingIntents.get(key);
  if (intent) {
    intent.value = value;
    intent.el = el;
    intent.generation++;
  } else {
    settingIntents.set(key, { value, el, generation: 0 });
    submitSetting(key);
  }
  overlaySettings();
  if (state) {
    if (key === "externalEyeTracking" || key === "foveatedEncoding") bindSettings();
    renderDependents();
  }
}
function submitSetting(key) {
  const intent = settingIntents.get(key);
  const sent = intent.value;
  const generation = intent.generation;
  request({ type: "setSettings", settings: { [key]: sent } }, 30000,
    () => settingError(intent.el, t("err.noResponse"))).then((r) => {
    if (r.settings) acceptSettings(r.settings);
    const failed = r.ok === false || !r.settings;
    settingError(intent.el, failed ? r.error || t("err.save") : "");
    if (intent.value !== sent || (intent.generation !== generation && confirmedSettings[key] !== intent.value)) submitSetting(key);
    else settingIntents.delete(key);
    overlaySettings();
    if (!state) return;
    bindSettings();
    renderDependents();
    renderAudio();
    drawSpark();
  });
}
function wireSetting(el) {
  if (el.type === "range") el.addEventListener("input", () => updateRangeLabel(el));
  el.addEventListener("change", async () => {
    if (el.dataset.setting === "requireApproval" && !el.checked &&
        !(await confirmAction(t("confirm.stopAsking.title"), t("confirm.stopAsking.text"), t("confirm.stopAsking.action")))) {
      el.checked = true;
      return;
    }
    commitSetting(el);
  });
}
$$("[data-setting]").forEach(wireSetting);

// ROUND19 T3: the autoscale ladder's read-only level line, hidden while the switch
// is off, and the manual reset - the only way the ladder climbs back to 100 %.
function renderAutoscale() {
  const line = $("#l7LevelLine");
  if (!line) return;
  const on = document.querySelector('[data-setting="l7Autoscale"]')?.checked;
  // 等级行跟随开关；底层项整块收在页面最末的「高级」折叠区，不再用行级 hidden 掏空标题。
  line.hidden = !on;
  const lvl = state?.session?.vr?.autoscaleLevel;
  $("#l7Level").textContent = lvl != null ? lvl + "%" : "—";
}
document.querySelector('[data-setting="l7Autoscale"]')?.addEventListener("change", renderAutoscale);

$("#l7Reset").addEventListener("click", async () => {
  const r = await request({ type: "autoscaleReset" }, 10000);
  if (r && !r.ok) toast(r.error || t("err.noResponse"));
  renderAutoscale();
});

// ---------------------------------------------------------------- language
const languageSelect = $("#language");
function fillLanguageOptions() {
  // Option names stay in their own language: find yours even in a UI you can't read.
  const detected = LANGUAGES.find((l) => l.code === detectLanguage())?.name || detectLanguage();
  languageSelect.replaceChildren(
    new Option(t("general.languageAuto", { language: detected }), "auto"),
    ...LANGUAGES.map((l) => new Option(l.name, l.code)),
  );
  languageSelect.value = getLanguageChoice();
}
// Clearing the render caches makes every re-render pick up the new strings.
function refreshForLanguage() {
  $("#homeActions").dataset.key = "";
  $("#devices").dataset.key = "";
  $("#issues").dataset.key = "";
  accountKey = "";
  renderHome.announced = "";
  fillLanguageOptions();
  if (state) renderAll();
  const active = $(".page.active");
  document.title = documentTitle(active.dataset.page, $("h1", active).textContent);
}
languageSelect.addEventListener("change", () => {
  setLanguage(languageSelect.value);
  applyI18n();
  refreshForLanguage();
});

// ---------------------------------------------------------------- health: one truth for the home screen
const ICONS = {
  warn: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 4l9 16H3zM12 10v4M12 17v.5"/></svg>',
  info: '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8v.5"/></svg>',
};

// A number only counts as poor after a few seconds in a row, not on one bad sample, so nothing flickers.
const poorRuns = {};
function sustained(name, poor) {
  poorRuns[name] = poor ? (poorRuns[name] || 0) + 1 : 0;
  return poorRuns[name] >= 5;
}
function streamTrouble(s) {
  // A19 audit #2: a paused stream (headset on the desktop / in a menu) keeps the
  // last numbers on the wire - frozen values must not blame the PC or the network.
  // No trouble card while paused; back to normal on resume.
  if (s.vr?.paused) return null;
  if (!numberCells(s).some((n) => n[3])) return null;
  // A slow picture with a busy encoder is the PC's doing; the rest is the network's.
  const { encodeMs } = liveNumbers(s);
  if (poorRuns.fps >= 5 && encodeMs >= 14) return { level: "warn", title: t("trouble.pc.title"), text: t("trouble.pc.text") };
  // A96: "the rest is the network's" was a guess, and on the machine we measured it was wrong:
  // 34 fps of a requested 72 with loss 0 %, no drops and a 3 ms round trip. The headset times
  // Game and SteamVR separately from the wire, so a game that needs clearly more than a whole
  // frame period is a resolution problem on this PC, and a cable tip sends the user to the
  // wrong end of the room. The margin is for the healthy case, where the game stage
  // equals the frame period by definition.
  const stages = s.client?.latencyStages || {};
  const period = s.vr?.refreshRate ? 1000 / s.vr.refreshRate : 0;
  if (poorRuns.fps >= 5 && stages.game > period * 1.1 && !(s.vr.droppedFps > 1)) {
    return {
      level: "warn",
      title: t("trouble.game.title"),
      text: t("trouble.game.text", { ms: stages.game.toFixed(0), period: period.toFixed(0), w: s.vr.eyeWidth, h: s.vr.eyeHeight }),
    };
  }
  if (s.vpn) return { level: "warn", title: t("trouble.vpn.title"), text: t("trouble.vpn.text") };
  if (s.remote) return { level: "warn", title: t("trouble.remote.title"), text: t("trouble.remote.text") };
  return { level: "warn", title: t("trouble.remote.title"), text: t("trouble.network.text") };
}

function health() {
  const sys = state.system;
  const issues = [];
  if (!sys.encoder.backend || !(sys.encoder.hevc || sys.encoder.av1 || sys.encoder.h264)) {
    issues.push({ level: "blocked", title: t("issue.encoder.title"), text: t("issue.encoder.text") });
  } else if (sys.encoder.hwFallback) {
    // A52: still streams, but on the software encoder - the usual reason "it got slower".
    issues.push({ level: "warn", title: t("issue.hwFallback.title"), text: t("issue.hwFallback.text") });
  }
  if (!sys.steamvr.installed) {
    issues.push({ level: "info", title: t("issue.steamvrMissing.title"), text: t("issue.steamvrMissing.text") });
  } else if (!sys.steamvr.driverRegistered) {
    issues.push({ level: "warn", title: t("issue.driver.title"), text: t("issue.driver.text"), action: t("vr.register"), run: registerDriver });
  }
  const a = sys.audioDriver;
  if (state.settings.streamAudio && state.settings.micPassthrough && !a.active?.speaker && !a.active?.microphone && state.settings.audioDevices !== "system") {
    issues.push({ level: "warn", title: t("issue.audio.title"), text: t("issue.audio.text") });
  }
  const c = state.cloud;
  if (c.signedIn && state.settings.allowRemote && c.status === "error") {
    issues.push({ level: "warn", title: t("issue.cloud.title"), text: t("issue.cloud.text", { error: c.error || t("err.cloudFailed") }) });
  }
  const fw = sys.firewall || {};
  if (fw.otherNetworksOpen === false) {
    issues.push({ level: "warn", title: t("issue.firewall.title"), text: t("issue.firewall.text"), action: t("issue.firewall.action"), run: () => setFirewall(!!fw.publicAllowed) });
  }
  // Not a fault: Public stays closed until someone chooses otherwise.
  if (fw.usbAdapter && fw.usbOpen === false) {
    issues.push({ level: "warn", title: t("issue.usb.title"), text: t("issue.usb.text", { adapter: fw.usbAdapter }), action: t("issue.usb.action"), run: allowUsbCable });
  }
  if (fw.publicInUse && !fw.publicAllowed) {
    issues.push({
      level: "info",
      title: t("issue.public.title"),
      text: t("issue.public.text", { vpn: (sys.vpnAddresses || []).length ? t("issue.public.vpn") : "" }),
      action: t("misc.allowOnPublic"),
      run: () => choosePublicNetworks(true),
    });
  }
  const trouble = state.session && streamTrouble(state.session);
  if (trouble) issues.push(trouble);
  const rank = { blocked: 0, warn: 1, info: 2 };
  issues.sort((a, b) => rank[a.level] - rank[b.level]);
  return { issues, blocked: issues.some((i) => i.level === "blocked") };
}

// ---------------------------------------------------------------- home
// Remembered here: "Allow once" never adds a headset to the settings.
function connectedBefore() {
  if (state.session || (state.settings.trustedDevices || []).length) {
    try { localStorage.setItem("connectedBefore", "1"); } catch {}
    return true;
  }
  try { return localStorage.getItem("connectedBefore") === "1"; } catch { return false; }
}

function setFacts(list, rows) {
  // Rewritten only when something changed, so text stays selectable while stats tick.
  const html = rows.filter((r) => r && r[1]).map(([k, v, cls]) => `<dt>${esc(k)}</dt><dd${cls ? ` class="${cls}"` : ""}>${esc(v)}</dd>`).join("");
  if (list.dataset.html !== html) {
    list.dataset.html = html;
    list.innerHTML = html;
  }
}

function encoderText() {
  const enc = state.system.encoder;
  if (!enc.backend) return t("encoder.none");
  const backend = { nvenc: "NVIDIA", amf: "AMD", mf: t("encoder.mf") }[enc.backend] || enc.backend;
  return `${backend}: ${[enc.h264 && "H.264", enc.hevc && "HEVC", enc.av1 && "AV1"].filter(Boolean).join(t("misc.listSep"))}`;
}

function renderHome() {
  if (!state) return;
  const s = state.session;
  const approval = state.pendingApproval;
  const { issues, blocked } = health();
  $("#live").hidden = true;
  $("#numbers").hidden = true;
  $("#steps").hidden = true;
  if (approval) {
    $("#headline").textContent = t("home.approvalHeadline", { name: approval.deviceName });
    $("#subline").dataset.state = "wait";
    $("#subline").textContent = t("home.approvalSub", { address: approval.address });
    setActions("", () => []);
    setFacts($("#facts"), []);
  } else if (s) {
    const inVr = s.vrState === "running";
    const app = s.vrApp || (s.vrProducer === "steamvr" ? "SteamVR" : t("home.aVrGame"));
    const streaming = inVr || (s.desktop || []).length > 0;
    $("#headline").textContent = inVr ? t("home.playing", { app }) : streaming ? t("home.streamingDesktop") : t("home.connectedTo", { name: s.deviceName });
    $("#subline").dataset.state = s.vrState === "launching" ? "wait" : "plain";
    $("#subline").textContent = s.vrState === "launching" ? t("home.startingSteamvr") : `${s.deviceName}, ${duration(s.connectedSeconds)}`;
    setActions(`session:${getLanguage()}:${s.deviceName}`, () => [button(t("home.disconnect"), "", async () => {
      const running = state.session?.vrState === "running";
      if (await confirmAction(
        t("confirm.disconnect.title", { name: s.deviceName }),
        running ? t("confirm.disconnect.vrText") : t("confirm.disconnect.plainText"),
        t("confirm.disconnect.action"),
        false,
      )) send({ type: "disconnect" });
    })]);
    $("#live").hidden = false;
    renderLive(s);
  } else {
    $("#headline").textContent = blocked ? t("home.blocked") : t("home.choosePc", { name: state.settings.pcName });
    const firstRun = !blocked && !connectedBefore();
    // The steps below say it all the first time; after that, the next step is the headline.
    if (firstRun) $("#headline").textContent = t("home.setupQuest");
    $("#subline").textContent = blocked ? t("home.blockedSub") : "";
    $("#stepPcName").textContent = state.settings.pcName;
    $("#subline").dataset.state = blocked ? "bad" : "";
    $("#steps").hidden = !firstRun;
    setActions("", () => []);
    history.length = 0;
    const devices = (state.settings.trustedDevices || []).slice().sort((a, b) => (b.lastSeen || 0) - (a.lastSeen || 0));
    const last = devices[0];
    setFacts($("#facts"), [
      [(state.system.addresses || []).length > 1 ? t("home.addresses") : t("home.address"), (state.system.addresses || []).join(", ") || t("home.noNetwork")],
      ...(state.system.vpnAddresses || []).map((v) => [v.name, t("home.vpnAddress", { address: v.address })]),
      state.cloud.signedIn && state.settings.allowRemote && state.cloud.status === "online" && [t("home.otherNetworks"), t("home.otherNetworksHint")],
      last && [t("home.lastHeadset"), `${last.name}${last.lastSeen ? `, ${new Date(last.lastSeen * 1000).toLocaleDateString(locale())}` : ""}`],
    ]);
  }
  renderIssues(issues);
  const headline = $("#headline").textContent;
  if (renderHome.announced !== headline) {
    renderHome.announced = headline;
    $("#liveStatus").textContent = headline;
  }
}

// Rebuilt only when the buttons change: stats arrive every second, and rebuilding would
// lose keyboard focus and clicks in progress.
function setActions(key, make) {
  const actions = $("#homeActions");
  if (actions.dataset.key === key) return;
  actions.dataset.key = key;
  actions.replaceChildren(...make());
}

function renderIssues(issues) {
  const list = $("#issues");
  // Rebuilt only when something changed: stats arrive every second, and the buttons must keep focus.
  const key = JSON.stringify(issues.map((i) => [i.level, i.title, i.text]));
  if (list.dataset.key === key) return;
  list.dataset.key = key;
  list.replaceChildren(...issues.map((i) => {
    const li = document.createElement("li");
    li.className = i.level;
    li.innerHTML = `<div>${i.level === "info" ? ICONS.info : ICONS.warn}<b>${esc(i.title)}</b><p>${esc(i.text)}</p></div>`;
    if (i.action) li.append(button(i.action, "", i.run));
    return li;
  }));
}

// What's on screen in the status bar and on the home page, from one place.
function liveNumbers(s) {
  const vr = s.vr && s.vrState === "running" ? s.vr : null;
  const desk = (s.desktop || []).reduce((a, d) => ({ fps: Math.max(a.fps, d.fps), mbps: a.mbps + d.mbps, encodeMs: Math.max(a.encodeMs, d.encodeMs || 0) }), { fps: 0, mbps: 0, encodeMs: 0 });
  return { vr, fps: vr ? vr.fps : desk.fps, mbps: vr ? vr.mbps : desk.mbps, encodeMs: vr ? vr.encodeMs : desk.encodeMs, latency: vr?.latencyMs || s.client?.latencyMs || s.rttMs };
}

function countPoor(s) {
  const { vr, fps, mbps, latency } = liveNumbers(s);
  // A19 audit #2: while the stream is paused its numbers freeze, so they must not
  // feed the "poor" runs. The runs reset (nothing stays red), and counting starts
  // fresh on resume.
  if (s.vr?.paused) {
    poorRuns.latency = poorRuns.fps = poorRuns.mbps = 0;
    return;
  }
  const wanted = vr?.refreshRate;
  sustained("latency", latency >= 80);
  sustained("fps", !!(wanted && fps && fps < wanted * 0.95));
  sustained("mbps", !!(vr && mbps && mbps < 10));
}

function numberCells(s) {
  const { vr, fps, mbps, latency } = liveNumbers(s);
  const wanted = vr?.refreshRate;
  const halfRate = wanted && fps >= wanted * 0.45 && fps <= wanted * 0.55;
  return [
    [t("stat.latency"), latency ? Math.round(latency) : "–", latency ? (latency >= 130 ? t("unit.msHigh") : latency >= 80 ? t("unit.msNoticeable") : t("unit.ms")) : "", poorRuns.latency >= 5],
    [t("stat.fps"), fps ? Math.round(fps) : "–", fps ? (wanted && fps < wanted * 0.95 ? (halfRate ? t("unit.fpsHalf") : t("unit.fpsBelow", { rate: Math.round(wanted) })) : t("unit.fps")) : "", poorRuns.fps >= 5],
    [t("stat.bitrate"), mbps ? mbps.toFixed(mbps < 10 ? 1 : 0) : "–", mbps ? (mbps < 10 ? t("unit.mbpsLow") : t("unit.mbps")) : "", poorRuns.mbps >= 5],
  ];
}

function renderLive(s) {
  const { vr, mbps, encodeMs } = liveNumbers(s);
  const paused = !!s.vr?.paused;  // A19 audit #2: the headset is on the desktop / in a menu
  const numbers = $("#numbers");
  numbers.hidden = false;
  numbers.innerHTML = numberCells(s).map(([label, value, unit, poor]) => `<div><span>${esc(label)}</span><b class="${poor ? "poor" : ""}">${esc(value)}<small>${esc(unit)}</small></b></div>`).join("");
  setFacts($("#facts"), [
    paused && [t("live.status"), t("live.paused")],
    vr && [t("home.video"), t("home.videoVr", { codec: vr.codec, w: vr.eyeWidth, h: vr.eyeHeight, hz: Math.round(vr.refreshRate) })],
    !vr && (s.desktop || []).length && [t("home.video"), s.desktop.map((d) => d.name).join(", ")],
    !vr && !(s.desktop || []).length && [t("home.video"), t("home.videoNothing")],
    encodeMs && [t("home.encoding"), encodeMs < 14 ? `${encodeMs.toFixed(1)} ms` : t("home.gpuBusy", { ms: encodeMs.toFixed(1) }), encodeMs < 14 ? "" : "no"],
    [t("home.network"), s.usb ? t("home.netUsb", { rtt: s.rttMs.toFixed(1) }) : s.vpn ? t("home.netVpn", { rtt: s.rttMs.toFixed(1) }) : s.remote ? t("home.netRemote", { rtt: s.rttMs.toFixed(1) }) : t("home.netLocal", { rtt: s.rttMs.toFixed(1) })],
  ].filter(Boolean));
  renderStages(s);
  history.push(mbps || 0);
  if (history.length > 60) history.shift();
  drawSpark();
}

// The headset times each stage a frame goes through; together they make up the latency.
const STAGES = [
  ["uplink", "stage.uplink"],
  ["game", "stage.game"],
  ["encoderWait", "stage.encoderWait"],
  ["encode", "stage.encode"],
  ["capture", "stage.capture"],
  ["downlink", "stage.downlink"],
  ["decode", "stage.decode"],
  ["nextFrame", "stage.nextFrame"],
  ["compositor", "stage.compositor"],
];

function renderStages(s) {
  const got = s.client?.latencyStages || {};
  const rows = STAGES.filter(([key]) => typeof got[key] === "number");
  $("#stagesBox").hidden = !rows.length;
  if (!rows.length) return;
  const total = rows.reduce((sum, [key]) => sum + Math.max(got[key], 0), 0) || 1;
  const biggest = Math.max(...rows.map(([key]) => got[key]));
  $("#stages").innerHTML = rows.map(([key, label]) => {
    const ms = Math.max(got[key], 0);
    return `<div class="stage${got[key] === biggest ? " big" : ""}"><span>${esc(t(label))}</span><i style="width:${((ms / total) * 100).toFixed(1)}%"></i><b>${ms < 10 ? ms.toFixed(1) : Math.round(ms)} ms</b></div>`;
  }).join("");
}

// Live numbers while connected, for the pages that don't have them already.
function renderStatus() {
  if (!state) return;  // push can arrive before the first getState reply
  const el = $("#status");
  const s = state.session;
  const onHome = $(".rail [data-page][aria-current]")?.dataset.page === "home";
  let html = "";
  if (s && !onHome) {
    const cell = (label, value, poor) => `<div><span>${esc(label)}</span><b class="${poor ? "poor" : ""}">${esc(value)}</b></div>`;
    html = cell(t("stat.connectedTo"), s.deviceName) + '<i class="gap"></i>' +
      numberCells(s).map(([label, value, unit, poor]) => cell(label, `${value} ${unit}`.trim(), poor)).join("");
  }
  if (el.dataset.html !== html) {
    el.dataset.html = html;
    el.innerHTML = html;
  }
}

function drawSpark() {
  const c = $("#spark");
  if (!c.clientWidth || !state) return;
  const dpr = devicePixelRatio || 1;
  const h = c.clientHeight;
  c.width = c.clientWidth * dpr;
  c.height = h * dpr;
  const g = c.getContext("2d");
  g.scale(dpr, dpr);
  const s = state.session;
  const limit = s?.vr ? s.headsetBitrateMbps || state.settings.vrBitrateMbps : state.settings.desktopBitrateMbps;
  const peak = Math.max(...history, 0);
  const w = c.clientWidth, max = Math.max(limit, peak);
  $("#sparkRange").textContent = t("home.sparkLimit", { mbps: limit });
  c.setAttribute("aria-label", t("home.sparkAria", { peak: Math.round(peak), limit }));
  const forced = matchMedia("(forced-colors: active)").matches;
  // The limit, as the top of the chart.
  g.fillStyle = forced ? "CanvasText" : "#555";
  g.fillRect(0, 0, w, 1);
  if (history.length < 2) return;
  const step = w / 59;
  const pts = history.map((v, i) => [w - (history.length - 1 - i) * step, h - (v / max) * (h - 2)]);
  g.beginPath();
  pts.forEach(([x, y], i) => (i ? g.lineTo(x, y) : g.moveTo(x, y)));
  g.strokeStyle = forced ? "CanvasText" : "#2ec4ff";
  g.lineWidth = 2;
  g.lineJoin = "round";
  g.stroke();
}

function duration(sec) {
  if (sec < 60) return t("duration.now");
  if (sec < 3600) return t("duration.min", { min: Math.floor(sec / 60) });
  return t("duration.hm", { h: Math.floor(sec / 3600), m: Math.floor((sec % 3600) / 60) });
}

function button(label, kind, onClick) {
  const b = document.createElement("button");
  b.className = `btn ${kind}`;
  b.textContent = label;
  b.onclick = onClick;
  return b;
}

// ---------------------------------------------------------------- approval
// Three explicit answers. Anything else (Esc, closing) means no.
const dialog = $("#approval");
let approvalFor = null;
function showApproval(a) {
  approvalFor = a;
  $("#approvalTitle").textContent = t("home.allowDevice", { name: a.deviceName });
  $("#approvalText").textContent = t("home.approvalText", { model: a.model || t("home.aHeadset"), address: a.address });
  if (dialog.open) return;  // state updates must not move focus
  dialog.returnValue = "";
  dialog.showModal();
  $("#denyBtn").focus();
}
function answerApproval(answer) { dialog.close(answer); }
$("#denyBtn").onclick = () => answerApproval("deny");
$("#allowOnceBtn").onclick = () => answerApproval("once");
$("#allowBtn").onclick = () => answerApproval("always");
dialog.addEventListener("close", () => {
  if (!approvalFor) return;
  const answer = dialog.returnValue;
  const allow = answer === "once" || answer === "always";
  send({ type: allow ? "approve" : "deny", deviceId: approvalFor.deviceId, remember: answer === "always" });
  if (state) state.pendingApproval = null;
  approvalFor = null;
  renderHome();
});

// ---------------------------------------------------------------- other pages
const AUDIO_SETS = {
  rincast: { speaker: "RinCast Speaker", mic: "RinCast Microphone" },
  steam: { speaker: "Steam Streaming Speakers", mic: "Steam Streaming Microphone" },
};

function setOff(row, off) {
  row.classList.toggle("off", off);
  row.querySelector("input").disabled = off;
}

// ---------------------------------------------------------------- streaming: advanced fold
// 「高级」 is the bottom-most block of the page and starts shut. Shut is the `hidden`
// attribute on its body, i.e. display:none - visibility or height:0 tricks would leave the
// tuning controls in the tab order inside a zero-height box. The title is a plain button, so
// Tab reaches it and Enter/Space toggle it with no key handling here; aria-expanded is the
// state AT announces, which is why it is written from the one place that changes `hidden`.
// Nothing else touches it: the fold is the user's choice, and the per-second state refresh
// (renderAll -> bindSettings) writes values, never disclosure state.
const advancedHead = $("#advancedHead");
const advancedBody = $("#advancedBody");
if (advancedHead && advancedBody) {
  advancedHead.addEventListener("click", () => {
    advancedBody.hidden = !advancedBody.hidden;
    advancedHead.setAttribute("aria-expanded", String(!advancedBody.hidden));
  });
  advancedHead.setAttribute("aria-expanded", String(!advancedBody.hidden));
}

function renderDependents() {
  const enc = state.system.encoder;
  const offers = { auto: true, hevc: enc.hevc, av1: enc.av1, h264: enc.h264 };
  const unsupported = [];
  $$("#codecOptions label").forEach((l) => {
    const off = !offers[l.querySelector("input").value] && !!enc.backend;
    if (off) unsupported.push(l.textContent.trim());
    setOff(l, off);  // 能力探测置灰而不是藏行：标准卡片的编码标题下永远有可见控件
  });
  const a = state.system.audioDriver;
  $("#audioRincast").hidden = !(a.speaker || a.microphone);
  setOff($("#audioSteam"), !(a.steam?.speaker || a.steam?.microphone));
  // What the connected headset chose goes first, so here it's only for show.
  const session = state.session;
  // "Use this PC's limit" wins: the headset's number only takes over when that
  // switch is off, matching EffectiveVrBitrate() on the server.
  const fromHeadset = !state.settings.pcBitrateWins && session?.headsetBitrateMbps > 0;
  setOff($("#vrBitrateBar"), !!fromHeadset);
  const vrBadge = $("#vrBitrateHeadsetControl");
  if (vrBadge) vrBadge.hidden = !fromHeadset;
  // "actual / ceiling": the throughput the encoder really managed, next to the cap,
  // so "set 140, got 90" is visible without opening the logs. Only while a stream
  // reports it; the plain cap shows before that.
  const ceiling = fromHeadset ? session.headsetBitrateMbps : state.settings.vrBitrateMbps;
  const actual = session?.vr?.mbps;
  // What the encoder really ran: "96 / 100 Mbps" says spent vs cap. The extra
  // details (encoded size, QP, fps) go to their own line so the slider never
  // overflows when a profile switch changes the resolution.
  const encW = session?.vr?.encWidth || 0;
  const qp = session?.vr?.qp || -1;
  const gameFps = Math.round(session?.vr?.gameFps || 0);
  const encFps = Math.round(session?.vr?.encFps || 0);
  $("#vrBitrateBar output").textContent = actual ? `${Math.round(actual)} / ${ceiling} Mbps` : `${ceiling} Mbps`;
  const liveBits = [];
  if (actual && encW > 0) liveBits.push(`${Math.round(encW / 2)}²`);
  if (actual && qp > 0) liveBits.push(`QP ${qp.toFixed(1)}`);
  if (actual && encFps > 0) liveBits.push(`${gameFps}/${encFps} fps`);
  const streamNote = $("#streamNote");
  streamNote.textContent = liveBits.length ? liveBits.join(" · ") : "";
  streamNote.hidden = !liveBits.length;
  if (fromHeadset) $("#vrBitrateBar input").style.setProperty("--fill", `${((session.headsetBitrateMbps - 20) / 480) * 100}%`);
  const codec = session?.headsetCodec && session.headsetCodec !== "auto" ? session.headsetCodec : "";
  const chosen = [fromHeadset && t("video.overrideBitrate", { mbps: session.headsetBitrateMbps }), codec && { hevc: "HEVC", av1: "AV1", h264: "H.264" }[codec]].filter(Boolean);
  $("#overrideNote").textContent = chosen.length
    ? t("video.override", { list: chosen.join(t("misc.listSep")) })
    : t("video.overrideDefault");
  if (unsupported.length) $("#overrideNote").textContent += ` ${t("video.unsupported", { list: unsupported.join(t("misc.listSep")) })}`;
  // A19 audit #14 (read-only tier): the headset owns frame generation, the PC has no
  // switch for it - say its state where the other headset-owned picks are shown.
  if (session?.headsetFrameGeneration != null)
    $("#overrideNote").textContent += ` ${t("video.frameGenState", { state: session.headsetFrameGeneration ? t("video.frameGenOn") : t("video.frameGenOff") })}`;
  // A19 audit #3: the switch that flips bitrate ownership back lives on the headset.
  $("#overrideNote").textContent += ` ${t("video.overrideChangeHint")}`;
  // A86 (user, 2026-11-23): the tier slider is gone, so there is no per-tier note to
  // write and no `state.settings.streamProfile` to read - the tuner picks the rung and
  // the page shows nothing about it on purpose (the headset's stats overlay is where a
  // running picture tier can be seen).
  const h264 = state.settings.codec === "h264";
  setOff($("#tenBitRow"), h264);
  $("#tenBitHint").hidden = !h264;
  // TASK-CAPS-GUARD: when the effective codec pick is H264 (the headset's choice
  // first, this PC's setting when the headset follows "auto" - the same mirror
  // ChooseCodec resolves) and the current tier would hand the encoder a frame wider
  // than H264's 4096 px limit, the config layer rebuilds the stream on HEVC. The
  // live stream's encoded width IS that frame (vr_stream.cpp sizes it exactly the
  // way the guard measures it), so the hint says so where the codec is chosen.
  const codecPick = session?.headsetCodec && session.headsetCodec !== "auto" ? session.headsetCodec : state.settings.codec;
  const h264FrameW = session?.vr?.encWidth || 0;
  $("#codecHint").hidden = !(codecPick === "h264" && h264FrameW > 4096);
  // All three profiles read profileControlsVideo (each floors the fovea at its own
  // share; only Best picture adds two-pass and the QP map), so the checkbox stays
  // clickable on every profile.
  const tunable = ["nvenc", "amf"].includes(state.system.encoder.backend);
  setOff($("#aqRow"), !tunable);
  // The QP map is the user's own NVENC switch, no longer forced by a profile. It
  // cannot share a frame with the headset's geometric squeeze, so grey it out (and
  // say why) whenever that squeeze is on or the encoder doesn't support it.
  const nvenc = state.system.encoder.backend === "nvenc";
  const squeeze = !!state.session?.headsetGazeFoveation;
  $("#foveatedRow input").checked = !!state.settings.foveatedEncoding;
  setOff($("#foveatedRow"), !nvenc || squeeze);
  renderOscEyeStatus();
  // Two-pass is the user's own switch. A profile no longer forces it on (the old
  // Best-picture behaviour surprised users who had left it off), so the checkbox
  // simply mirrors the setting and is only greyed when the encoder can't do it.
  $("#twoPassRow input").checked = !!state.settings.twoPassEncoding;
  setOff($("#twoPassRow"), !tunable);
  // CBR only takes effect when the profile drives video settings, the headset isn't
  // following SteamVR's resolution, and the link isn't remote/slow (server-side gates).
  // A86b: CBR is a plain independent switch now. It used to be gated on the sharpest
  // tier AND on a "let the tier drive video" checkbox; both are gone, because a
  // tier-driven CBR switch oscillated against the tuner. Only the link gates remain.
  const cbrBlockedByLink = !!session && (!!session.matchSteamVr || !!session.slowPath);
  setOff($("#cbrRow"), cbrBlockedByLink);
  setOff($("#playOnPcRow"), !state.settings.streamAudio);
  setOff($("#micRow"), state.settings.audioDevices === "system");
}

function renderOscEyeStatus() {
  if (!state) return;
  const on = !!state.settings.externalEyeTracking;
  const opts = $("#externalEyeOpts");
  if (opts) opts.hidden = !on;
  const el = $("#externalEyeStatus");
  if (!el) return;
  if (!on) {
    el.textContent = "";
    return;
  }
  const osc = state.oscEye || {};
  const port = osc.port || state.settings.externalEyeOscPort || 9945;
  if (osc.bindError) {
    el.style.color = "#e05252";
    el.textContent = osc.bindError === 10048
      ? t("video.externalEyePortBusy", { port })
      : `${t("video.externalEyePortBusy", { port })} (WSA ${osc.bindError})`;
  } else if (osc.active) {
    el.style.color = "#3fb950";
    const nx = Number(osc.lastNormX || 0);
    const ny = Number(osc.lastNormY || 0);
    const sx = `${nx >= 0 ? "+" : ""}${nx.toFixed(2)}`;
    const sy = `${ny >= 0 ? "+" : ""}${ny.toFixed(2)}`;
    el.textContent = `${t("video.externalEyeConnected", { hz: Math.round(osc.rateHz || 0), addr: osc.lastAddress || "" })} (X: ${sx}, Y: ${sy})`;
  } else {
    el.style.color = "";
    el.textContent = t("video.externalEyeWaiting", { port });
  }
}

function renderAudio() {
  if (!state) return;
  const a = state.system.audioDriver;
  const steam = a.steam || {};
  const active = a.active || {};
  const row = (ok, name, inUse) => [name, ok ? (inUse ? t("audio.inUse") : t("audio.installed")) : t("audio.notInstalled"), ok ? "" : "no"];
  const rows = [
    row(steam.speaker, AUDIO_SETS.steam.speaker, active.speaker === "steam"),
    row(steam.microphone, AUDIO_SETS.steam.mic, active.microphone === "steam"),
  ];
  // The RinCast driver only ships with signed builds: don't list it as missing otherwise.
  if (a.speaker || a.microphone) {
    rows.unshift(row(a.microphone && a.microphoneFeed, AUDIO_SETS.rincast.mic, active.microphone === "rincast"));
    rows.unshift(row(a.speaker, AUDIO_SETS.rincast.speaker, active.speaker === "rincast"));
  }
  // Only what's missing needs saying.
  const missing = rows.filter((r) => r[2] === "no").map((r) => r[0]);
  $("#audioMissing").textContent = missing.length
    ? t(missing.length > 1 ? "audio.missingMany" : "audio.missingOne", { names: missing.join(t("misc.and")) })
    : "";
  const mic = AUDIO_SETS[active.microphone]?.mic;
  $("#micHint").textContent = mic ? t("audio.micHint", { mic }) : t("audio.micHintNone");
  const speaker = AUDIO_SETS[active.speaker]?.speaker;
  if (!speaker) $("#audioDevicesHint").textContent = t("audio.usingDefault");
  else {
    const list = mic && active.microphone !== active.speaker ? [speaker, mic].join(t("misc.and")) : speaker;
    $("#audioDevicesHint").textContent = t("audio.using", { list });
  }
}

async function allowUsbCable() {
  toast(t("toast.acceptPrompt"));
  const r = await request({ type: "allowUsbCable" }, 90000);
  if (r.ok) toast(t("toast.usbAllowed"));
  else showError(t("err.firewall"));
  await request({ type: "getState" });
}

async function setFirewall(allowPublic) {
  toast(t("toast.acceptPrompt"));
  const r = await request({ type: "setFirewall", allowPublic }, 90000);
  if (r.ok) toast(t(allowPublic ? "toast.publicAllowed" : "toast.publicClosed"));
  else showError(t("err.firewall"));
  await request({ type: "getState" });
}

// Opening up asks first, and says what it means. Closing doesn't need convincing.
async function choosePublicNetworks(on) {
  if (on) {
    const risk = state.settings.requireApproval
      ? t("confirm.public.riskGuarded")
      : t("confirm.public.riskUnguarded");
    const yes = await confirmAction(t("confirm.public.title"), t("confirm.public.text", { risk }), t("misc.allowOnPublic"));
    if (!yes) {
      renderFirewall();
      return;
    }
  }
  await setFirewall(on);
  renderFirewall();
}

function renderFirewall() {
  if (!state) return;  // push can arrive before the first getState reply
  const fw = state.system.firewall || {};
  const box = $("#allowPublic");
  box.checked = !!fw.publicAllowed;
  setOff($("#publicRow"), !!fw.publicUnguarded);
  $("#publicNote").textContent = fw.publicUnguarded ? t("general.publicNoteUnguarded") : t("general.publicNoteOpen");
}
$("#allowPublic").addEventListener("change", (e) => choosePublicNetworks(e.target.checked));

async function registerDriver() {
  const r = await request({ type: "registerSteamVrDriver" });
  if (r.ok) toast(t("toast.driverAdded"));
  else showError(t("err.driverRegister"));
  request({ type: "getState" });
}

function renderVr() {
  const sys = state.system;
  $$("#runtimeSeg input").forEach((b) => { b.checked = (b.value === "rincast" && sys.openxr.rincastActive) || (b.value === "steamvr" && sys.openxr.steamvrActive); });
  $("#runtimeRow").hidden = sys.openxr.rincastAvailable === false;
  $("#runtimeNote").textContent = sys.openxr.rincastActive || sys.openxr.steamvrActive ? "" : t("vr.otherRuntime");
  setFacts($("#vrFacts"), [
    [t("vr.status"), !sys.steamvr.installed ? t("vr.notInstalled") : sys.steamvr.running ? t("vr.running") : t("vr.notRunning"), sys.steamvr.installed ? "" : "no"],
    sys.steamvr.installed && [t("vr.driver"), sys.steamvr.driverRegistered ? t("vr.added") : t("vr.notAdded"), sys.steamvr.driverRegistered ? "" : "no"],
  ]);
  $("#registerDriver").hidden = !sys.steamvr.installed || sys.steamvr.driverRegistered;
  $("#launchSteamVR").hidden = !sys.steamvr.installed || sys.steamvr.running;
  $("#restartSteamVR").hidden = !sys.steamvr.running;
  $("#getSteamVR").hidden = sys.steamvr.installed;
}
$$("#runtimeSeg input").forEach((b) => b.addEventListener("change", async () => {
  toast(t("toast.acceptPrompt"));
  const r = await request({ type: "setOpenXrRuntime", runtime: b.value });
  if (r.ok) toast(t("toast.runtimeSwitch", { runtime: b.nextElementSibling.textContent }));
  else showError(t("err.runtimeSwitch"));
  request({ type: "getState" });
}));
$("#registerDriver").onclick = registerDriver;
$("#getSteamVR").onclick = () => send({ type: "openUrl", url: "https://store.steampowered.com/app/250820/SteamVR/" });
$("#launchSteamVR").onclick = () => { send({ type: "launchSteamVR" }); toast(t("toast.startingSteamvr")); };
$("#restartSteamVR").onclick = async () => {
  if (await confirmAction(t("confirm.restartSteamvr.title"), t("confirm.restartSteamvr.text"), t("confirm.restartSteamvr.action"))) { send({ type: "restartSteamVR" }); toast(t("toast.restartSteamvr")); }
};
$("#openLogs").onclick = () => send({ type: "openLogs" });
function renderElevate() {
  const sys = state && state.system;
  $("#elevateRow").hidden = !sys || !!sys.elevated;
  const blocked = !!(sys && sys.inputBlocked);
  const note = $("#elevateNote");
  note.textContent = blocked ? t("video.elevateBlocked") : t("video.elevateHint");
  note.classList.toggle("warn", blocked);
}
$("#elevateBtn").onclick = async () => {
  const yes = await confirmAction(t("video.elevate"), t("video.elevateConfirm"), t("video.elevate"), false);
  if (!yes) return;
  const r = await request({ type: "restartElevated" });
  if (!r.ok && r.error !== "cancelled") showError(t("video.elevateFailed"));
};

function renderDevices() {
  const list = $("#devices");
  const devices = state.settings.trustedDevices || [];
  const key = `${getLanguage()}:${JSON.stringify(devices)}`;
  if (list.dataset.key === key) return;
  list.dataset.key = key;
  if (!devices.length) {
    list.innerHTML = `<li class="empty">${esc(t("devices.empty"))}</li>`;
    return;
  }
  list.innerHTML = devices.map((d) => `
    <li><div class="meta"><b>${esc(d.name)}</b><small>${esc(t("devices.lastSeen", { date: d.lastSeen ? new Date(d.lastSeen * 1000).toLocaleDateString(locale()) : t("devices.never") }))}</small></div>
    <button class="btn" data-remove="${esc(d.id)}" data-name="${esc(d.name)}" aria-label="${esc(t("devices.removeAria", { name: d.name }))}">${esc(t("devices.remove"))}</button></li>`).join("");
  $$("[data-remove]", list).forEach((b) => (b.onclick = async () => {
    if (!(await confirmAction(t("confirm.remove.title", { name: b.dataset.name }), t("confirm.remove.text"), t("confirm.remove.action")))) return;
    const r = await request({ type: "removeTrusted", deviceId: b.dataset.remove });
    if (r.ok === false) showError(t("err.removeDevice"));
    else toast(t("toast.deviceRemoved"));
    request({ type: "getState" });
  }));
}

let authMode = "signIn";
let accountKey = "";
function renderAccount() {
  const view = $("#accountView");
  const c = state.cloud;
  const key = `${getLanguage()}:${c.signedIn ? `in:${c.userName}:${c.email}` : `out:${authMode}`}`;
  if (key !== accountKey) {
    accountKey = key;  // otherwise leave the form alone: state updates must not erase typing
    if (c.signedIn) renderSignedIn(view, c);
    else renderSignedOut(view);
  }
  if (c.signedIn) {
    const allow = $("#allowRemote");
    allow.checked = state.settings.allowRemote;
    $("#remoteStatus").textContent = !state.settings.allowRemote
      ? t("account.remoteOff")
      : { online: t("account.remoteOnline"), connecting: t("account.remoteConnecting"), error: c.error || t("account.remoteError") }[c.status] || "";
  }
}
function renderSignedIn(view, c) {
  view.innerHTML = `
    <div class="cols">
      <div>
        <h2 class="first">${esc(c.userName)}</h2>
        <p class="note selectable">${esc(c.email)}</p>
        <label class="check spaced"><input type="checkbox" data-setting="allowRemote" id="allowRemote"><span>${esc(t("account.allowRemote"))}</span><small id="remoteStatus"></small></label>
        <div class="btns spaced"><button class="btn" id="signOut">${esc(t("account.signOut"))}</button></div>
      </div>
      <div>
        <h2 class="first">${esc(t("account.signInHeadset"))}</h2>
        <h3 id="codeLabel">${esc(t("account.codeLabel"))}</h3>
        <form class="code-entry" id="codeForm"><input type="text" id="code" maxlength="9" placeholder="XXXX-XXXX" autocomplete="off" spellcheck="false" aria-labelledby="codeLabel"><button class="btn primary" id="codeSubmit">${esc(t("account.approve"))}</button></form>
        <p class="failed" id="codeError" role="alert"></p>
      </div>
    </div>`;
  wireSetting($("#allowRemote"));
  $("#codeForm").onsubmit = async (e) => {
    e.preventDefault();
    $("#codeError").textContent = "";
    const submit = $("#codeSubmit");
    submit.disabled = true;
    submit.textContent = t("account.approving");
    const r = await request({ type: "approveCode", code: $("#code").value });
    submit.disabled = false;
    submit.textContent = t("account.approve");
    if (r.ok) { toast(t("toast.headsetSignedIn")); $("#code").value = ""; }
    else $("#codeError").textContent = r.error || t("err.code");
  };
  $("#signOut").onclick = async () => {
    if (!(await confirmAction(t("confirm.signOut.title"), t("confirm.signOut.text"), t("confirm.signOut.action")))) return;
    const r = await request({ type: "signOut" });
    if (r.ok === false) showError(r.error || t("err.signOut"));
    else toast(t("toast.signedOut"));
    request({ type: "getState" });
  };
}
function renderSignedOut(view) {
  const register = authMode === "register";
  view.innerHTML = `
    <h2 class="first">${esc(register ? t("account.createTitle") : t("account.signInTitle"))}</h2>
    <p class="note auth-note">${esc(t("account.authNote"))}</p>
    <form class="auth" id="authForm" novalidate>
      ${register ? `<label for="authName">${esc(t("account.name"))}</label><input type="text" id="authName" name="name" autocomplete="nickname">` : ""}
      <label for="authEmail">${esc(t("account.email"))}</label>
      <input type="email" id="authEmail" name="email" autocomplete="email" required aria-describedby="authError">
      <label for="authPassword">${esc(t("account.password"))}${register ? ` ${esc(t("account.passwordHint"))}` : ""}</label>
      <input type="password" id="authPassword" name="password" autocomplete="${register ? "new-password" : "current-password"}" required aria-describedby="authError">
      <p class="error" id="authError" role="alert"></p>
      <button class="btn primary" id="authSubmit">${esc(register ? t("account.createSubmit") : t("account.signInSubmit"))}</button>
      <p class="switch-mode">${register
        ? `${esc(t("account.haveAccount"))} <button type="button" class="link" id="toggleAuth">${esc(t("account.signInSubmit"))}</button>`
        : `${esc(t("account.noAccount"))} <button type="button" class="link" id="toggleAuth">${esc(t("account.createSubmit"))}</button>`}</p>
      ${register ? "" : `<p class="switch-mode">${esc(t("account.forgot"))}</p>`}
    </form>`;
  $("#toggleAuth").onclick = () => { authMode = register ? "signIn" : "register"; renderAccount(); ($("#authName") || $("#authEmail")).focus(); };
  $("#authForm").onsubmit = async (e) => {
    e.preventDefault();
    const data = Object.fromEntries(new FormData(e.target));
    const err = $("#authError");
    err.textContent = "";
    if (!/^\S+@\S+\.\S+$/.test(data.email || "")) { err.textContent = t("err.email"); $("#authEmail").focus(); return; }
    if (register && (data.password || "").length < 8) { err.textContent = t("err.password"); $("#authPassword").focus(); return; }
    const btn = $("#authSubmit");
    btn.disabled = true;
    btn.textContent = register ? t("account.creating") : t("account.signingIn");
    const r = await request({ type: authMode, ...data });
    btn.disabled = false;
    btn.textContent = register ? t("account.createSubmit") : t("account.signInSubmit");
    if (!r.ok) { err.textContent = r.error || t("err.auth"); return; }
    toast(register ? t("toast.accountCreated") : t("toast.signedIn"));
    request({ type: "getState" });
  };
}

$("#downloadLink").onclick = () => send({ type: "openUrl", url: state.downloadUrl });

function renderRail() {
  $("#downloadLink").textContent = t("home.getIt", { site: state.downloadSite });
  setFacts($("#aboutFacts"), [
    [t("about.gpu"), state.system.gpu],
    state.system.encoder.backend && [t("about.encoder"), encoderText()],
    [t("about.version"), state.version],
  ]);
  $("#aboutLine").textContent = t("about.line", { port: state.system.port });
}

function renderAll() {
  bindSettings();
  renderHome();
  renderStatus();
  renderFirewall();
  renderDependents();
  renderAudio();
  renderVr();
  renderElevate();
  renderDevices();
  renderAccount();
  renderRail();
  if (state.pendingApproval) showApproval(state.pendingApproval);
}

window.addEventListener("resize", () => history.length && drawSpark());
fillLanguageOptions();
request({ type: "getState" });

// ---------------------------------------------------------------- design-preview mock
function mock(msg) {
  const demo = new URLSearchParams(location.search).get("demo") || "connected";
  const none = demo === "firstrun" || demo === "problem" || demo === "signedout";
  if (demo === "firstrun") localStorage.removeItem("connectedBefore");
  const audio = { speaker: false, microphone: false, microphoneFeed: false, steam: { speaker: !(demo === "problem"), microphone: !(demo === "problem") }, active: demo === "problem" ? {} : { speaker: "steam", microphone: "steam" } };
  const base = mock.base ||= {
    type: "state", version: "1.0.0", downloadUrl: "https://rincast.com/#download", downloadSite: "rincast.com",
    settings: { pcName: "Studio PC", codec: "auto", tenBit: false, vrBitrateMbps: 200, desktopBitrateMbps: 40, adaptiveBitrate: true, autoTune: false, streamProfile: 1, profileControlsVideo: false, qualityProfileCbr: false, fillMode: false, encodeScalePct: 0, pcBitrateWins: false, fecPercent: 5, foveatedEncoding: false, externalEyeTracking: false, externalEyeOscPort: 9945, externalEyeInvertX: false, externalEyeInvertY: false, externalEyeOffsetX: 0, externalEyeOffsetY: 0, externalEyeGainX: 1, externalEyeGainY: 1, adaptiveQuantization: true, twoPassEncoding: false, vrSlicedEncoding: false, desktopMaxHeight: 0, streamAudio: true, playOnPcToo: false, micPassthrough: true, switchDefaultAudio: true, audioDevices: "auto", autoRegisterSteamVrDriver: true, requireApproval: true, desktopInput: true, startWithWindows: true, preventSleep: true, cloudUrl: "https://rincast.com", allowRemote: true,
      trustedDevices: none ? [] : [{ id: "q3", name: "Quest 3", model: "Meta Quest 3", lastSeen: Date.now() / 1000 }] },
    cloud: demo === "signedout" || demo === "firstrun" ? { signedIn: false } : { signedIn: true, userName: "Sam", email: "sam@example.com", status: demo === "problem" ? "error" : "online", error: demo === "problem" ? "The server didn't answer." : "", allowRemote: true },
    system: { gpu: demo === "problem" ? "Microsoft Basic Display Adapter" : "NVIDIA GeForce RTX 5070 Ti", port: 38711, addresses: ["192.168.1.20"], vpnAddresses: demo === "vpn" ? [{ name: "Tailscale", address: "100.101.102.103" }] : [], firewall: { otherNetworksOpen: demo !== "problem", publicInUse: demo === "public", publicAllowed: false, publicUnguarded: false }, encoder: demo === "problem" ? {} : { backend: "nvenc", h264: true, hevc: true, av1: true, hevc10: true },
      steamvr: { installed: true, running: false, driverRegistered: demo !== "problem" }, openxr: { rincastAvailable: false, rincastActive: false, steamvrActive: true },
      audioDriver: audio, elevated: demo === "elevated", inputBlocked: demo === "blocked" },
    session: demo === "connected" || demo === "lowbandwidth" || demo === "paused" ? { deviceName: "Quest 3", model: "Meta Quest 3", address: "192.168.1.42", remote: demo === "lowbandwidth", connectedSeconds: 1460, rttMs: demo === "lowbandwidth" ? 42.6 : 3.4, vrState: "running", vrApp: "Half-Life: Alyx",
      headsetBitrateMbps: 0, headsetCodec: "auto", headsetFrameGeneration: demo === "paused" ? true : undefined, vr: { fps: demo === "lowbandwidth" || demo === "paused" ? 45 : 90, encodeMs: demo === "paused" ? 18 : 3.1, mbps: demo === "lowbandwidth" || demo === "paused" ? 7 : 182, codec: "HEVC", eyeWidth: 1536, eyeHeight: 1632, refreshRate: 90, latencyMs: demo === "lowbandwidth" || demo === "paused" ? 96 : 38, paused: demo === "paused" }, desktop: [],
      client: { latencyStages: demo === "lowbandwidth" ? { uplink: 21.3, game: 12.1, encoderWait: 0.7, encode: 4.4, downlink: 41.8, decode: 4.6, nextFrame: 3.1, compositor: 8 } : { uplink: 1.7, game: 11.4, encoderWait: 0.6, encode: 4.1, downlink: 5.3, decode: 4.2, nextFrame: 2.9, compositor: 7.8 } } } : null,
    pendingApproval: demo === "approval" ? { deviceId: "x", deviceName: "Quest 3", model: "Meta Quest 3", address: "192.168.1.42" } : null,
  };
  setTimeout(() => {
    if (msg.type === "setSettings") Object.assign(base.settings, msg.settings);
    base.settings._revision = (base.settings._revision || 0) + 1;
    if (msg.type === "getState") receive(structuredClone({ ...base, requestId: msg.requestId }));
    else if (msg.type === "restartElevated") receive({ type: "reply", requestId: msg.requestId, ok: false, error: "cancelled" });
    else if (msg.requestId) receive({ type: "reply", requestId: msg.requestId, ok: true, settings: structuredClone(base.settings) });
  }, 30);
  if (!mock.timer && base.session) {
    for (let i = 0; i < 40; i++) history.push(base.session.vr.mbps * (0.86 + Math.random() * 0.14));
    mock.timer = setInterval(() => {
      if (demo === "connected") {
        base.session.vr.mbps = 170 + Math.random() * 25;
        base.session.vr.latencyMs = 34 + Math.random() * 8;
      }
      receive({ type: "stats", session: base.session });
    }, 1000);
  }
}
