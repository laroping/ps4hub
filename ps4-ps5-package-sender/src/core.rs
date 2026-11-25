//! Application core: config, PKG library, install queue, queue scanner and
//! the snapshot pump that feeds the UI.

use serde::{Deserialize, Serialize};
use std::collections::VecDeque;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use tauri::{AppHandle, Emitter};

use crate::console::{Console, InstallMeta, InstallOutcome, Target};
use crate::pkg;
use crate::server::{self, Entry, HttpServer, StartError};
use crate::win::{self, PortOwner};

#[derive(Serialize, Deserialize, Clone, Debug)]
#[serde(default)]
pub struct Config {
    pub server_ip: String,
    pub server_port: u16,
    pub auto_start_server: bool,
    pub base_path: String,
    pub auto_scan: bool,
    pub deep_scan: bool,
    pub url_prefix: bool,
    pub queue_scanner: bool,
    pub ps_ip: String,
    pub ps_app: String,
    pub app_port: u16,
    pub poll_interval_ms: u64,
    pub max_parallel: u32,
    pub delay_between_s: u64,
    pub request_timeout_s: u64,
    pub check_installed: bool,
    pub send_icon: bool,
    pub language: String,
    pub accent: String,
    pub animations: bool,
    pub glass: String,
    pub confirm_exit: bool,
    pub dragged_files: Vec<String>,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            server_ip: String::new(),
            server_port: 8339,
            auto_start_server: true,
            base_path: String::new(),
            auto_scan: true,
            deep_scan: false,
            url_prefix: false,
            queue_scanner: true,
            ps_ip: String::new(),
            ps_app: "flatz".into(),
            app_port: 12800,
            poll_interval_ms: 1500,
            max_parallel: 1,
            delay_between_s: 2,
            request_timeout_s: 8,
            check_installed: false,
            send_icon: true,
            language: "en".into(),
            accent: "blue".into(),
            animations: true,
            glass: "light".into(),
            confirm_exit: true,
            dragged_files: vec![],
        }
    }
}

#[derive(Serialize, Clone, Debug)]
pub struct LibFile {
    pub id: String,
    pub path: String,
    pub name: String,
    pub dir: String,
    pub size: u64,
    pub title: String,
    pub title_id: String,
    pub content_id: String,
    pub category: String,
    pub kind: String,
    pub app_ver: String,
    pub sys_ver: String,
    pub platform: String,
    pub has_icon: bool,
    pub source: String,
    pub url: String,
    #[serde(skip)]
    pub icon: Option<(u64, u32)>,
}

#[derive(Serialize, Clone, Debug)]
pub struct QueueItem {
    pub qid: u64,
    pub file_id: String,
    pub path: String,
    pub name: String,
    pub title: String,
    pub title_id: String,
    pub content_id: String,
    pub kind: String,
    pub size: u64,
    pub has_icon: bool,
    pub target: String,
    pub status: String,
    pub progress: f64,
    pub transferred: u64,
    pub rest_sec: Option<u64>,
    pub speed: u64,
    pub task_id: Option<i64>,
    pub error: Option<String>,
    pub added_at: u64,
    pub started_at: Option<u64>,
    pub finished_at: Option<u64>,
    #[serde(skip)]
    icon: Option<(u64, u32)>,
    #[serde(skip)]
    poll_failures: u32,
    #[serde(skip)]
    last_bytes: u64,
    #[serde(skip)]
    last_sample: Option<Instant>,
}

impl QueueItem {
    fn active(&self) -> bool {
        matches!(
            self.status.as_str(),
            "sending" | "downloading" | "waiting" | "paused"
        )
    }
    fn finished(&self) -> bool {
        matches!(self.status.as_str(), "completed" | "installed")
    }
}

#[derive(Serialize, Clone, Debug, Default)]
pub struct ServerStatus {
    pub running: bool,
    pub port: u16,
    pub ip: String,
    pub error: Option<String>,
    pub conflict: Option<PortOwner>,
    pub connections: usize,
    pub speed: u64,
    pub total_sent: u64,
}

#[derive(Serialize, Clone, Debug)]
pub struct LogLine {
    pub seq: u64,
    pub ts: u64,
    pub level: String,
    pub msg: String,
}

pub struct Inner {
    pub cfg: Config,
    pub library: Vec<LibFile>,
    pub queue: Vec<QueueItem>,
    pub server: ServerStatus,
    pub scanning: bool,
    last_finish: Option<Instant>,
}

#[derive(Serialize)]
pub struct Snapshot {
    pub cfg: Config,
    pub server: ServerStatus,
    pub queue: Vec<QueueItem>,
    pub library: Option<Vec<LibFile>>,
    pub scanning: bool,
}

pub struct Core {
    pub app: OnceLock<AppHandle>,
    pub inner: Mutex<Inner>,
    pub server: HttpServer,
    cfg_path: PathBuf,
    dirty: AtomicBool,
    lib_dirty: AtomicBool,
    next_qid: AtomicU64,
    logs: Mutex<VecDeque<LogLine>>,
    log_seq: AtomicU64,
    shutting_down: AtomicBool,
}

pub fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

pub fn file_id(path: &str) -> String {
    // FNV-1a 64 over the normalised path: stable, short, URL-safe.
    let mut h: u64 = 0xcbf29ce484222325;
    for b in path.replace('/', "\\").to_lowercase().bytes() {
        h ^= b as u64;
        h = h.wrapping_mul(0x100000001b3);
    }
    format!("{:012x}", h & 0xFFFF_FFFF_FFFF)
}

fn kind_for(cat: &str, name: &str) -> String {
    let k = pkg::category_label(cat);
    if k != "PKG" {
        return k.into();
    }
    let l = name.to_lowercase();
    if l.contains("patch") || l.contains("update") || l.contains("[upd") {
        "Patch".into()
    } else if l.contains("dlc") {
        "DLC".into()
    } else {
        "PKG".into()
    }
}

