//! Console-side installer protocols.
//!
//! * PS4 flatZ Remote Package Installer (and PS4Hub, which is wire
//!   compatible): JSON over HTTP on port 12800, `/api/install`,
//!   `/api/get_task_progress`, `/api/*_task`, `/api/is_exists`, ...
//!   Numbers in responses are bare hex literals (handled by `relax_json`).
//! * PS5 etaHEN Direct Package Installer:
//!   - v2: HTTP on 12800, `POST /upload` form with `url`, `content_name`,
//!     `content_id`, `icon_url`; plain-text "SUCCESS: ..." / "FAILED: ...".
//!   - v1: raw TCP on 9090, one JSON object in, `{"res":"<code>"}` out.
//!
//! etaHEN has no progress API; progress is measured by the local server.

use serde::Serialize;
use serde_json::{json, Value};
use std::io::{Read, Write};
use std::time::Duration;

use crate::http::{self, parse_json};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Target {
    Flatz,
    EtaV2,
    EtaV1,
}

impl Target {
    pub fn parse(s: &str) -> Self {
        match s {
            "etahen_v2" => Target::EtaV2,
            "etahen_v1" => Target::EtaV1,
            _ => Target::Flatz,
        }
    }
    pub fn default_port(self) -> u16 {
        match self {
            Target::EtaV1 => 9090,
            _ => 12800,
        }
    }
    pub fn has_progress_api(self) -> bool {
        self == Target::Flatz
    }
}

pub struct Console {
    pub ip: String,
    pub port: u16,
    pub target: Target,
    pub timeout: Duration,
}

pub struct InstallMeta<'a> {
    pub title: &'a str,
    pub content_id: &'a str,
    pub icon_url: Option<String>,
}

pub enum InstallOutcome {
    Task(i64),
    AlreadyInstalled,
    Sent,
}

#[derive(Default, Debug, Clone)]
pub struct Progress {
    pub length_total: u64,
    pub transferred_total: u64,
    pub rest_sec_total: u64,
    pub error: i64,
}

fn num(v: &Value, k: &str) -> Option<i64> {
    match &v[k] {
        Value::Number(n) => n.as_i64().or_else(|| n.as_u64().map(|u| u as i64)),
        Value::String(s) => {
            let s = s.trim();
            if let Some(h) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
                i64::from_str_radix(h, 16).ok()
            } else {
                s.parse().ok()
            }
        }
        _ => None,
    }
}

pub fn describe_error_code(code: u32) -> String {
    format!("error 0x{code:08X}")
}

fn flatz_fail(v: &Value) -> String {
    if let Some(c) = num(v, "error_code") {
        return describe_error_code(c as u32);
    }
    if let Some(e) = v["error"].as_str() {
        return e.to_string();
    }
    format!("console returned: {v}")
}

impl Console {
    fn flatz_post(&self, path: &str, body: Value) -> Result<Value, String> {
        let payload = body.to_string();
        let r = http::request(
            &self.ip,
            self.port,
            "POST",
            path,
            Some("application/json"),
            payload.as_bytes(),
            self.timeout,
        )?;
        let v = parse_json(&r.body)?;
        if v["status"].as_str() == Some("success") {
            Ok(v)
        } else {
            Err(flatz_fail(&v))
        }
    }

    pub fn install(&self, url: &str, meta: &InstallMeta) -> Result<InstallOutcome, String> {
        match self.target {
            Target::Flatz => {
                let v = self.flatz_post(
                    "/api/install",
                    json!({ "type": "direct", "packages": [url] }),
                )?;
                match num(&v, "task_id") {
                    Some(id) if id >= 0 => Ok(InstallOutcome::Task(id)),
                    _ => Ok(InstallOutcome::AlreadyInstalled),
                }
            }
            Target::EtaV2 => self.eta_v2_install(url, meta),
            Target::EtaV1 => self.eta_v1_install(url, meta),
        }
    }

