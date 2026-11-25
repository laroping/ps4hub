"use strict";

const TAURI = window.__TAURI__;
const invoke = TAURI ? TAURI.core.invoke : async () => null;
const listen = TAURI ? TAURI.event.listen : async () => () => {};
const appWin = TAURI ? TAURI.window.getCurrentWindow() : { minimize() {}, toggleMaximize() {}, close() {} };

const S = {
  cfg: null,
  server: { running: false, port: 8340, ip: "127.0.0.1", speed: 0 },
  queue: [],
  lib: [],
  scanning: false,
  logs: [],
  ifaces: [],
  view: "processing",
  libSrc: "base",
  libSearch: "",
  qSearch: "",
  qSel: new Set(),
  icons: new Map(),
  tidIcons: new Map(),
  iconReq: new Set(),
  iconQueue: [],
  iconBusy: 0,
  logFilter: "",
  consoleState: null,
  lastCfgKey: "",
  lastLookCls: "",
  dirtyQueue: true,
  dirtyLib: true,
  rafId: 0,
};

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];

const ICONS = {
  box: '<path d="M21 8 12 3 3 8v8l9 5 9-5z"/><path d="m3 8 9 5 9-5"/><path d="M12 13v8"/>',
  download: '<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/>',
  refresh: '<path d="M21 12a9 9 0 1 1-2.64-6.36"/><path d="M21 3v6h-6"/>',
  layers: '<polygon points="12 2 2 7 12 12 22 7 12 2"/><polyline points="2 17 12 22 22 17"/><polyline points="2 12 12 17 22 12"/>',
  power: '<path d="M12 2v10"/><path d="M18.4 6.6a9 9 0 1 1-12.77.04"/>',
  alert: '<path d="M12 9v4"/><path d="M12 17h.01"/><path d="M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z"/>',
  rotate: '<path d="M3 12a9 9 0 1 0 3-6.7L3 8"/><path d="M3 3v5h5"/>',
  check: '<path d="M20 6 9 17l-5-5"/>',
  chev: '<path d="M6 9l6 6 6-6"/>',
  play: '<polygon points="6 4 18 12 6 20 6 4" fill="currentColor" stroke="none"/>',
  pause: '<rect x="6" y="5" width="4" height="14" rx="1" fill="currentColor" stroke="none"/><rect x="14" y="5" width="4" height="14" rx="1" fill="currentColor" stroke="none"/>',
  stop: '<rect x="6" y="6" width="12" height="12" rx="2" fill="currentColor" stroke="none"/>',
  search: '<circle cx="11" cy="11" r="7"/><path d="m21 21-4.3-4.3"/>',
  trash: '<path d="M3 6h18"/><path d="M8 6V4h8v2"/><path d="M19 6l-1 14H6L5 6"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  file: '<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/>',
  server: '<rect x="2" y="2" width="20" height="8" rx="2"/><rect x="2" y="14" width="20" height="8" rx="2"/><line x1="6" y1="6" x2="6.01" y2="6"/><line x1="6" y1="18" x2="6.01" y2="18"/>',
  edit: '<path d="M12 20h9"/><path d="M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4z"/>',
  folder: '<path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/>',
  copy: '<rect x="9" y="9" width="12" height="12" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/>',
  wrench: '<path d="M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.77-3.77a6 6 0 0 1-7.94 7.94l-6.91 6.91a2.12 2.12 0 0 1-3-3l6.91-6.91a6 6 0 0 1 7.94-7.94l-3.76 3.76z"/>',
  info: '<circle cx="12" cy="12" r="10"/><line x1="12" y1="16" x2="12" y2="12"/><line x1="12" y1="8" x2="12.01" y2="8"/>',
  list: '<line x1="8" y1="6" x2="21" y2="6"/><line x1="8" y1="12" x2="21" y2="12"/><line x1="8" y1="18" x2="21" y2="18"/><line x1="3" y1="6" x2="3.01" y2="6"/><line x1="3" y1="12" x2="3.01" y2="12"/><line x1="3" y1="18" x2="3.01" y2="18"/>',
  x: '<path d="M18 6 6 18M6 6l12 12"/>',
  link: '<path d="M10 13a5 5 0 0 0 7.5.5l3-3a5 5 0 0 0-7-7l-1.7 1.7"/><path d="M14 11a5 5 0 0 0-7.5-.5l-3 3a5 5 0 0 0 7 7l1.7-1.7"/>',
  up: '<polyline points="18 15 12 9 6 15"/>',
  down: '<polyline points="6 9 12 15 18 9"/>',
};

const SVG_ATTRS = 'viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"';
const icon = (n) => `<svg ${SVG_ATTRS}>${ICONS[n] || ""}</svg>`;

function hydrateIcons(root = document) {
  $$("svg[data-icon]", root).forEach((el) => {
    for (const [k, v] of [["viewBox", "0 0 24 24"], ["fill", "none"], ["stroke", "currentColor"], ["stroke-width", "2"], ["stroke-linecap", "round"], ["stroke-linejoin", "round"]]) el.setAttribute(k, v);
    el.innerHTML = ICONS[el.dataset.icon] || "";
  });
}

function t(k, params = {}) {
  let s = (window.I18N && window.I18N.en && window.I18N.en[k]) || k;
  for (const [p, v] of Object.entries(params)) {
    s = s.replace(new RegExp(`\\{${p}\\}`, "g"), v);
  }
  return s;
}

function applyI18n(root = document) {
  const dict = (window.I18N && window.I18N.en) || {};
  $$("[data-i18n]", root).forEach((el) => {
    const k = el.dataset.i18n;
    if (dict[k] != null) el.textContent = dict[k];
  });
  $$("[data-i18n-title]", root).forEach((el) => {
    const k = el.dataset.i18nTitle;
    if (dict[k] != null) el.title = dict[k];
  });
  $$("[data-i18n-ph]", root).forEach((el) => {
    const k = el.dataset.i18nPh;
    if (dict[k] != null) el.placeholder = dict[k];
  });
}

const fmtSize = (b) => {
  if (!b) return "0 B";
  if (b >= 1073741824) return (b / 1073741824).toFixed(2) + " GB";
  if (b >= 1048576) return (b / 1048576).toFixed(1) + " MB";
  return Math.max(1, Math.round(b / 1024)) + " KB";
};
const fmtSpeed = (b) => (!b ? "" : b >= 1048576 ? (b / 1048576).toFixed(1) + " MB/s" : Math.round(b / 1024) + " KB/s");
const fmtEta = (s) => {
  if (s == null) return "-";
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), x = s % 60;
  return h ? `${h}h ${m}m` : m ? `${m}m ${x}s` : `${x}s`;
};
const fmtTime = (ms) => new Date(ms).toLocaleTimeString([], { hour12: false });
const esc = (s) => String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
const setText = (el, v) => { if (el && el.textContent !== v) el.textContent = v; };
const el = (tag, cls, html) => { const e = document.createElement(tag); if (cls) e.className = cls; if (html != null) e.innerHTML = html; return e; };