pub fn build_libfile(path: &Path, source: &str) -> Option<LibFile> {
    let md = std::fs::metadata(path).ok()?;
    if !md.is_file() {
        return None;
    }
    let p = path.to_string_lossy().to_string();
    let name = path.file_name()?.to_string_lossy().to_string();
    let meta = pkg::read_meta(path);
    Some(LibFile {
        id: file_id(&p),
        dir: path
            .parent()
            .map(|d| d.to_string_lossy().to_string())
            .unwrap_or_default(),
        size: md.len(),
        title: if meta.title.is_empty() {
            name.trim_end_matches(".pkg")
                .trim_end_matches(".PKG")
                .to_string()
        } else {
            meta.title.clone()
        },
        kind: kind_for(&meta.category, &name),
        title_id: meta.title_id,
        content_id: meta.content_id,
        category: meta.category,
        app_ver: if meta.app_ver.is_empty() {
            meta.version
        } else {
            meta.app_ver
        },
        sys_ver: meta.sys_ver,
        platform: meta.platform,
        has_icon: meta.icon.is_some(),
        icon: meta.icon,
        source: source.into(),
        url: String::new(),
        path: p,
        name,
    })
}

fn is_pkg(p: &Path) -> bool {
    p.extension()
        .map(|e| e.eq_ignore_ascii_case("pkg"))
        .unwrap_or(false)
}

pub fn collect_pkgs(dir: &Path, deep: bool, out: &mut Vec<PathBuf>, depth: u32) {
    let Ok(rd) = std::fs::read_dir(dir) else {
        return;
    };
    for e in rd.flatten() {
        let p = e.path();
        let Ok(ft) = e.file_type() else { continue };
        if ft.is_dir() {
            let n = e.file_name().to_string_lossy().to_lowercase();
            if deep && depth < 12 && !n.starts_with('$') && n != "system volume information" {
                collect_pkgs(&p, deep, out, depth + 1);
            }
        } else if is_pkg(&p) {
            out.push(p);
        }
    }
}

impl Core {
    pub fn new(cfg_path: PathBuf) -> Arc<Self> {
        let cfg: Config = std::fs::read_to_string(&cfg_path)
            .ok()
            .and_then(|s| serde_json::from_str(&s).ok())
            .unwrap_or_default();
        let core = Arc::new_cyclic(|weak: &std::sync::Weak<Core>| {
            let w = weak.clone();
            let log: server::LogFn = Arc::new(move |m: String| {
                if let Some(c) = w.upgrade() {
                    let lvl = if m.starts_with("ERROR") {
                        "error"
                    } else {
                        "server"
                    };
                    c.log(lvl, m.trim_start_matches("ERROR ").to_string());
                }
            });
            Core {
                app: OnceLock::new(),
                inner: Mutex::new(Inner {
                    cfg,
                    library: vec![],
                    queue: vec![],
                    server: ServerStatus::default(),
                    scanning: false,
                    last_finish: None,
                }),
                server: HttpServer::new(log),
                cfg_path,
                dirty: AtomicBool::new(true),
                lib_dirty: AtomicBool::new(true),
                next_qid: AtomicU64::new(1),
                logs: Mutex::new(VecDeque::new()),
                log_seq: AtomicU64::new(1),
                shutting_down: AtomicBool::new(false),
            }
        });
        core
    }

    pub fn log(&self, level: &str, msg: impl Into<String>) {
        let line = LogLine {
            seq: self.log_seq.fetch_add(1, Ordering::Relaxed),
            ts: now_ms(),
            level: level.into(),
            msg: msg.into(),
        };
        #[cfg(debug_assertions)]
        eprintln!("[{}] {}", line.level, line.msg);
        {
            let mut l = self.logs.lock().unwrap();
            l.push_back(line.clone());
            while l.len() > 500 {
                l.pop_front();
            }
        }
        if let Some(app) = self.app.get() {
            let _ = app.emit("log", &line);
        }
    }

    pub fn logs(&self) -> Vec<LogLine> {
        self.logs.lock().unwrap().iter().cloned().collect()
    }

    pub fn toast(&self, kind: &str, msg: impl Into<String>) {
        if let Some(app) = self.app.get() {
            let _ = app.emit(
                "toast",
                serde_json::json!({ "kind": kind, "msg": msg.into() }),
            );
        }
    }

    pub fn mark_dirty(&self) {
        self.dirty.store(true, Ordering::Release);
    }
    fn mark_lib_dirty(&self) {
        self.lib_dirty.store(true, Ordering::Release);
        self.dirty.store(true, Ordering::Release);
    }

    pub fn save_config(&self) {
        let cfg = self.inner.lock().unwrap().cfg.clone();
        if let Some(dir) = self.cfg_path.parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        let tmp = self.cfg_path.with_extension("tmp");
        if std::fs::write(&tmp, serde_json::to_string_pretty(&cfg).unwrap_or_default()).is_ok() {
            let _ = std::fs::rename(&tmp, &self.cfg_path);
        }
    }

    pub fn ensure_server_ip(&self) {
        let ifs = win::interfaces();
        let mut g = self.inner.lock().unwrap();
        if g.cfg.server_ip.is_empty() || !ifs.iter().any(|i| i.ip == g.cfg.server_ip) {
            let pick = ifs
                .iter()
                .find(|i| i.ip.starts_with("192.168."))
                .or_else(|| ifs.iter().find(|i| i.ip.starts_with("10.")))
                .or(ifs.first());
            if let Some(i) = pick {
                g.cfg.server_ip = i.ip.clone();
            }
        }
    }