    fn eta_v2_install(&self, url: &str, meta: &InstallMeta) -> Result<InstallOutcome, String> {
        let boundary = format!(
            "----PkgSender{:016x}",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_nanos() as u64)
                .unwrap_or(7)
        );
        let mut body = String::new();
        let mut field = |k: &str, v: &str| {
            if !v.is_empty() {
                body.push_str(&format!(
                    "--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n"
                ));
            }
        };
        field("url", url);
        field("content_name", meta.title);
        field("content_id", meta.content_id);
        field("icon_url", meta.icon_url.as_deref().unwrap_or(""));
        body.push_str(&format!("--{boundary}--\r\n"));
        let ct = format!("multipart/form-data; boundary={boundary}");
        // Registering the task can take a while on the PS5 - give it more time.
        let r = http::request(
            &self.ip,
            self.port,
            "POST",
            "/upload",
            Some(&ct),
            body.as_bytes(),
            self.timeout.max(Duration::from_secs(20)),
        )?;
        let text = r.body.trim();
        if text.starts_with("SUCCESS") {
            Ok(InstallOutcome::Sent)
        } else if text.starts_with("FAILED") {
            Err(text
                .trim_start_matches("FAILED:")
                .trim()
                .chars()
                .take(200)
                .collect())
        } else if r.status == 200 {
            Ok(InstallOutcome::Sent)
        } else {
            Err(format!(
                "HTTP {}: {}",
                r.status,
                text.chars().take(160).collect::<String>()
            ))
        }
    }

    fn eta_v1_install(&self, url: &str, meta: &InstallMeta) -> Result<InstallOutcome, String> {
        // etaHEN reads a single 1024-byte buffer - drop optional fields if too long.
        let mut v =
            json!({ "url": url, "content_name": meta.title, "content_id": meta.content_id });
        if let Some(i) = &meta.icon_url {
            v["icon_url"] = json!(i);
        }
        let mut payload = v.to_string();
        if payload.len() > 1000 {
            payload = json!({ "url": url, "content_id": meta.content_id }).to_string();
        }
        if payload.len() > 1000 {
            return Err("package URL too long for etaHEN DPI v1 (max ~1000 bytes)".into());
        }
        let mut s = http::connect(&self.ip, self.port, self.timeout)?;
        let _ = s.set_read_timeout(Some(self.timeout.max(Duration::from_secs(20))));
        s.write_all(payload.as_bytes()).map_err(|e| e.to_string())?;
        let mut buf = vec![0u8; 512];
        let n = s
            .read(&mut buf)
            .map_err(|e| format!("no reply from etaHEN: {e}"))?;
        let v = parse_json(&String::from_utf8_lossy(&buf[..n]))?;
        let res = num(&v, "res").unwrap_or(-1);
        if res == 0 {
            Ok(InstallOutcome::Sent)
        } else {
            Err(describe_error_code(res as u32))
        }
    }

    pub fn progress(&self, task_id: i64) -> Result<Progress, String> {
        let v = self.flatz_post("/api/get_task_progress", json!({ "task_id": task_id }))?;
        Ok(Progress {
            length_total: num(&v, "length_total")
                .or_else(|| num(&v, "length"))
                .unwrap_or(0) as u64,
            transferred_total: num(&v, "transferred_total")
                .or_else(|| num(&v, "transferred"))
                .unwrap_or(0) as u64,
            rest_sec_total: num(&v, "rest_sec_total")
                .or_else(|| num(&v, "rest_sec"))
                .unwrap_or(0) as u64,
            error: num(&v, "error").unwrap_or(0),
        })
    }

    pub fn task_action(&self, action: &str, task_id: i64) -> Result<(), String> {
        if self.target != Target::Flatz {
            return Err("not supported by etaHEN".into());
        }
        let path = match action {
            "pause" => "/api/pause_task",
            "resume" => "/api/resume_task",
            "stop" => "/api/stop_task",
            "start" => "/api/start_task",
            "unregister" => "/api/unregister_task",
            _ => return Err("unknown action".into()),
        };
        self.flatz_post(path, json!({ "task_id": task_id }))
            .map(|_| ())
    }

    pub fn is_exists(&self, title_id: &str) -> Result<Option<u64>, String> {
        if self.target != Target::Flatz {
            return Err("not supported by etaHEN".into());
        }
        let v = self.flatz_post("/api/is_exists", json!({ "title_id": title_id }))?;
        let exists = match &v["exists"] {
            Value::Bool(b) => *b,
            Value::String(s) => s == "true",
            _ => false,
        };
        Ok(exists.then(|| num(&v, "size").unwrap_or(0) as u64))
    }

    pub fn uninstall(&self, kind: &str, id: &str) -> Result<(), String> {
        if self.target != Target::Flatz {
            return Err("not supported by etaHEN".into());
        }
        let (path, key) = match kind {
            "game" => ("/api/uninstall_game", "title_id"),
            "patch" => ("/api/uninstall_patch", "title_id"),
            "dlc" => ("/api/uninstall_ac", "content_id"),
            "theme" => ("/api/uninstall_theme", "content_id"),
            _ => return Err("unknown kind".into()),
        };
        self.flatz_post(path, json!({ key: id })).map(|_| ())
    }

    pub fn test(&self) -> Result<String, String> {
        match self.target {
            Target::Flatz => {
                let r = http::request(
                    &self.ip,
                    self.port,
                    "POST",
                    "/api/is_exists",
                    Some("application/json"),
                    br#"{"title_id":"CUSA00000"}"#,
                    self.timeout,
                )?;
                let v = parse_json(&r.body)?;
                if v.get("status").is_some() {
                    Ok("Remote Package Installer is responding".into())
                } else {
                    Err("port is open but the response is not from flatZ RPI".into())
                }
            }
            Target::EtaV2 => {
                let r = http::request(&self.ip, self.port, "GET", "/", None, b"", self.timeout)?;
                if r.status == 200 {
                    Ok(
                        if r.body.contains("etaHEN") || r.body.contains("uploadForm") {
                            "etaHEN Direct Package Installer v2 is responding".into()
                        } else {
                            "HTTP server is responding on the port".into()
                        },
                    )
                } else {
                    Err(format!("HTTP {}", r.status))
                }
            }
            Target::EtaV1 => {
                // Connecting is enough; sending nothing would pop an error
                // notification on the PS5, so close right away.
                http::connect(&self.ip, self.port, self.timeout)?;
                Ok("etaHEN DPI v1 port 9090 is open".into())
            }
        }
    }
}