const ACTIVE = ["sending", "downloading", "waiting", "paused"];
const DONE = ["completed", "installed"];
const STATUS_NAMES = {
  queued: "Queued",
  sending: "Sending",
  waiting: "Waiting",
  downloading: "Downloading",
  paused: "Paused",
  completed: "Completed",
  installed: "Already installed",
  failed: "Failed",
  stopped: "Stopped",
};

const APPS = {
  flatz: { name: "PS4 RPI (flatZ)", port: 12800, hint: "PS4: run Remote Package Installer (flatZ) or PS4Hub on the console. Progress, pause and resume are read from the console." },
  etahen_v2: { name: "PS5 etaHEN - DPI v2 (12800)", port: 12800, hint: "PS5: enable \"Direct Package Installer V2\" in the etaHEN Toolbox (port 12800). etaHEN has no progress API - progress is measured by the bytes this server sends." },
  etahen_v1: { name: "PS5 etaHEN - DPI v1 (9090)", port: 9090, hint: "PS5: enable DPI (v1, port 9090) in /data/etaHEN/config.ini or the Toolbox. Progress is measured by the bytes this server sends." },
};

async function call(cmd, args) {
  try {
    return await invoke(cmd, args);
  } catch (e) {
    toast("error", String(e));
    throw e;
  }
}

const TOAST_MAX = 4;
const TOAST_MS = { ok: 2500, info: 2500, warn: 3500, error: 5000 };

function dismissToast(n) {
  if (n._gone) return;
  n._gone = true;
  clearTimeout(n._timer);
  n.classList.add("out");
  setTimeout(() => n.remove(), 260);
}

function armToast(n) {
  clearTimeout(n._timer);
  n._timer = setTimeout(() => dismissToast(n), TOAST_MS[n._kind] || 2500);
}

function toast(kind, msg) {
  const box = $("#toasts");
  const live = [...box.children].filter((x) => !x._gone);
  const dup = live.find((x) => x._kind === kind && x._msg === msg);
  if (dup) {
    dup._count++;
    const c = dup.querySelector(".tc");
    c.textContent = "×" + dup._count;
    c.hidden = false;
    armToast(dup);
    return;
  }
  const n = el("div", `toast ${kind}`, `<span class="ti"></span><span class="tm">${esc(msg)}</span><span class="tc" hidden></span>`);
  Object.assign(n, { _kind: kind, _msg: msg, _count: 1, title: msg });
  n.addEventListener("click", () => dismissToast(n));
  n.addEventListener("mouseenter", () => clearTimeout(n._timer));
  n.addEventListener("mouseleave", () => armToast(n));
  box.appendChild(n);
  live.push(n);
  while (live.length > TOAST_MAX) dismissToast(live.shift());
  armToast(n);
}

function confirmBox({ title, text, yes = "OK", no = "Cancel" }) {
  return new Promise((res) => {
    const m = $("#modal");
    setText($("#modal-title"), title);
    setText($("#modal-text"), text);
    setText($("#modal-yes"), yes);
    setText($("#modal-no"), no);
    m.classList.add("show");
    const done = (v) => {
      m.classList.remove("show");
      $("#modal-yes").onclick = $("#modal-no").onclick = m.onclick = null;
      res(v);
    };
    $("#modal-yes").onclick = () => done(true);
    $("#modal-no").onclick = () => done(false);
    m.onclick = (e) => { if (e.target === m) done(false); };
  });
}

function updateInk() {
  const ink = $("#tab-ink");
  if (!ink) return;
  const activeTab = $(`.tab.active`);
  if (!activeTab) return;
  ink.style.left = activeTab.offsetLeft + "px";
  ink.style.width = activeTab.offsetWidth + "px";
}

function showView(v) {
  S.view = v;
  $$(".view").forEach((x) => x.classList.toggle("active", x.id === "view-" + v));
  $$(".tab").forEach((tab) => {
    const isDirect = tab.dataset.view === v;
    const isMiscChild = ["logs", "tools", "about"].includes(v) && tab.id === "tab-miscs";
    tab.classList.toggle("active", isDirect || isMiscChild);
  });
  updateInk();
  if (v === "processing" && S.dirtyQueue) renderQueue();
  else if (v === "server" && S.dirtyLib) renderLib();
}

function applyThumbUrl(th, url) {
  if (!url) return;
  if (th.dataset.appliedUrl !== url) {
    th.style.backgroundImage = `url("${url}")`;
    th.dataset.appliedUrl = url;
  }
  if (!th.classList.contains("img")) th.classList.add("img");
}

function findTitleIdForFile(id) {
  const f = S.lib.find((x) => x.id === id);
  if (f && f.title_id) return f.title_id;
  const q = S.queue.find((x) => x.file_id === id);
  return q ? q.title_id : "";
}

function hasSharedIcon(tid) {
  if (!tid) return false;
  return S.lib.some((f) => f.title_id === tid && f.has_icon) || S.queue.some((q) => q.title_id === tid && q.has_icon);
}

function wantIcon(id, thumb, tid) {
  thumb.dataset.iconId = id;
  if (tid) thumb.dataset.tid = tid;
  const cached = S.icons.get(id) || (tid ? S.tidIcons.get(tid) : null);
  if (cached) {
    applyThumbUrl(thumb, cached);
    return;
  }
  if (S.iconReq.has(id)) return;
  S.iconReq.add(id);
  S.iconQueue.push(id);
  pumpIcons();
}

function pumpIcons() {
  while (S.iconBusy < 4 && S.iconQueue.length) {
    const id = S.iconQueue.shift();
    S.iconBusy++;
    invoke("get_icon", { id }).then((url) => {
      if (!url) return;
      S.icons.set(id, url);
      const tid = findTitleIdForFile(id);
      if (tid && !S.tidIcons.has(tid)) {
        S.tidIcons.set(tid, url);
        $$(`.badge-thumb[data-tid="${CSS.escape(tid)}"]:not(.img)`).forEach((th) => applyThumbUrl(th, url));
      }
      $$(`.badge-thumb[data-icon-id="${CSS.escape(id)}"]`).forEach((th) => applyThumbUrl(th, url));
    }).catch(() => {}).finally(() => {
      S.iconBusy--;
      pumpIcons();
    });
  }
}

const thumbLabel = (x) => (x.platform === "PS5" ? "PS5" : x.kind === "Patch" ? "UPD" : x.kind === "DLC" ? "DLC" : "PKG");
function setThumb(r, x, id) {
  const th = r.querySelector(".badge-thumb");
  if (!th) return;
  const lbl = thumbLabel(x);
  const tid = x.title_id || "";
  if (th.textContent !== lbl) th.textContent = lbl;
  if (tid && th.dataset.tid !== tid) th.dataset.tid = tid;
  const cached = S.icons.get(id) || (tid ? S.tidIcons.get(tid) : null);
  const wantCls = "badge-thumb " + lbl + (cached ? " img" : "");
  if (th.className !== wantCls) th.className = wantCls;
  if (cached) {
    applyThumbUrl(th, cached);
  } else if (x.has_icon || hasSharedIcon(tid)) {
    wantIcon(id, th, tid);
  }
}

function closeMenus() {
  $$(".menu").forEach((m) => m.classList.remove("show"));
}