    pub fn apply_config(self: &Arc<Self>, mut new: Config) {
        let (rescan, restart, urls_changed) = {
            let mut g = self.inner.lock().unwrap();
            new.dragged_files = g.cfg.dragged_files.clone();
            new.max_parallel = new.max_parallel.clamp(1, 8);
            new.poll_interval_ms = new.poll_interval_ms.clamp(500, 10_000);
            new.request_timeout_s = new.request_timeout_s.clamp(2, 60);
            if new.server_port == 0 {
                new.server_port = 8339;
            }
            let old = std::mem::replace(&mut g.cfg, new);
            let rescan = old.base_path != g.cfg.base_path || old.deep_scan != g.cfg.deep_scan;
            let restart = old.server_port != g.cfg.server_port && g.server.running;
            let urls = old.server_ip != g.cfg.server_ip
                || old.server_port != g.cfg.server_port
                || old.url_prefix != g.cfg.url_prefix;
            (rescan, restart, urls)
        };
        self.save_config();
        if urls_changed {
            self.refresh_urls();
        }
        if restart {
            let c = self.clone();
            std::thread::spawn(move || {
                c.stop_server();
                let _ = c.start_server(true);
            });
        }
        if rescan {
            self.scan_base();
        }
        self.mark_dirty();
    }

    fn refresh_urls(&self) {
        let mut g = self.inner.lock().unwrap();
        let (ip, port, pre) = (g.cfg.server_ip.clone(), g.cfg.server_port, g.cfg.url_prefix);
        for f in g.library.iter_mut() {
            f.url = format!(
                "http://{ip}:{port}{}",
                server::url_path(&f.id, &f.path, &f.name, pre)
            );
        }
        g.server.ip = ip;
        drop(g);
        self.mark_lib_dirty();
    }

    /// Start the file server. If the port is held by a stale instance of
    /// this program it is terminated automatically; any other owner is
    /// reported to the UI so the user can decide.
    pub fn start_server(self: &Arc<Self>, auto_kill_self: bool) -> Result<(), String> {
        let port = self.inner.lock().unwrap().cfg.server_port;
        let mut attempt = 0;
        loop {
            attempt += 1;
            // Windows lets a second process bind a port whose owner used
            // SO_REUSEADDR, so a successful bind() proves nothing. Ask the
            // TCP table who is listening first.
            let precheck = if self.server.is_running() {
                None
            } else {
                win::port_owner(port)
            };
            let res = if precheck.is_some() {
                Err(StartError::InUse)
            } else {
                self.server.start(port)
            };
            match res {
                Ok(()) => {
                    let mut g = self.inner.lock().unwrap();
                    g.server.running = true;
                    g.server.port = port;
                    g.server.ip = g.cfg.server_ip.clone();
                    g.server.error = None;
                    g.server.conflict = None;
                    drop(g);
                    self.log("info", format!("Server listening on 0.0.0.0:{port}"));
                    self.mark_dirty();
                    return Ok(());
                }
                Err(StartError::InUse) => {
                    let owner = win::port_owner(port);
                    if let Some(o) = &owner {
                        if o.is_self_exe && auto_kill_self && attempt <= 2 {
                            self.log(
                                "warn",
                                format!(
                                    "Port {port} held by a stale instance (PID {}), closing it",
                                    o.pid
                                ),
                            );
                            if win::kill_process(o.pid).is_ok() {
                                std::thread::sleep(Duration::from_millis(300));
                                continue;
                            }
                        }
                    }
                    // Windows may still be tearing the socket down - retry briefly.
                    if owner.is_none() && attempt <= 10 {
                        std::thread::sleep(Duration::from_millis(250));
                        continue;
                    }
                    let msg = match &owner {
                        Some(o) => format!("Port {port} is used by {} (PID {})", o.name, o.pid),
                        None => format!("Port {port} is already in use"),
                    };
                    let mut g = self.inner.lock().unwrap();
                    g.server.running = false;
                    g.server.error = Some(msg.clone());
                    g.server.conflict = owner;
                    drop(g);
                    self.log("error", msg.clone());
                    self.mark_dirty();
                    return Err(msg);
                }
                Err(StartError::Denied(m)) | Err(StartError::Other(m)) => {
                    let mut g = self.inner.lock().unwrap();
                    g.server.running = false;
                    g.server.error = Some(m.clone());
                    g.server.conflict = None;
                    drop(g);
                    self.log("error", m.clone());
                    self.mark_dirty();
                    return Err(m);
                }
            }
        }
    }

    pub fn stop_server(&self) {
        let was = self.server.is_running();
        self.server.stop();
        let mut g = self.inner.lock().unwrap();
        g.server.running = false;
        g.server.connections = 0;
        g.server.speed = 0;
        drop(g);
        if was {
            self.log("info", "Server stopped, port released");
        }
        self.mark_dirty();
    }

    pub fn kill_port_owner(self: &Arc<Self>, pid: u32) -> Result<(), String> {
        let port = self.inner.lock().unwrap().cfg.server_port;
        match win::port_owner(port) {
            Some(o) if o.pid == pid => {
                if o.protected {
                    return Err(format!("{} is a protected system process", o.name));
                }
                win::kill_process(pid)?;
                self.log(
                    "warn",
                    format!("Terminated {} (PID {pid}) to free port {port}", o.name),
                );
                std::thread::sleep(Duration::from_millis(300));
                self.start_server(false)
            }
            _ => self.start_server(false),
        }
    }

    pub fn use_next_free_port(self: &Arc<Self>) -> Result<u16, String> {
        let start = self.inner.lock().unwrap().cfg.server_port;
        for p in start.saturating_add(1)..start.saturating_add(200) {
            if std::net::TcpListener::bind(("0.0.0.0", p)).is_ok() {
                {
                    self.inner.lock().unwrap().cfg.server_port = p;
                }
                self.save_config();
                self.refresh_urls();
                self.start_server(false)?;
                return Ok(p);
            }
        }
        Err("no free port found".into())
    }

    fn register(&self, f: &LibFile) {
        self.server.register(Entry {
            id: f.id.clone(),
            path: PathBuf::from(&f.path),
            name: f.name.clone(),
            icon: f.icon,
        });
    }

    fn unregister_if_unused(&self, g: &Inner, id: &str) {
        let in_lib = g.library.iter().any(|f| f.id == id);
        let in_q = g.queue.iter().any(|q| q.file_id == id);
        if !in_lib && !in_q {
            self.server.unregister(id);
        }
    }