#[derive(Serialize, Clone, Debug)]
pub struct Found {
    pub ip: String,
    pub app: String,
    pub label: String,
}

fn port_open(ip: &str, port: u16, ms: u64) -> bool {
    http::connect(ip, port, Duration::from_millis(ms)).is_ok()
}

pub fn scan_network(base_ip: &str) -> Vec<Found> {
    let parts: Vec<&str> = base_ip.split('.').collect();
    if parts.len() != 4 {
        return vec![];
    }
    let prefix = format!("{}.{}.{}.", parts[0], parts[1], parts[2]);
    let hosts: Vec<String> = (1..=254)
        .map(|i| format!("{prefix}{i}"))
        .filter(|h| h != base_ip)
        .collect();
    let found = std::sync::Mutex::new(Vec::new());
    std::thread::scope(|sc| {
        for chunk in hosts.chunks(16) {
            let found = &found;
            sc.spawn(move || {
                for ip in chunk {
                    let p12800 = port_open(ip, 12800, 300);
                    let p9090 = !p12800 && port_open(ip, 9090, 200);
                    if !p12800 && !p9090 {
                        continue;
                    }
                    let mut f = Found {
                        ip: ip.clone(),
                        app: "flatz".into(),
                        label: "PS4 RPI (flatZ)".into(),
                    };
                    if p12800 {
                        let c = Console {
                            ip: ip.clone(),
                            port: 12800,
                            target: Target::EtaV2,
                            timeout: Duration::from_millis(1500),
                        };
                        let eta = http::request(&c.ip, c.port, "GET", "/", None, b"", c.timeout)
                            .map(|r| r.body.contains("etaHEN") || r.body.contains("uploadForm"))
                            .unwrap_or(false);
                        if eta {
                            f.app = "etahen_v2".into();
                            f.label = "PS5 etaHEN DPI v2".into();
                        }
                    } else {
                        f.app = "etahen_v1".into();
                        f.label = "PS5 etaHEN DPI v1".into();
                    }
                    found.lock().unwrap().push(f);
                }
            });
        }
    });
    let mut v = found.into_inner().unwrap();
    v.sort_by_key(|f| {
        f.ip.rsplit('.')
            .next()
            .and_then(|x| x.parse::<u32>().ok())
            .unwrap_or(0)
    });
    v
}