function openMenu(menuId, anchor) {
  closeMenus();
  const menu = $("#" + menuId);
  if (!menu) return;
  const rect = anchor.getBoundingClientRect();
  menu.style.top = (rect.bottom + 4) + "px";
  menu.style.left = Math.min(rect.left, window.innerWidth - 210) + "px";
  menu.classList.add("show");
}

document.addEventListener("click", (e) => {
  const btn = e.target.closest("[data-menu]");
  if (btn) {
    e.stopPropagation();
    const menuId = btn.dataset.menu;
    const menu = $("#" + menuId);
    if (menu && menu.classList.contains("show")) {
      closeMenus();
    } else {
      openMenu(menuId, btn);
    }
    return;
  }
  if (!e.target.closest(".menu")) {
    closeMenus();
  }
});

function copyText(s) {
  const fallback = () => {
    const ta = el("textarea");
    ta.value = s;
    Object.assign(ta.style, { position: "fixed", opacity: "0" });
    document.body.appendChild(ta);
    ta.select();
    document.execCommand("copy");
    ta.remove();
  };
  if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(s).catch(fallback);
  else fallback();
}

function qRowOps(q) {
  const flatz = q.target === "flatz";
  const op = (a, ic, title, cls = "") => `<button class="op-btn ${cls}" data-a="${a}" title="${esc(title)}">${icon(ic)}</button>`;
  switch (q.status) {
    case "queued":
      return op("start", "play", "Start now") + op("remove", "x", "Remove", "danger");
    case "sending":
      return `<span class="op-btn"><span class="spinner"></span></span>`;
    case "downloading":
    case "waiting":
      return (flatz && q.task_id != null ? op("pause", "pause", "Pause") : "") + op("stop", "stop", "Stop", "danger") + op("remove", "x", "Remove", "danger");
    case "paused":
      return op("resume", "play", "Resume") + op("stop", "stop", "Stop", "danger") + op("remove", "x", "Remove", "danger");
    default:
      return op("retry", "rotate", "Retry") + op("remove", "x", "Remove", "danger");
  }
}

function qRowHtml(q) {
  const isErr = !!q.error;
  const sub = isErr ? q.error : `${esc(q.title_id || "")} · ${esc(q.name || "")}`;
  const kind = q.kind || "Game";
  const plat = q.target === "flatz" ? "PS4" : "PS5";
  const rest = q.status === "downloading" ? fmtEta(q.rest_sec) : "-";
  const statusName = STATUS_NAMES[q.status] || q.status;
  const pct = (q.progress || 0).toFixed(1);
  const isDl = q.status === "downloading";
  const isDone = DONE.includes(q.status);

  let metaLeft = "", metaRight = "";
  if (isDl) {
    metaLeft = `${pct}%`;
    metaRight = fmtSpeed(q.speed) || "…";
  } else if (isDone) {
    metaLeft = "100.0%";
    metaRight = fmtSize(q.size);
  } else if (q.progress > 0) {
    metaLeft = `${pct}%`;
    metaRight = fmtSize(q.transferred);
  }

  const barCls = isDl ? "live" : isDone ? "done" : q.status === "failed" ? "fail" : (q.status === "paused" || q.status === "stopped") ? "paused" : "";
  const barWidth = isDone ? 100 : (q.progress || 0);

  return `
    <label class="check plain"><input type="checkbox" class="q-chk" ${S.qSel.has(q.qid) ? "checked" : ""}></label>
    <div class="name-cell">
      <div class="badge-thumb"></div>
      <div class="name-txt">
        <div class="name-title">${esc(q.title || q.name)}</div>
        <div class="name-sub ${isErr ? "err" : ""}">${esc(sub)}</div>
      </div>
    </div>
    <div class="task-cell">
      <span class="tag-pill ${kind}">${esc(kind)}</span>
      <span class="tag-plat">${plat}</span>
    </div>
    <div class="rest-cell mono">${rest}</div>
    <div class="status-cell">
      <span class="status-pill ${q.status}"><span class="sd"></span>${statusName}</span>
    </div>
    <div class="size-cell mono r">${fmtSize(q.size)}</div>
    <div class="prog-cell">
      <div class="bar-wrap ${barCls}"><i style="width:${barWidth.toFixed(2)}%"></i></div>
      <div class="prog-meta"><span>${metaLeft}</span><span>${metaRight}</span></div>
    </div>
    <div class="ops-cell">
      ${qRowOps(q)}
    </div>
  `;
}

function createQRow(q) {
  const row = el("div", "row grid-q", qRowHtml(q));
  row._qid = q.qid;
  row._stKey = `${q.status}:${STATUS_NAMES[q.status] || q.status}`;
  row._opsKey = `${q.status}:${q.target}:${q.task_id != null}`;
  const progMetaSpans = row.querySelectorAll(".prog-meta span");
  row._refs = {
    chk: row.querySelector(".q-chk"),
    title: row.querySelector(".name-title"),
    sub: row.querySelector(".name-sub"),
    tagKind: row.querySelector(".tag-pill"),
    tagPlat: row.querySelector(".tag-plat"),
    rest: row.querySelector(".rest-cell"),
    statusCell: row.querySelector(".status-cell"),
    size: row.querySelector(".size-cell"),
    barWrap: row.querySelector(".bar-wrap"),
    barFill: row.querySelector(".bar-wrap i"),
    metaL: progMetaSpans[0],
    metaR: progMetaSpans[1],
    ops: row.querySelector(".ops-cell"),
  };
  row._refs.chk.addEventListener("change", (e) => {
    e.target.checked ? S.qSel.add(row._qid) : S.qSel.delete(row._qid);
    row.classList.toggle("sel", e.target.checked);
    updateSelectAll();
  });
  row._refs.ops.addEventListener("click", (e) => {
    const b = e.target.closest("[data-a]");
    if (b) qAction(row._qid, b.dataset.a, b);
  });
  return row;
}

function patchQRow(row, q) {
  const r = row._refs;
  const sel = S.qSel.has(q.qid);
  row.classList.toggle("sel", sel);
  if (r.chk.checked !== sel) r.chk.checked = sel;

  setThumb(row, q, q.file_id);

  const isErr = !!q.error;
  const sub = isErr ? q.error : `${q.title_id || ""} · ${q.name || ""}`;
  setText(r.title, q.title || q.name);
  setText(r.sub, sub);
  r.sub.classList.toggle("err", isErr);

  const kind = q.kind || "Game";
  const plat = q.target === "flatz" ? "PS4" : "PS5";
  const wantTagCls = `tag-pill ${kind}`;
  if (r.tagKind.className !== wantTagCls) r.tagKind.className = wantTagCls;
  setText(r.tagKind, kind);
  setText(r.tagPlat, plat);

  setText(r.rest, q.status === "downloading" ? fmtEta(q.rest_sec) : "-");

  const statusName = STATUS_NAMES[q.status] || q.status;
  const stKey = `${q.status}:${statusName}`;
  if (row._stKey !== stKey) {
    row._stKey = stKey;
    r.statusCell.innerHTML = `<span class="status-pill ${esc(q.status)}"><span class="sd"></span>${esc(statusName)}</span>`;
  }

  setText(r.size, fmtSize(q.size));

  const pct = (q.progress || 0).toFixed(1);
  const isDl = q.status === "downloading";
  const isDone = DONE.includes(q.status);
  let metaLeft = "", metaRight = "";
  if (isDl) {
    metaLeft = `${pct}%`;
    metaRight = fmtSpeed(q.speed) || "…";
  } else if (isDone) {
    metaLeft = "100.0%";
    metaRight = fmtSize(q.size);
  } else if (q.progress > 0) {
    metaLeft = `${pct}%`;
    metaRight = fmtSize(q.transferred);
  }

  const barCls = isDl ? "live" : isDone ? "done" : q.status === "failed" ? "fail" : (q.status === "paused" || q.status === "stopped") ? "paused" : "";
  const wantBarCls = barCls ? `bar-wrap ${barCls}` : "bar-wrap";
  if (r.barWrap.className !== wantBarCls) r.barWrap.className = wantBarCls;
  const barWidth = (isDone ? 100 : (q.progress || 0)).toFixed(2) + "%";
  if (r.barFill.style.width !== barWidth) r.barFill.style.width = barWidth;
  setText(r.metaL, metaLeft);
  setText(r.metaR, metaRight);

  const opsKey = `${q.status}:${q.target}:${q.task_id != null}`;
  if (row._opsKey !== opsKey) {
    row._opsKey = opsKey;
    r.ops.innerHTML = qRowOps(q);
  }
}