    fn insert_files(&self, files: Vec<LibFile>, source: &str, replace_source: bool) -> usize {
        for f in &files {
            self.register(f);
        }
        let mut g = self.inner.lock().unwrap();
        if replace_source {
            let removed: Vec<String> = g
                .library
                .iter()
                .filter(|f| f.source == source)
                .map(|f| f.id.clone())
                .collect();
            g.library.retain(|f| f.source != source);
            for id in removed {
                if !files.iter().any(|f| f.id == id) {
                    self.unregister_if_unused(&g, &id);
                }
            }
        }
        let mut added = 0;
        for f in files {
            if !g.library.iter().any(|x| x.id == f.id) {
                g.library.push(f);
                added += 1;
            }
        }
        g.library.sort_by(|a, b| {
            a.title
                .to_lowercase()
                .cmp(&b.title.to_lowercase())
                .then(a.name.cmp(&b.name))
        });
        drop(g);
        self.refresh_urls();
        added
    }

    pub fn scan_base(self: &Arc<Self>) {
        let (base, deep) = {
            let mut g = self.inner.lock().unwrap();
            if g.scanning {
                return;
            }
            g.scanning = true;
            (g.cfg.base_path.clone(), g.cfg.deep_scan)
        };
        self.mark_dirty();
        let c = self.clone();
        std::thread::spawn(move || {
            let t0 = Instant::now();
            let mut paths = Vec::new();
            if !base.is_empty() {
                collect_pkgs(Path::new(&base), deep, &mut paths, 0);
            }
            let files: Vec<LibFile> = paths
                .iter()
                .filter_map(|p| build_libfile(p, "base"))
                .collect();
            let n = files.len();
            c.insert_files(files, "base", true);
            c.inner.lock().unwrap().scanning = false;
            if !base.is_empty() {
                c.log(
                    "info",
                    format!(
                        "Scanned {base}: {n} PKG file(s) in {} ms",
                        t0.elapsed().as_millis()
                    ),
                );
            }
            c.mark_lib_dirty();
        });
    }

    pub fn add_dragged(self: &Arc<Self>, paths: Vec<PathBuf>) {
        let c = self.clone();
        std::thread::spawn(move || {
            let mut list = Vec::new();
            for p in paths {
                if p.is_dir() {
                    collect_pkgs(&p, true, &mut list, 0);
                } else if is_pkg(&p) {
                    list.push(p);
                }
            }
            if list.is_empty() {
                c.toast("warn", "No .pkg files found in the dropped items");
                return;
            }
            let files: Vec<LibFile> = list
                .iter()
                .filter_map(|p| build_libfile(p, "dragged"))
                .collect();
            {
                let mut g = c.inner.lock().unwrap();
                for f in &files {
                    if !g
                        .cfg
                        .dragged_files
                        .iter()
                        .any(|d| d.eq_ignore_ascii_case(&f.path))
                    {
                        g.cfg.dragged_files.push(f.path.clone());
                    }
                }
            }
            c.save_config();
            let added = c.insert_files(files, "dragged", false);
            c.log("info", format!("Added {added} dragged file(s)"));
            c.toast("ok", format!("+{added} PKG"));
        });
    }

    pub fn load_dragged(self: &Arc<Self>) {
        let list: Vec<PathBuf> = self
            .inner
            .lock()
            .unwrap()
            .cfg
            .dragged_files
            .iter()
            .map(PathBuf::from)
            .collect();
        let files: Vec<LibFile> = list
            .iter()
            .filter_map(|p| build_libfile(p, "dragged"))
            .collect();
        {
            let mut g = self.inner.lock().unwrap();
            let keep: Vec<String> = files.iter().map(|f| f.path.clone()).collect();
            g.cfg.dragged_files = keep;
        }
        self.insert_files(files, "dragged", true);
    }

    pub fn remove_files(&self, ids: &[String]) {
        let mut g = self.inner.lock().unwrap();
        let removed: Vec<LibFile> = g
            .library
            .iter()
            .filter(|f| ids.contains(&f.id) && f.source == "dragged")
            .cloned()
            .collect();
        g.library
            .retain(|f| !(ids.contains(&f.id) && f.source == "dragged"));
        g.cfg
            .dragged_files
            .retain(|p| !removed.iter().any(|r| r.path.eq_ignore_ascii_case(p)));
        for r in &removed {
            self.unregister_if_unused(&g, &r.id);
        }
        drop(g);
        self.save_config();
        self.mark_lib_dirty();
    }

    pub fn icon_data_url(&self, id: &str) -> Option<String> {
        let (path, icon) = {
            let g = self.inner.lock().unwrap();
            let direct = g
                .library
                .iter()
                .find(|f| f.id == id)
                .map(|f| (f.path.clone(), f.icon, f.title_id.clone()))
                .or_else(|| {
                    g.queue
                        .iter()
                        .find(|q| q.file_id == id)
                        .map(|q| (q.path.clone(), q.icon, q.title_id.clone()))
                })?;
            if direct.1.is_some() {
                (direct.0, direct.1)
            } else if !direct.2.is_empty() {
                let tid = &direct.2;
                g.library
                    .iter()
                    .find(|f| f.title_id == *tid && f.icon.is_some())
                    .map(|f| (f.path.clone(), f.icon))
                    .or_else(|| {
                        g.queue
                            .iter()
                            .find(|q| q.title_id == *tid && q.icon.is_some())
                            .map(|q| (q.path.clone(), q.icon))
                    })
                    .unwrap_or((direct.0, direct.1))
            } else {
                (direct.0, direct.1)
            }
        };
        let bytes = pkg::read_icon(Path::new(&path), icon?)?;
        let mime = if bytes.len() >= 4 && &bytes[..3] == b"\xFF\xD8\xFF" {
            "image/jpeg"
        } else if bytes.len() >= 12 && &bytes[..4] == b"RIFF" && &bytes[8..12] == b"WEBP" {
            "image/webp"
        } else {
            "image/png"
        };
        Some(format!(
            "data:{mime};base64,{}",
            crate::http::base64(&bytes)
        ))
    }

    pub fn queue_add(&self, ids: &[String]) -> usize {
        let mut g = self.inner.lock().unwrap();
        let target = g.cfg.ps_app.clone();
        let mut n = 0;
        for id in ids {
            if g.queue.iter().any(|q| {
                &q.file_id == id && !q.finished() && q.status != "failed" && q.status != "stopped"
            }) {
                continue;
            }
            let Some(f) = g.library.iter().find(|f| &f.id == id).cloned() else {
                continue;
            };
            g.queue.push(QueueItem {
                qid: self.next_qid.fetch_add(1, Ordering::Relaxed),
                file_id: f.id,
                path: f.path,
                name: f.name,
                title: f.title,
                title_id: f.title_id,
                content_id: f.content_id,
                kind: f.kind,
                size: f.size,
                has_icon: f.has_icon,
                icon: f.icon,
                target: target.clone(),
                status: "queued".into(),
                progress: 0.0,
                transferred: 0,
                rest_sec: None,
                speed: 0,
                task_id: None,
                error: None,
                added_at: now_ms(),
                started_at: None,
                finished_at: None,
                poll_failures: 0,
                last_bytes: 0,
                last_sample: None,
            });
            n += 1;
        }
        drop(g);
        self.mark_dirty();
        n
    }

    fn console(&self, g: &Inner) -> Console {
        Console {
            ip: g.cfg.ps_ip.trim().to_string(),
            port: if g.cfg.app_port == 0 {
                Target::parse(&g.cfg.ps_app).default_port()
            } else {
                g.cfg.app_port
            },
            target: Target::parse(&g.cfg.ps_app),
            timeout: Duration::from_secs(g.cfg.request_timeout_s.max(2)),
        }
    }

    pub fn console_now(&self) -> Console {
        let g = self.inner.lock().unwrap();
        self.console(&g)
    }

    fn set_item(&self, qid: u64, f: impl FnOnce(&mut QueueItem)) {
        let mut g = self.inner.lock().unwrap();
        if let Some(q) = g.queue.iter_mut().find(|q| q.qid == qid) {
            f(q);
        }
        drop(g);
        self.mark_dirty();
    }

    pub fn start_item(self: &Arc<Self>, qid: u64) {
        let (item, console, url, icon_url, check, server_ok) = {
            let mut g = self.inner.lock().unwrap();
            let cfg = g.cfg.clone();
            let console = self.console(&g);
            let running = g.server.running;
            let Some(q) = g.queue.iter_mut().find(|q| q.qid == qid) else {
                return;
            };
            if q.active() && q.status != "paused" {
                return;
            }
            q.status = "sending".into();
            q.error = None;
            q.progress = 0.0;
            q.transferred = 0;
            q.speed = 0;
            q.rest_sec = None;
            q.task_id = None;
            q.target = cfg.ps_app.clone();
            q.started_at = Some(now_ms());
            q.finished_at = None;
            q.poll_failures = 0;
            q.last_bytes = 0;
            q.last_sample = None;
            let url = format!(
                "http://{}:{}{}",
                cfg.server_ip,
                cfg.server_port,
                server::url_path(&q.file_id, &q.path, &q.name, cfg.url_prefix)
            );
            let icon_url = (cfg.send_icon && q.has_icon).then(|| {
                format!(
                    "http://{}:{}/icon/{}.png",
                    cfg.server_ip, cfg.server_port, q.file_id
                )
            });
            (
                q.clone(),
                console,
                url,
                icon_url,
                cfg.check_installed,
                running,
            )
        };
        self.mark_dirty();
        let fail = |msg: String| {
            self.log("error", format!("{}: {msg}", item.title));
            self.set_item(qid, |q| {
                q.status = "failed".into();
                q.error = Some(msg.clone());
                q.finished_at = Some(now_ms());
            });
        };
        if console.ip.is_empty() {
            return fail("PlayStation IP is not set (Config tab)".into());
        }
        if !server_ok && self.start_server(true).is_err() {
            return fail("local server is not running".into());
        }
        self.server.register(Entry {
            id: item.file_id.clone(),
            path: PathBuf::from(&item.path),
            name: item.name.clone(),
            icon: item.icon,
        });
        self.server.set_blocked(&item.file_id, false);
        self.server.reset_stats(&item.file_id);

        if check
            && console.target == Target::Flatz
            && item.kind == "Game"
            && !item.title_id.is_empty()
        {
            if let Ok(Some(_)) = console.is_exists(&item.title_id) {
                self.log(
                    "warn",
                    format!(
                        "{} ({}) is already installed - skipped",
                        item.title, item.title_id
                    ),
                );
                self.set_item(qid, |q| {
                    q.status = "installed".into();
                    q.progress = 100.0;
                    q.finished_at = Some(now_ms());
                });
                self.inner.lock().unwrap().last_finish = Some(Instant::now());
                return;
            }
        }

        self.log(
            "info",
            format!("Sending {} -> {}:{}", item.title, console.ip, console.port),
        );
        let meta = InstallMeta {
            title: &item.title,
            content_id: &item.content_id,
            icon_url,
        };
        match console.install(&url, &meta) {
            Ok(InstallOutcome::Task(id)) => {
                self.log("ok", format!("{}: install task {id} created", item.title));
                self.set_item(qid, |q| {
                    q.status = "downloading".into();
                    q.task_id = Some(id);
                });
            }
            Ok(InstallOutcome::AlreadyInstalled) => {
                self.log(
                    "warn",
                    format!("{}: console reports it is already installed", item.title),
                );
                self.set_item(qid, |q| {
                    q.status = "installed".into();
                    q.progress = 100.0;
                    q.finished_at = Some(now_ms());
                });
                self.inner.lock().unwrap().last_finish = Some(Instant::now());
            }
            Ok(InstallOutcome::Sent) => {
                self.log(
                    "ok",
                    format!(
                        "{}: accepted by etaHEN, waiting for the console to download",
                        item.title
                    ),
                );
                self.set_item(qid, |q| q.status = "waiting".into());
            }
            Err(e) => fail(e),
        }
    }