function renderQueue() {
  S.dirtyQueue = false;
  const tbody = $("#q-body");
  const pollMs = (S.cfg && S.cfg.poll_interval_ms) || 1500;
  const dur = Math.max(0.4, Math.min(2.0, pollMs / 1000)).toFixed(2) + "s";
  if (tbody._progDur !== dur) {
    tbody._progDur = dur;
    tbody.style.setProperty("--prog-dur", dur);
  }

  const s = S.qSearch.trim().toLowerCase();
  const filtered = S.queue.filter((q) => !s || (q.title + " " + q.name + " " + (q.title_id || "")).toLowerCase().includes(s));

  const active = S.queue.filter((q) => ACTIVE.includes(q.status)).length;
  const queued = S.queue.filter((q) => q.status === "queued").length;
  const done = S.queue.filter((q) => DONE.includes(q.status)).length;
  setText($("#pc-summary"), t("pc.summary", { active, queued, done }));

  const btnScanner = $("#btn-scanner");
  if (btnScanner && S.cfg) {
    btnScanner.classList.toggle("on", !!S.cfg.queue_scanner);
  }

  if (!filtered.length) {
    if (!tbody.querySelector(".empty")) {
      tbody.innerHTML = `<div class="empty"><div class="empty-box">${icon("box")}<span>${esc(t("empty.queue"))}</span></div></div>`;
    }
    $("#q-all").checked = false;
    $("#q-all").indeterminate = false;
    return;
  }

  const emp = tbody.querySelector(".empty");
  if (emp) emp.remove();

  const existing = new Map();
  for (const ch of [...tbody.children]) {
    if (ch._qid != null) existing.set(ch._qid, ch);
    else ch.remove();
  }

  const wantIds = new Set(filtered.map((q) => q.qid));
  for (const [id, ch] of existing) {
    if (!wantIds.has(id)) {
      ch.remove();
      existing.delete(id);
    }
  }

  let cursor = tbody.firstElementChild;
  for (const q of filtered) {
    let row = existing.get(q.qid);
    if (row) {
      patchQRow(row, q);
    } else {
      row = createQRow(q);
      row.classList.toggle("sel", S.qSel.has(q.qid));
      setThumb(row, q, q.file_id);
    }
    if (row === cursor) {
      cursor = cursor.nextElementSibling;
    } else {
      tbody.insertBefore(row, cursor);
    }
  }

  updateSelectAll();
}

function updateSelectAll() {
  const all = $("#q-all");
  const n = S.qSel.size;
  const total = S.queue.length;
  all.checked = total > 0 && n === total;
  all.indeterminate = n > 0 && n < total;
}

async function qAction(qid, a, btn) {
  try {
    if (a === "remove") return await invoke("queue_remove", { qids: [qid] });
    if (a === "retry") {
      await invoke("queue_action", { qid, action: "requeue" });
      return await invoke("queue_action", { qid, action: "start" });
    }
    if (btn) btn.disabled = true;
    await invoke("queue_action", { qid, action: a });
  } catch (e) {
    toast("error", String(e));
  } finally {
    if (btn) btn.disabled = false;
  }
}

function libItemStatus(f, qByFile) {
  const q = qByFile ? qByFile.get(f.id) : S.queue.findLast ? S.queue.findLast((x) => x.file_id === f.id) : (() => {
    for (let i = S.queue.length - 1; i >= 0; i--) if (S.queue[i].file_id === f.id) return S.queue[i];
    return null;
  })();
  if (!q) return null;
  if (q.status === "queued") return { cls: "inqueue", txt: t("st.inqueue") };
  if (q.status === "downloading") return { cls: "downloading", txt: `${t("st.downloading")} ${q.progress.toFixed(0)}%` };
  return { cls: q.status, txt: STATUS_NAMES[q.status] || q.status };
}

function libRowHtml(f, qByFile) {
  const st = libItemStatus(f, qByFile);
  const statusHtml = st ? `<span class="status-pill ${st.cls}"><span class="sd"></span>${esc(st.txt)}</span>` : "";
  const isBusy = !!(st && ["downloading", "sending", "waiting", "inqueue"].includes(st.cls));

  return `
    <div class="name-cell">
      <div class="badge-thumb"></div>
      <div class="name-txt">
        <div class="name-title">${esc(f.title || f.name)}</div>
        <div class="name-sub" title="${esc(f.path)}">${esc(f.name)}</div>
      </div>
    </div>
    <div class="tid mono">${esc(f.title_id || "-")}</div>
    <div class="status-cell">${statusHtml}</div>
    <div class="size mono r">${fmtSize(f.size)}</div>
    <div class="ops-cell">
      <span class="hover-only">
        <button class="op-btn" data-a="copy" title="${esc(t("op.copy"))}">${icon("link")}</button>
        <button class="op-btn" data-a="reveal" title="${esc(t("op.reveal"))}">${icon("folder")}</button>
        ${f.source === "dragged" ? `<button class="op-btn danger" data-a="remove" title="${esc(t("op.removeFile"))}">${icon("x")}</button>` : ""}
      </span>
      <button class="btn small" data-a="queue">${icon("plus")}${esc(t("op.queue"))}</button>
      <button class="btn small primary" data-a="install" ${isBusy ? "disabled" : ""}>${esc(t("op.install"))}</button>
    </div>
  `;
}

function createLibRow(f, qByFile) {
  const row = el("div", "row grid-lib", libRowHtml(f, qByFile));
  const st = libItemStatus(f, qByFile);
  row._fid = f.id;
  row._item = f;
  row._stKey = st ? `${st.cls}:${st.txt}` : "";
  row._src = f.source;
  row._refs = {
    title: row.querySelector(".name-title"),
    sub: row.querySelector(".name-sub"),
    tid: row.querySelector(".tid"),
    statusCell: row.querySelector(".status-cell"),
    size: row.querySelector(".size"),
    installBtn: row.querySelector('[data-a="install"]'),
    hoverOnly: row.querySelector(".hover-only"),
    ops: row.querySelector(".ops-cell"),
  };
  row._refs.ops.addEventListener("click", (e) => {
    const b = e.target.closest("[data-a]");
    if (b) libAction(row._item, b.dataset.a, b);
  });
  return row;
}

function patchLibRow(row, f, qByFile) {
  row._item = f;
  const r = row._refs;
  setThumb(row, f, f.id);
  setText(r.title, f.title || f.name);
  setText(r.sub, f.name);
  if (r.sub.title !== f.path) r.sub.title = f.path;
  setText(r.tid, f.title_id || "-");
  setText(r.size, fmtSize(f.size));

  const st = libItemStatus(f, qByFile);
  const stKey = st ? `${st.cls}:${st.txt}` : "";
  if (row._stKey !== stKey) {
    row._stKey = stKey;
    r.statusCell.innerHTML = st ? `<span class="status-pill ${esc(st.cls)}"><span class="sd"></span>${esc(st.txt)}</span>` : "";
  }
  const isBusy = !!(st && ["downloading", "sending", "waiting", "inqueue"].includes(st.cls));
  if (r.installBtn && r.installBtn.disabled !== isBusy) r.installBtn.disabled = isBusy;
  if (row._src !== f.source) {
    row._src = f.source;
    r.hoverOnly.innerHTML = `
      <button class="op-btn" data-a="copy" title="${esc(t("op.copy"))}">${icon("link")}</button>
      <button class="op-btn" data-a="reveal" title="${esc(t("op.reveal"))}">${icon("folder")}</button>
      ${f.source === "dragged" ? `<button class="op-btn danger" data-a="remove" title="${esc(t("op.removeFile"))}">${icon("x")}</button>` : ""}
    `;
  }
}

function renderLib() {
  S.dirtyLib = false;
  const tbody = $("#lib-body");
  const s = S.libSearch.trim().toLowerCase();
  const filtered = S.lib.filter((f) => {
    if (S.libSrc === "base" && f.source !== "base") return false;
    if (S.libSrc === "dragged" && f.source !== "dragged") return false;
    if (!s) return true;
    return (f.title + " " + f.name + " " + (f.title_id || "")).toLowerCase().includes(s);
  });

  const nBase = S.lib.filter((f) => f.source === "base").length;
  const nDragged = S.lib.filter((f) => f.source === "dragged").length;
  setText($("#cnt-base"), String(nBase));
  setText($("#cnt-dragged"), String(nDragged));

  $$("#lib-seg .btn").forEach((b) => b.classList.toggle("active", b.dataset.src === S.libSrc));
  $("#btn-rescan").classList.toggle("busy", S.scanning);

  if (!filtered.length) {
    const empKey = S.scanning ? "scan" : (S.libSrc === "base" && !nBase) ? "base" : (S.libSrc === "dragged" && !nDragged) ? "dragged" : "nodata";
    if (tbody._empKey !== empKey || !tbody.querySelector(".empty")) {
      tbody._empKey = empKey;
      if (empKey === "scan") {
        tbody.innerHTML = `<div class="empty"><div class="empty-box"><span class="spinner" style="width:36px;height:36px"></span><span>${esc(t("empty.scanning"))}</span></div></div>`;
      } else if (empKey === "base") {
        tbody.innerHTML = `<div class="empty"><div class="empty-box">${icon("folder")}<span>${esc(t("empty.base"))}</span></div></div>`;
      } else if (empKey === "dragged") {
        tbody.innerHTML = `<div class="empty"><div class="empty-box">${icon("download")}<span>${esc(t("empty.dragged"))}</span></div></div>`;
      } else {
        tbody.innerHTML = `<div class="empty"><div class="empty-box">${icon("search")}<span>${esc(t("empty.noData"))}</span></div></div>`;
      }
    }
    return;
  }

  tbody._empKey = "";
  const emp = tbody.querySelector(".empty");
  if (emp) emp.remove();

  const qByFile = new Map();
  for (const q of S.queue) qByFile.set(q.file_id, q);

  const existing = new Map();
  for (const ch of [...tbody.children]) {
    if (ch._fid != null) existing.set(ch._fid, ch);
    else ch.remove();
  }

  const wantIds = new Set(filtered.map((f) => f.id));
  for (const [id, ch] of existing) {
    if (!wantIds.has(id)) {
      ch.remove();
      existing.delete(id);
    }
  }

  let cursor = tbody.firstElementChild;
  for (const f of filtered) {
    let row = existing.get(f.id);
    if (row) {
      patchLibRow(row, f, qByFile);
    } else {
      row = createLibRow(f, qByFile);
      setThumb(row, f, f.id);
    }
    if (row === cursor) {
      cursor = cursor.nextElementSibling;
    } else {
      tbody.insertBefore(row, cursor);
    }
  }
}

async function libAction(f, a, btn) {
  try {
    if (a === "install") {
      btn.disabled = true;
      await invoke("install_now", { id: f.id });
      toast("ok", `Sending ${f.title}`);
    } else if (a === "queue") {
      const n = await invoke("queue_add", { ids: [f.id] });
      toast(n ? "ok" : "warn", n ? t("toast.added", { n: 1 }) : "Already in queue");
    } else if (a === "copy") {
      copyText(f.url);
      toast("ok", t("toast.copied"));
    } else if (a === "reveal") {
      await invoke("reveal_file", { path: f.path });
    } else if (a === "remove") {
      await invoke("remove_files", { ids: [f.id] });
    }
  } catch (e) {
    toast("error", String(e));
    if (btn) btn.disabled = false;
  }
}

async function pickFolder() {
  const p = await call("pick_folder").catch(() => null);
  if (p && S.cfg) {
    S.cfg.base_path = p;
    S.cfg.auto_scan = true;
    syncConfig();
    saveCfg(true);
    renderLib();
  }
}

let saveTimer = null;
function saveCfg(now) {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    invoke("save_config", { cfg: S.cfg }).catch((e) => toast("error", String(e)));
  }, now ? 0 : 250);
}

function renderIfaces() {
  const sel = $("#cfg-server_ip");
  if (!sel) return;
  const list = [...S.ifaces];
  if (S.cfg && S.cfg.server_ip && !list.some((i) => i.ip === S.cfg.server_ip)) {
    list.push({ name: "?", ip: S.cfg.server_ip });
  }
  const html = list.map((i) => `<option value="${esc(i.ip)}">${esc(i.name)}: ${esc(i.ip)}</option>`).join("");
  if (sel._h !== html) {
    sel.innerHTML = html;
    sel._h = html;
    if (S.cfg) sel.value = S.cfg.server_ip;
  }
}