    pub fn start_items(self: &Arc<Self>, qids: Vec<u64>) {
        let c = self.clone();
        std::thread::spawn(move || {
            for q in qids {
                c.start_item(q);
            }
        });
    }

    pub fn autostart(self: &Arc<Self>) -> usize {
        let qids: Vec<u64> = {
            let g = self.inner.lock().unwrap();
            let active = g.queue.iter().filter(|q| q.active()).count();
            let free = (g.cfg.max_parallel as usize).saturating_sub(active);
            g.queue
                .iter()
                .filter(|q| q.status == "queued")
                .take(free)
                .map(|q| q.qid)
                .collect()
        };
        let n = qids.len();
        self.start_items(qids);
        n
    }

    pub fn item_action(self: &Arc<Self>, qid: u64, action: &str) -> Result<(), String> {
        let (item, console) = {
            let g = self.inner.lock().unwrap();
            let q = g
                .queue
                .iter()
                .find(|q| q.qid == qid)
                .cloned()
                .ok_or("item not found")?;
            (q, self.console(&g))
        };
        let tgt = Target::parse(&item.target);
        match action {
            "start" => {
                self.start_items(vec![qid]);
                Ok(())
            }
            "pause" | "resume" => {
                let id = item.task_id.ok_or("no console task for this item")?;
                if !tgt.has_progress_api() {
                    return Err("pause is not supported by etaHEN".into());
                }
                let c = Console {
                    target: tgt,
                    ..console
                };
                c.task_action(action, id)?;
                self.set_item(qid, |q| {
                    q.status = if action == "pause" {
                        "paused".into()
                    } else {
                        "downloading".into()
                    }
                });
                self.log("info", format!("{}: {action}d", item.title));
                Ok(())
            }
            "stop" => {
                if let (Some(id), true) = (item.task_id, tgt.has_progress_api()) {
                    let c = Console {
                        target: tgt,
                        ..console
                    };
                    if let Err(e) = c.task_action("stop", id) {
                        self.log(
                            "warn",
                            format!("{}: stop on console failed: {e}", item.title),
                        );
                    }
                }
                // Cut the transfer on our side as well (the only way for etaHEN).
                self.server.set_blocked(&item.file_id, true);
                self.set_item(qid, |q| {
                    q.status = "stopped".into();
                    q.speed = 0;
                    q.rest_sec = None;
                    q.finished_at = Some(now_ms());
                });
                self.log("warn", format!("{}: stopped", item.title));
                Ok(())
            }
            "requeue" => {
                self.server.set_blocked(&item.file_id, false);
                self.set_item(qid, |q| {
                    q.status = "queued".into();
                    q.progress = 0.0;
                    q.error = None;
                    q.task_id = None;
                    q.speed = 0;
                    q.rest_sec = None;
                    q.transferred = 0;
                });
                Ok(())
            }
            "up" | "down" | "top" => {
                let mut g = self.inner.lock().unwrap();
                let i = g
                    .queue
                    .iter()
                    .position(|q| q.qid == qid)
                    .ok_or("item not found")?;
                match action {
                    "up" if i > 0 => g.queue.swap(i, i - 1),
                    "down" if i + 1 < g.queue.len() => g.queue.swap(i, i + 1),
                    "top" => {
                        let it = g.queue.remove(i);
                        g.queue.insert(0, it);
                    }
                    _ => {}
                }
                drop(g);
                self.mark_dirty();
                Ok(())
            }
            _ => Err("unknown action".into()),
        }
    }

    pub fn queue_remove(&self, qids: &[u64]) {
        let mut g = self.inner.lock().unwrap();
        let removed: Vec<String> = g
            .queue
            .iter()
            .filter(|q| qids.contains(&q.qid))
            .map(|q| q.file_id.clone())
            .collect();
        g.queue.retain(|q| !qids.contains(&q.qid));
        for id in removed {
            self.unregister_if_unused(&g, &id);
        }
        drop(g);
        self.mark_dirty();
    }

    pub fn queue_reset(&self, qids: &[u64]) {
        let mut g = self.inner.lock().unwrap();
        for q in g
            .queue
            .iter_mut()
            .filter(|q| qids.contains(&q.qid) && !matches!(q.status.as_str(), "sending"))
        {
            q.status = "queued".into();
            q.progress = 0.0;
            q.error = None;
            q.task_id = None;
            q.speed = 0;
            q.rest_sec = None;
            q.transferred = 0;
            q.started_at = None;
            q.finished_at = None;
            self.server.set_blocked(&q.file_id, false);
        }
        drop(g);
        self.mark_dirty();
    }

    pub fn spawn_workers(self: &Arc<Self>) {
        let c = self.clone();
        std::thread::Builder::new()
            .name("queue-scanner".into())
            .spawn(move || loop {
                let iv = c.inner.lock().unwrap().cfg.poll_interval_ms.max(500);
                std::thread::sleep(Duration::from_millis(iv));
                if c.shutting_down.load(Ordering::Relaxed) {
                    break;
                }
                c.tick();
            })
            .ok();

        let c = self.clone();
        std::thread::Builder::new()
            .name("snapshot-pump".into())
            .spawn(move || {
                let mut last_total = c.server.shared.total_sent.load(Ordering::Relaxed);
                let mut last_t = Instant::now();
                let mut speed_ema = 0f64;
                let mut tick = 0u32;
                loop {
                    std::thread::sleep(Duration::from_millis(250));
                    if c.shutting_down.load(Ordering::Relaxed) {
                        break;
                    }
                    tick += 1;
                    if tick.is_multiple_of(4) {
                        let total = c.server.shared.total_sent.load(Ordering::Relaxed);
                        let dt = last_t.elapsed().as_secs_f64().max(0.001);
                        let inst = (total - last_total) as f64 / dt;
                        speed_ema = if speed_ema == 0.0 {
                            inst
                        } else {
                            speed_ema * 0.6 + inst * 0.4
                        };
                        if inst == 0.0 && speed_ema < 1024.0 {
                            speed_ema = 0.0;
                        }
                        last_total = total;
                        last_t = Instant::now();
                        let conns = c.server.connections();
                        let mut g = c.inner.lock().unwrap();
                        let changed = g.server.connections != conns
                            || g.server.speed != speed_ema as u64
                            || g.server.total_sent != total;
                        g.server.connections = conns;
                        g.server.speed = speed_ema as u64;
                        g.server.total_sent = total;
                        drop(g);
                        if changed {
                            c.mark_dirty();
                        }
                    }
                    if c.dirty.swap(false, Ordering::AcqRel) {
                        c.emit_snapshot();
                    }
                }
            })
            .ok();
    }

    pub fn snapshot(&self, with_lib: bool) -> Snapshot {
        let g = self.inner.lock().unwrap();
        Snapshot {
            cfg: g.cfg.clone(),
            server: g.server.clone(),
            queue: g.queue.clone(),
            library: with_lib.then(|| g.library.clone()),
            scanning: g.scanning,
        }
    }

    fn emit_snapshot(&self) {
        let with_lib = self.lib_dirty.swap(false, Ordering::AcqRel);
        let snap = self.snapshot(with_lib);
        if let Some(app) = self.app.get() {
            let _ = app.emit("snapshot", &snap);
        }
    }

    fn tick(self: &Arc<Self>) {
        let (console, polls, eta_items) = {
            let g = self.inner.lock().unwrap();
            let console = self.console(&g);
            let polls: Vec<(u64, i64, String)> = g
                .queue
                .iter()
                .filter(|q| {
                    q.status == "downloading" && Target::parse(&q.target).has_progress_api()
                })
                .filter_map(|q| q.task_id.map(|t| (q.qid, t, q.target.clone())))
                .collect();
            let eta: Vec<(u64, String, u64)> = g
                .queue
                .iter()
                .filter(|q| {
                    matches!(q.status.as_str(), "waiting" | "downloading")
                        && !Target::parse(&q.target).has_progress_api()
                })
                .map(|q| (q.qid, q.file_id.clone(), q.size))
                .collect();
            (console, polls, eta)
        };

        for (qid, task, tgt) in polls {
            let c = Console {
                target: Target::parse(&tgt),
                ip: console.ip.clone(),
                port: console.port,
                timeout: console.timeout,
            };
            let res = c.progress(task);
            let mut done_msg = None;
            {
                let mut g = self.inner.lock().unwrap();
                let Some(q) = g.queue.iter_mut().find(|q| q.qid == qid) else {
                    continue;
                };
                if q.status != "downloading" {
                    continue;
                }
                match res {
                    Ok(p) => {
                        q.poll_failures = 0;
                        let total = if p.length_total > 0 {
                            p.length_total
                        } else {
                            q.size
                        };
                        let now = Instant::now();
                        if let Some(t) = q.last_sample {
                            let dt = now.duration_since(t).as_secs_f64().max(0.001);
                            let inst = p.transferred_total.saturating_sub(q.last_bytes) as f64 / dt;
                            q.speed = if q.speed == 0 {
                                inst as u64
                            } else {
                                (q.speed as f64 * 0.5 + inst * 0.5) as u64
                            };
                        }
                        q.last_sample = Some(now);
                        q.last_bytes = p.transferred_total;
                        q.transferred = p.transferred_total;
                        q.progress = if total > 0 {
                            (p.transferred_total as f64 / total as f64 * 100.0).min(100.0)
                        } else {
                            0.0
                        };
                        q.rest_sec = (p.rest_sec_total > 0).then_some(p.rest_sec_total);
                        if p.error != 0 {
                            q.status = "failed".into();
                            q.error = Some(crate::console::describe_error_code(p.error as u32));
                            q.finished_at = Some(now_ms());
                            q.speed = 0;
                            done_msg = Some((
                                "error",
                                format!(
                                    "{}: download failed ({})",
                                    q.title,
                                    q.error.clone().unwrap_or_default()
                                ),
                            ));
                        } else if total > 0 && p.transferred_total >= total {
                            q.status = "completed".into();
                            q.progress = 100.0;
                            q.rest_sec = None;
                            q.speed = 0;
                            q.finished_at = Some(now_ms());
                            done_msg = Some((
                                "ok",
                                format!(
                                    "{}: download finished, the console is installing it",
                                    q.title
                                ),
                            ));
                        }
                    }
                    Err(e) => {
                        q.poll_failures += 1;
                        if q.poll_failures >= 8 {
                            q.status = "failed".into();
                            q.error = Some(format!("lost contact with task: {e}"));
                            q.finished_at = Some(now_ms());
                            q.speed = 0;
                            done_msg = Some(("error", format!("{}: {e}", q.title)));
                        }
                    }
                }
                if done_msg.is_some() {
                    g.last_finish = Some(Instant::now());
                }
            }
            if let Some((lvl, m)) = done_msg {
                self.log(lvl, m.clone());
                self.toast(lvl, m);
            }
            self.mark_dirty();
        }

        for (qid, fid, size) in eta_items {
            let st = self.server.stats(&fid);
            let covered = st.covered();
            let mut done_msg = None;
            {
                let mut g = self.inner.lock().unwrap();
                let Some(q) = g.queue.iter_mut().find(|q| q.qid == qid) else {
                    continue;
                };
                let now = Instant::now();
                if let Some(t) = q.last_sample {
                    let dt = now.duration_since(t).as_secs_f64().max(0.001);
                    let inst = st.bytes_sent.saturating_sub(q.last_bytes) as f64 / dt;
                    q.speed = if q.speed == 0 {
                        inst as u64
                    } else {
                        (q.speed as f64 * 0.5 + inst * 0.5) as u64
                    };
                    if inst == 0.0 && st.active == 0 {
                        q.speed = 0;
                    }
                }
                q.last_sample = Some(now);
                q.last_bytes = st.bytes_sent;
                q.transferred = covered;
                q.progress = if size > 0 {
                    (covered as f64 / size as f64 * 100.0).min(100.0)
                } else {
                    0.0
                };
                if covered > 0 && q.status == "waiting" {
                    q.status = "downloading".into();
                }
                // The console never asked for the file: wrong server IP, firewall, ...
                if q.status == "waiting"
                    && st.last_activity.is_none()
                    && now_ms().saturating_sub(q.started_at.unwrap_or(0)) > 180_000
                {
                    q.status = "failed".into();
                    q.error = Some(
                        "console never requested the file (check Server IP / Windows Firewall)"
                            .into(),
                    );
                    q.finished_at = Some(now_ms());
                    g.last_finish = Some(Instant::now());
                    drop(g);
                    self.mark_dirty();
                    continue;
                }
                q.rest_sec = (q.speed > 0).then(|| size.saturating_sub(covered) / q.speed.max(1));
                if size > 0 && covered >= size {
                    q.status = "completed".into();
                    q.progress = 100.0;
                    q.speed = 0;
                    q.rest_sec = None;
                    q.finished_at = Some(now_ms());
                    done_msg = Some(format!(
                        "{}: transfer finished, PS5 is installing it",
                        q.title
                    ));
                }
                if done_msg.is_some() {
                    g.last_finish = Some(Instant::now());
                }
            }
            if let Some(m) = done_msg {
                self.log("ok", m.clone());
                self.toast("ok", m);
            }
            self.mark_dirty();
        }

        let next = {
            let g = self.inner.lock().unwrap();
            if !g.cfg.queue_scanner {
                None
            } else {
                let active = g.queue.iter().filter(|q| q.active()).count();
                let cooled = g
                    .last_finish
                    .map(|t| t.elapsed() >= Duration::from_secs(g.cfg.delay_between_s))
                    .unwrap_or(true);
                if active < g.cfg.max_parallel as usize && cooled {
                    g.queue.iter().find(|q| q.status == "queued").map(|q| q.qid)
                } else {
                    None
                }
            }
        };
        if let Some(qid) = next {
            self.start_item(qid);
        }
    }