function syncConfig(force) {
  if (!S.cfg) return;
  const activeCfg = document.activeElement && document.activeElement.hasAttribute && document.activeElement.hasAttribute("data-cfg");
  const cfgKey = JSON.stringify(S.cfg);
  if (!force && !activeCfg && S.lastCfgKey === cfgKey) return;
  renderIfaces();
  $$("[data-cfg]").forEach((inp) => {
    if (document.activeElement === inp) return;
    const v = S.cfg[inp.dataset.cfg];
    if (inp.type === "checkbox") {
      if (inp.checked !== !!v) inp.checked = !!v;
    } else if (String(inp.value) !== String(v ?? "")) {
      inp.value = v ?? "";
    }
  });

  const bp = $("#base-path");
  if (bp && bp.readOnly && bp.value !== (S.cfg.base_path || "")) {
    bp.value = S.cfg.base_path || "";
  }
  const autoScanChk = $("#cfg-auto_scan");
  if (autoScanChk) autoScanChk.disabled = !S.cfg.base_path;

  const appSel = $("#cfg-ps_app");
  if (appSel && appSel.value !== S.cfg.ps_app) appSel.value = S.cfg.ps_app;
  setText($("#app-hint"), (APPS[S.cfg.ps_app] || APPS.flatz).hint);

  $$("#swatches .swatch").forEach((s) => s.classList.toggle("active", s.dataset.accent === (S.cfg.accent || "blue")));
  if (!activeCfg) S.lastCfgKey = cfgKey;
}

function applyLook() {
  if (!S.cfg) return;
  const cls = `accent-${S.cfg.accent || "blue"} glass-${S.cfg.glass || "light"}${S.cfg.animations === false ? " no-anim" : ""}`;
  if (S.lastLookCls !== cls) {
    S.lastLookCls = cls;
    document.body.className = cls;
  }
}

function setApp(app) {
  if (!S.cfg || S.cfg.ps_app === app) return;
  S.cfg.ps_app = app;
  S.cfg.app_port = APPS[app].port;
  S.consoleState = null;
  setText($("#test-result"), "");
  syncConfig();
  saveCfg(true);
}

const FLAT_OK_SVG = '<svg viewBox="0 0 16 16" fill="currentColor" aria-hidden="true"><path d="M8 1.5a6.5 6.5 0 1 0 0 13 6.5 6.5 0 0 0 0-13Zm-1 9.2L4.3 8l1-1L7 8.7l3.7-3.7 1 1L7 10.7Z"/></svg>';
const FLAT_ERR_SVG = '<svg viewBox="0 0 16 16" fill="currentColor" aria-hidden="true"><path d="M8 1.5a6.5 6.5 0 1 0 0 13 6.5 6.5 0 0 0 0-13Zm2.8 8.3-1 1L8 9l-1.8 1.8-1-1L7 8 5.2 6.2l1-1L8 7l1.8-1.8 1 1L9 8l1.8 1.8Z"/></svg>';
const FLAT_UP_SVG = '<svg class="spd-ico" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true"><path d="M8 2 3.5 6.5h3v5h3v-5h3L8 2ZM3 13v1.5h10V13H3Z"/></svg>';

function setResult(el, kind, msg) {
  el.className = kind ? `result ${kind}` : "result";
  if (!kind) {
    el.textContent = msg;
    return;
  }
  const ico = kind === "ok" ? FLAT_OK_SVG : FLAT_ERR_SVG;
  el.innerHTML = `${ico}<span>${esc(msg)}</span>`;
}

function bindConfig() {
  $$("[data-cfg]").forEach((inp) => {
    inp.addEventListener("change", () => {
      if (!S.cfg) return;
      const k = inp.dataset.cfg;
      let v = inp.type === "checkbox" ? inp.checked : inp.value;
      if (inp.hasAttribute("data-num")) v = parseInt(v, 10) || 0;
      if (typeof v === "string") v = v.trim();
      S.cfg[k] = v;
      if (k === "ps_ip" || k === "app_port") S.consoleState = null;
      if (k === "ps_app") setApp(v);
      if (k === "queue_scanner") {
        const btnScanner = $("#btn-scanner");
        if (btnScanner) btnScanner.classList.toggle("on", !!v);
      }
      saveCfg();
      applyLook();
      syncConfig();
      renderQueue();
    });
  });

  $$("#swatches .swatch").forEach((s) => s.addEventListener("click", () => {
    if (!S.cfg) return;
    S.cfg.accent = s.dataset.accent;
    applyLook();
    syncConfig();
    saveCfg();
  }));

  const bp = $("#base-path");
  $("#btn-manual").addEventListener("click", () => {
    bp.readOnly = false;
    bp.focus();
    bp.select();
  });
  const commitBp = () => {
    if (bp.readOnly || !S.cfg) return;
    bp.readOnly = true;
    const v = bp.value.trim();
    if (v !== S.cfg.base_path) {
      S.cfg.base_path = v;
      saveCfg(true);
      renderLib();
    }
  };
  bp.addEventListener("keydown", (e) => {
    if (e.key === "Enter") commitBp();
    if (e.key === "Escape") {
      bp.value = S.cfg ? (S.cfg.base_path || "") : "";
      bp.readOnly = true;
    }
  });
  bp.addEventListener("blur", commitBp);
  $("#btn-pick").addEventListener("click", pickFolder);

  $("#btn-open-server").addEventListener("click", () => {
    if (!S.server.running) return toast("warn", "The server is not running");
    call("open_external", { target: `http://${S.cfg.server_ip}:${S.server.port}/` }).catch(() => {});
  });
  $("#btn-srv-restart").addEventListener("click", async (e) => {
    const b = e.currentTarget;
    b.classList.add("busy");
    try {
      await call("server_restart");
      toast("ok", "Server restarted");
    } catch {} finally {
      b.classList.remove("busy");
    }
  });
  $("#btn-srv-toggle").addEventListener("click", async () => {
    try {
      S.server.running ? await call("server_stop") : await call("server_start");
    } catch {}
  });

  $("#btn-test").addEventListener("click", testConsole);
  $("#btn-find-ps").addEventListener("click", findConsole);

  const tool = async (fn) => {
    const r = $("#tool-result");
    setResult(r, "", "…");
    try {
      setResult(r, "ok", await fn());
    } catch (e) {
      setResult(r, "err", String(e));
    }
  };
  $("#btn-tool-check").onclick = () => tool(async () => {
    const size = await invoke("check_installed", { titleId: $("#tool-tid").value });
    return size == null ? t("tools.notInstalled") : t("tools.installed", { size: fmtSize(size) });
  });
  const un = (kind, input, label) => async () => {
    const id = $(input).value.trim();
    if (!id) return;
    if (!(await confirmBox({ title: `Remove ${label}?`, text: `${id} will be uninstalled from the console.`, yes: "Remove" }))) return;
    tool(async () => {
      await invoke("uninstall", { kind, id });
      return "Removed";
    });
  };
  $("#btn-tool-ungame").onclick = un("game", "#tool-tid", "game");
  $("#btn-tool-unpatch").onclick = un("patch", "#tool-tid", "update");
  $("#btn-tool-undlc").onclick = un("dlc", "#tool-cid", "DLC");
  $("#btn-tool-untheme").onclick = un("theme", "#tool-cid", "theme");
}

async function testConsole() {
  const b = $("#btn-test"), res = $("#test-result");
  b.disabled = true;
  setResult(res, "", "Connecting…");
  try {
    setResult(res, "ok", await invoke("test_connection"));
    S.consoleState = "ok";
  } catch (err) {
    setResult(res, "err", String(err));
    S.consoleState = "err";
  } finally {
    b.disabled = false;
  }
}

async function findConsole() {
  const b = $("#btn-find-ps"), pop = $("#found-pop"), label = b.querySelector("span");
  b.disabled = true;
  b.classList.add("busy");
  if (label) label.textContent = t("scan.busy");
  try {
    const list = await invoke("scan_network");
    if (!list.length) {
      toast("warn", t("toast.noScan"));
      return;
    }
    pop.innerHTML = list.map((f, i) => `<button data-i="${i}"><span>${esc(f.ip)}</span><span class="sub">${esc(f.label)}</span></button>`).join("");
    pop.style.display = "block";
    pop.onclick = (ev) => {
      const x = ev.target.closest("[data-i]");
      if (!x) return;
      const f = list[+x.dataset.i];
      S.cfg.ps_ip = f.ip;
      S.cfg.ps_app = f.app;
      S.cfg.app_port = APPS[f.app].port;
      S.consoleState = "ok";
      pop.style.display = "none";
      syncConfig();
      saveCfg(true);
    };
  } catch (err) {
    toast("error", String(err));
  } finally {
    b.disabled = false;
    b.classList.remove("busy");
    if (label) label.textContent = t("cfg.findPs");
  }
}

document.addEventListener("mousedown", (e) => {
  const pop = $("#found-pop");
  if (pop && pop.style.display !== "none" && !pop.contains(e.target) && !$("#btn-find-ps").contains(e.target)) {
    pop.style.display = "none";
  }
});

function renderServer() {
  const s = S.server, c = S.cfg;
  const pill = $("#srv-pill");
  const txt = $("#srv-pill-txt");
  const spd = $("#srv-speed");
  const statusTag = $("#srv-status-tag");
  const hint = $("#srv-hint");

  const setCls = (el, c) => { if (el && el.className !== c) el.className = c; };
  if (s.running) {
    pill.classList.add("running");
    pill.classList.remove("err");
    setText(txt, `Server - ${c ? c.server_ip : s.ip}:${s.port}`);
    const spdTxt = s.speed ? fmtSpeed(s.speed) : "";
    if (spd._last !== spdTxt) {
      spd._last = spdTxt;
      spd.innerHTML = spdTxt ? `${FLAT_UP_SVG}<span>${esc(spdTxt)}</span>` : "";
    }
    if (statusTag) {
      setCls(statusTag, "tagline ok");
      setText(statusTag, t("st.running"));
    }
    if (hint) {
      setText(hint, t("srv.hintRun", { port: s.port, ip: c ? c.server_ip : s.ip }));
    }
    $("#btn-srv-toggle").classList.add("on");
  } else {
    pill.classList.remove("running");
    pill.classList.toggle("err", !!s.error);
    setText(txt, s.error ? "Server error" : t("pill.off"));
    if (spd._last !== "") {
      spd._last = "";
      spd.innerHTML = "";
    }
    if (statusTag) {
      setCls(statusTag, "tagline" + (s.error ? " err" : ""));
      setText(statusTag, s.error ? t("st.error") : t("st.stoppedSrv"));
    }
    if (hint) {
      setText(hint, s.error || t("srv.hintStop"));
    }
    $("#btn-srv-toggle").classList.remove("on");
  }

  const banner = $("#port-banner");
  const show = !s.running && !!s.error;
  banner.classList.toggle("hidden", !show);
  if (show) {
    setText($("#port-banner-txt"), s.error);
    const o = s.conflict, kb = $("#btn-kill-owner");
    kb.style.display = o && !o.protected ? "" : "none";
    if (o) setText(kb, t("banner.kill", { name: o.name }));
    $("#btn-next-port").style.display = /port/i.test(s.error) ? "" : "none";
  }
}

function logLine(l) {
  return el("div", "lg " + l.level, `<span class="ts">${fmtTime(l.ts)}</span><span class="lv">${esc(l.level)}</span><span class="m">${esc(l.msg)}</span>`);
}
const logMatches = (l) => !S.logFilter || l.level === S.logFilter;

function renderLogs() {
  const boxes = [$("#log-full"), $("#log-mini")].filter(Boolean);
  boxes.forEach((box) => {
    box.innerHTML = "";
    const f = document.createDocumentFragment();
    S.logs.filter(logMatches).slice(-300).forEach((l) => f.appendChild(logLine(l)));
    box.appendChild(f);
    box.scrollTop = box.scrollHeight;
  });
}

function pushLog(l) {
  S.logs.push(l);
  if (S.logs.length > 500) S.logs.shift();
  if (!logMatches(l)) return;
  [$("#log-full"), $("#log-mini")].filter(Boolean).forEach((box) => {
    const stick = box.scrollTop + box.clientHeight >= box.scrollHeight - 30;
    box.appendChild(logLine(l));
    while (box.children.length > 300) box.firstChild.remove();
    if (stick) box.scrollTop = box.scrollHeight;
  });
}

function toggleDrawer(force) {
  const drawer = $("#drawer");
  const show = force ?? !drawer.classList.contains("show");
  drawer.classList.toggle("show", show);
  $("#btn-logs").classList.toggle("on", show);
  if (show) renderLogs();
}

async function requestQuit() {
  const active = S.queue.filter((q) => ACTIVE.includes(q.status)).length;
  if (S.cfg && S.cfg.confirm_exit && active > 0) {
    const ok = await confirmBox({
      title: t("modal.quitT"),
      text: t("modal.quitP", { n: active }),
      yes: t("modal.yes"),
      no: t("modal.no"),
    });
    if (!ok) return;
  }
  invoke("quit_app");
}