    pub fn shutdown(&self) {
        if self.shutting_down.swap(true, Ordering::SeqCst) {
            return;
        }
        self.save_config();
        self.server.stop();
    }
}

#[cfg(test)]
mod e2e {
    use super::*;

    fn wait(core: &Arc<Core>, what: &str, secs: u64, f: impl Fn(&Inner) -> bool) {
        let t0 = Instant::now();
        loop {
            {
                let g = core.inner.lock().unwrap();
                if f(&g) {
                    return;
                }
                if t0.elapsed().as_secs() > secs {
                    panic!(
                        "timeout waiting for {what}; queue: {:?}",
                        g.queue
                            .iter()
                            .map(|q| (&q.status, q.progress, &q.error))
                            .collect::<Vec<_>>()
                    );
                }
            }
            std::thread::sleep(Duration::from_millis(200));
        }
    }

    /// Needs `python tools/mock_console.py 12855` running.
    #[test]
    #[ignore]
    fn end_to_end_flatz_etahen_and_port_release() {
        let dir = std::env::temp_dir().join("pkgsender-e2e");
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join("sub")).unwrap();
        let src = Path::new(env!("CARGO_MANIFEST_DIR")).join("../ps4hub.pkg");
        std::fs::copy(&src, dir.join("sub").join("Test Game [v1.00].pkg")).unwrap();

        let core = Core::new(dir.join("cfg.json"));
        {
            let mut g = core.inner.lock().unwrap();
            g.cfg.base_path = dir.to_string_lossy().to_string();
            g.cfg.deep_scan = true;
            g.cfg.server_ip = "127.0.0.1".into();
            g.cfg.server_port = 18339;
            g.cfg.ps_ip = "127.0.0.1".into();
            g.cfg.app_port = 12855;
            g.cfg.ps_app = "flatz".into();
            g.cfg.poll_interval_ms = 500;
            g.cfg.delay_between_s = 0;
            g.cfg.queue_scanner = true;
        }
        core.start_server(true).unwrap();
        core.scan_base();
        std::thread::sleep(Duration::from_millis(100));
        wait(&core, "scan", 10, |g| !g.scanning && !g.library.is_empty());
        let f = core.inner.lock().unwrap().library[0].clone();
        eprintln!(
            "PKG: title={:?} tid={} cid={} cat={} ver={} icon={} url={}",
            f.title, f.title_id, f.content_id, f.category, f.app_ver, f.has_icon, f.url
        );
        assert!(!f.title_id.is_empty());
        assert!(
            core.icon_data_url(&f.id)
                .map(|u| u.starts_with("data:image/png;base64,"))
                .unwrap_or(false)
                || !f.has_icon
        );

        core.spawn_workers();
        assert_eq!(core.queue_add(std::slice::from_ref(&f.id)), 1);
        wait(&core, "flatz complete", 60, |g| {
            g.queue[0].status == "completed"
        });
        assert!(core.server.stats(&f.id).covered() >= f.size);

        core.inner.lock().unwrap().cfg.ps_app = "etahen_v2".into();
        let qid = core.inner.lock().unwrap().queue[0].qid;
        core.queue_reset(&[qid]);
        wait(&core, "etahen complete", 60, |g| {
            g.queue[0].status == "completed" && g.queue[0].target == "etahen_v2"
        });

        core.stop_server();
        std::net::TcpListener::bind(("0.0.0.0", 18339))
            .expect("port must be released right after stop");
        core.shutdown();
    }
}