function bindUi() {
  $("#win-min").onclick = () => appWin.minimize();
  $("#win-max").onclick = () => appWin.toggleMaximize();
  $("#win-close").onclick = requestQuit;

  $$(".tab[data-view]").forEach((b) => b.addEventListener("click", () => showView(b.dataset.view)));

  $("#btn-refresh").onclick = () => invoke("rescan");
  $("#btn-logs").onclick = () => toggleDrawer();
  $("#btn-power").onclick = requestQuit;
  $("#btn-drawer-close").onclick = () => toggleDrawer(false);

  $$("#menu-miscs button[data-go]").forEach((b) => {
    b.addEventListener("click", () => {
      closeMenus();
      showView(b.dataset.go);
    });
  });

  $("#btn-scanner").onclick = () => {
    if (!S.cfg) return;
    S.cfg.queue_scanner = !S.cfg.queue_scanner;
    $("#btn-scanner").classList.toggle("on", !!S.cfg.queue_scanner);
    const cb = $("#cfg-queue_scanner");
    if (cb) cb.checked = !!S.cfg.queue_scanner;
    saveCfg(true);
  };
  $("#btn-autostart").onclick = async () => {
    const n = await call("queue_autostart");
    if (n) toast("ok", t("toast.started", { n }));
    else toast("warn", t("toast.nothing"));
  };
  $("#q-search").addEventListener("input", (e) => {
    S.qSearch = e.target.value;
    renderQueue();
  });
  $("#q-all").addEventListener("change", (e) => {
    S.qSel.clear();
    if (e.target.checked) S.queue.forEach((q) => S.qSel.add(q.qid));
    renderQueue();
  });

  $$("#menu-reset button[data-act]").forEach((b) => {
    b.addEventListener("click", async () => {
      closeMenus();
      const a = b.dataset.act;
      if (a === "reset-sel") {
        const qids = [...S.qSel].filter((id) => {
          const q = S.queue.find((x) => x.qid === id);
          return q && !ACTIVE.includes(q.status);
        });
        if (qids.length) await call("queue_reset", { qids });
      } else if (a === "reset-failed") {
        const qids = S.queue.filter((q) => q.status === "failed" || q.status === "stopped").map((q) => q.qid);
        if (qids.length) await call("queue_reset", { qids });
      } else if (a === "rm-sel") {
        const qids = [...S.qSel];
        if (qids.length) {
          await call("queue_remove", { qids });
          S.qSel.clear();
        }
      } else if (a === "rm-done") {
        const qids = S.queue.filter((q) => DONE.includes(q.status) || q.status === "stopped").map((q) => q.qid);
        if (qids.length) await call("queue_remove", { qids });
      } else if (a === "rm-all") {
        if (!S.queue.length) return;
        const ok = await confirmBox({ title: "Clear queue?", text: "All items will be removed from the transfer queue.", yes: "Clear" });
        if (ok) {
          await call("queue_remove", { qids: S.queue.map((q) => q.qid) });
          S.qSel.clear();
        }
      }
      renderQueue();
    });
  });

  $$("#menu-check button[data-chk]").forEach((b) => {
    b.addEventListener("click", () => {
      closeMenus();
      const c = b.dataset.chk;
      S.qSel.clear();
      if (c === "all") {
        S.queue.forEach((q) => S.qSel.add(q.qid));
      } else if (c === "queued") {
        S.queue.filter((q) => q.status === "queued").forEach((q) => S.qSel.add(q.qid));
      } else if (c === "done") {
        S.queue.filter((q) => DONE.includes(q.status)).forEach((q) => S.qSel.add(q.qid));
      } else if (c === "failed") {
        S.queue.filter((q) => q.status === "failed" || q.status === "stopped").forEach((q) => S.qSel.add(q.qid));
      }
      renderQueue();
    });
  });

  $$("#lib-seg .btn[data-src]").forEach((b) => {
    b.addEventListener("click", () => {
      S.libSrc = b.dataset.src;
      renderLib();
    });
  });
  $("#lib-search").addEventListener("input", (e) => {
    S.libSearch = e.target.value;
    renderLib();
  });
  $("#btn-add-files").onclick = () => call("pick_files").catch(() => {});
  $("#btn-rescan").onclick = () => invoke("rescan");
  $("#btn-add-all").onclick = async () => {
    const s = S.libSearch.trim().toLowerCase();
    const filtered = S.lib.filter((f) => {
      if (S.libSrc === "base" && f.source !== "base") return false;
      if (S.libSrc === "dragged" && f.source !== "dragged") return false;
      if (!s) return true;
      return (f.title + " " + f.name + " " + (f.title_id || "")).toLowerCase().includes(s);
    });
    const ids = filtered.map((f) => f.id);
    const n = await call("queue_add", { ids });
    toast(n ? "ok" : "warn", n ? t("toast.added", { n }) : "All items already in queue");
  };
  $("#btn-remove-all").onclick = async () => {
    const ids = S.lib.filter((f) => f.source === S.libSrc).map((f) => f.id);
    if (!ids.length) return;
    const ok = await confirmBox({
      title: "Remove all files?",
      text: `${ids.length} file(s) will be removed from this list. Nothing will be deleted from disk.`,
      yes: "Remove",
    });
    if (ok) await call("remove_files", { ids });
  };

  $("#log-filter").addEventListener("change", (e) => {
    S.logFilter = e.target.value;
    renderLogs();
  });
  $("#btn-log-copy").onclick = () => {
    copyText(S.logs.map((l) => `${fmtTime(l.ts)} [${l.level}] ${l.msg}`).join("\n"));
    toast("ok", t("toast.copied"));
  };
  $("#btn-log-clear").onclick = () => {
    S.logs = [];
    renderLogs();
  };

  $("#btn-kill-owner").onclick = async () => {
    const o = S.server.conflict;
    if (!o) return;
    const ok = await confirmBox({
      title: t("modal.killT", { name: o.name }),
      text: t("modal.killP", { name: o.name, pid: o.pid, port: S.cfg.server_port }),
      yes: t("modal.kill"),
    });
    if (ok) call("kill_port_owner", { pid: o.pid }).catch(() => {});
  };
  $("#btn-next-port").onclick = () => call("use_next_port").then((p) => toast("ok", `Now using port ${p}`)).catch(() => {});
  $("#btn-retry-port").onclick = () => call("server_start").catch(() => {});

  window.addEventListener("resize", () => requestAnimationFrame(updateInk));
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") {
      closeMenus();
      toggleDrawer(false);
      const pop = $("#found-pop");
      if (pop) pop.style.display = "none";
    }
    if (e.key === "F5") {
      e.preventDefault();
      invoke("rescan");
    }
    if (e.ctrlKey && e.key.toLowerCase() === "f") {
      e.preventDefault();
      if (S.view === "processing") $("#q-search").focus();
      else if (S.view === "server") $("#lib-search").focus();
    }
  });

  document.addEventListener("contextmenu", (e) => {
    if (!e.target.closest("input, textarea, .loglist")) e.preventDefault();
  });
}

function flushRender() {
  S.rafId = 0;
  applyLook();
  syncConfig();
  renderServer();
  if (S.view === "processing") renderQueue();
  else if (S.view === "server") renderLib();
}

function onSnapshot(s, immediate) {
  S.cfg = s.cfg;
  S.server = s.server;
  S.queue = s.queue;
  if (s.library) S.lib = s.library;
  S.scanning = s.scanning;
  S.dirtyQueue = true;
  S.dirtyLib = true;

  if (immediate) {
    if (S.rafId) { cancelAnimationFrame(S.rafId); S.rafId = 0; }
    flushRender();
    return;
  }
  if (!S.rafId) S.rafId = requestAnimationFrame(flushRender);
}

async function init() {
  applyI18n();
  hydrateIcons();
  bindUi();

  try { S.ifaces = await invoke("list_interfaces"); } catch {}
  const snap = await invoke("get_snapshot");
  onSnapshot(snap, true);
  S.logs = (await invoke("get_logs")) || [];
  bindConfig();
  showView("processing");

  try {
    const ver = await TAURI.app.getVersion();
    setText($("#about-ver"), "v" + ver);
  } catch {}
  $("#about-gh")?.addEventListener("click", (e) => {
    e.preventDefault();
    call("open_external", { target: "https://github.com/laroping" }).catch(() => {});
  });

  await listen("snapshot", (e) => onSnapshot(e.payload));
  await listen("log", (e) => pushLog(e.payload));
  await listen("toast", (e) => toast(e.payload.kind, e.payload.msg));
  await listen("drag", (e) => {
    $("#drop").classList.toggle("show", e.payload === "enter");
    if (e.payload === "drop") {
      S.libSrc = "dragged";
      showView("server");
    }
  });

  setInterval(async () => {
    try {
      S.ifaces = await invoke("list_interfaces");
      renderIfaces();
    } catch {}
  }, 15000);
}

init().catch((e) => toast("error", "init: " + e));
