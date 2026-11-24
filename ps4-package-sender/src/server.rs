//! Built-in PKG file server.
//!
//! Runs inside the app process on plain `std::net` threads - there is no
//! child process (unlike Electron + express), so closing the app always
//! releases the port. Rust std creates sockets with
//! WSA_FLAG_NO_HANDLE_INHERIT, so anything we spawn (browser, explorer)
//! can never inherit and keep the listening socket alive. SO_REUSEADDR is
//! deliberately NOT set: on Windows it would let two processes bind the same
//! port at once, which is the classic "port taken by a ghost" situation.

use std::collections::{HashMap, HashSet};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};
use std::net::{Ipv4Addr, Shutdown, SocketAddr, TcpListener, TcpStream};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, RwLock};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crate::http::{pct_decode, pct_encode};

#[derive(Clone, Debug)]
pub struct Entry {
    pub id: String,
    pub path: PathBuf,
    pub name: String,
    pub icon: Option<(u64, u32)>,
}

#[derive(Default, Clone)]
pub struct FileStats {
    ranges: Vec<(u64, u64)>,
    pub bytes_sent: u64,
    pub last_activity: Option<Instant>,
    pub active: u32,
}

impl FileStats {
    fn add(&mut self, s: u64, e: u64) {
        if e <= s {
            return;
        }
        self.ranges.push((s, e));
        self.ranges.sort_unstable();
        let mut merged: Vec<(u64, u64)> = Vec::with_capacity(self.ranges.len());
        for &(a, b) in &self.ranges {
            match merged.last_mut() {
                Some(last) if a <= last.1 => last.1 = last.1.max(b),
                _ => merged.push((a, b)),
            }
        }
        self.ranges = merged;
    }
    pub fn covered(&self) -> u64 {
        self.ranges.iter().map(|(a, b)| b - a).sum()
    }
}

pub type LogFn = Arc<dyn Fn(String) + Send + Sync>;

pub struct Shared {
    files: RwLock<HashMap<String, Entry>>,
    by_path: RwLock<HashMap<String, String>>,
    stats: Mutex<HashMap<String, FileStats>>,
    blocked: RwLock<HashSet<String>>,
    conns: Mutex<HashMap<u64, TcpStream>>,
    next_conn: AtomicU64,
    running: AtomicBool,
    pub total_sent: AtomicU64,
    log: LogFn,
}

pub enum StartError {
    InUse,
    Denied(String),
    Other(String),
}

pub struct HttpServer {
    pub shared: Arc<Shared>,
    run: Mutex<Option<(u16, JoinHandle<()>)>>,
}

const MAX_CONNS: usize = 96;
const CHUNK: usize = 512 * 1024;

fn path_key(p: &str) -> String {
    p.replace('\\', "/").to_lowercase()
}

/// URL path for a file, either short (`/<id>/<name>`) or prefixed with the
/// full local path (`/f/C:/Games/x.pkg`), which makes every URL unique.
pub fn url_path(id: &str, path: &str, name: &str, full_prefix: bool) -> String {
    if full_prefix {
        let p = path.replace('\\', "/");
        let enc: Vec<String> = p.split('/').map(pct_encode).collect();
        format!("/f/{}", enc.join("/"))
    } else {
        format!("/{id}/{}", pct_encode(name))
    }
}

impl HttpServer {
    pub fn new(log: LogFn) -> Self {
        Self {
            shared: Arc::new(Shared {
                files: RwLock::new(HashMap::new()),
                by_path: RwLock::new(HashMap::new()),
                stats: Mutex::new(HashMap::new()),
                blocked: RwLock::new(HashSet::new()),
                conns: Mutex::new(HashMap::new()),
                next_conn: AtomicU64::new(1),
                running: AtomicBool::new(false),
                total_sent: AtomicU64::new(0),
                log,
            }),
            run: Mutex::new(None),
        }
    }

    pub fn register(&self, e: Entry) {
        self.shared
            .by_path
            .write()
            .unwrap()
            .insert(path_key(&e.path.to_string_lossy()), e.id.clone());
        self.shared.files.write().unwrap().insert(e.id.clone(), e);
    }

    pub fn unregister(&self, id: &str) {
        if let Some(e) = self.shared.files.write().unwrap().remove(id) {
            self.shared
                .by_path
                .write()
                .unwrap()
                .remove(&path_key(&e.path.to_string_lossy()));
        }
    }

    pub fn set_blocked(&self, id: &str, blocked: bool) {
        let mut b = self.shared.blocked.write().unwrap();
        if blocked {
            b.insert(id.to_string());
        } else {
            b.remove(id);
        }
        // Transfers already running for this file notice the blocked set
        // between chunks and abort by themselves.
    }

    pub fn stats(&self, id: &str) -> FileStats {
        self.shared
            .stats
            .lock()
            .unwrap()
            .get(id)
            .cloned()
            .unwrap_or_default()
    }

    pub fn reset_stats(&self, id: &str) {
        self.shared.stats.lock().unwrap().remove(id);
    }

    pub fn connections(&self) -> usize {
        self.shared.conns.lock().unwrap().len()
    }

    pub fn is_running(&self) -> bool {
        self.run.lock().unwrap().is_some()
    }

    pub fn start(&self, port: u16) -> Result<(), StartError> {
        let mut run = self.run.lock().unwrap();
        if let Some((p, _)) = run.as_ref() {
            if *p == port {
                return Ok(());
            }
        }
        if let Some((_, h)) = run.take() {
            self.shutdown_inner(h);
        }
        let listener = bind_exclusive(port).map_err(|e| match e.kind() {
            std::io::ErrorKind::AddrInUse => StartError::InUse,
            std::io::ErrorKind::PermissionDenied => StartError::Denied(format!(
                "port {port} is blocked by Windows (reserved/excluded port range or firewall policy) - pick another port"
            )),
            _ => StartError::Other(e.to_string()),
        })?;
        listener
            .set_nonblocking(true)
            .map_err(|e| StartError::Other(e.to_string()))?;
        self.shared.running.store(true, Ordering::SeqCst);
        let shared = self.shared.clone();
        let h = std::thread::Builder::new()
            .name("pkg-server-accept".into())
            .spawn(move || accept_loop(shared, listener))
            .map_err(|e| StartError::Other(e.to_string()))?;
        *run = Some((port, h));
        Ok(())
    }

    pub fn stop(&self) {
        let mut run = self.run.lock().unwrap();
        if let Some((_, h)) = run.take() {
            self.shutdown_inner(h);
        }
    }

    fn shutdown_inner(&self, h: JoinHandle<()>) {
        self.shared.running.store(false, Ordering::SeqCst);
        for (_, c) in self.shared.conns.lock().unwrap().drain() {
            // linger(0) => RST instead of FIN: no TIME_WAIT left behind on our side.
            let _ = socket2::SockRef::from(&c).set_linger(Some(Duration::ZERO));
            let _ = c.shutdown(Shutdown::Both);
        }
        let _ = h.join(); // accept loop polls every 40 ms; listener is dropped on exit
    }
}

/// Bind with SO_EXCLUSIVEADDRUSE. Without it Windows lets another process
/// that used SO_REUSEADDR (e.g. a leftover Node/Electron sender) listen on
/// the same port at the same time, and connections from the console land in
/// either process at random. With it, a taken port is reported as taken and
/// nobody can hijack ours.
fn bind_exclusive(port: u16) -> std::io::Result<TcpListener> {
    use socket2::{Domain, Protocol, Socket, Type};
    use std::os::windows::io::AsRawSocket;
    let sock = Socket::new(Domain::IPV4, Type::STREAM, Some(Protocol::TCP))?;
    let one: i32 = 1;
    let rc = unsafe {
        windows_sys::Win32::Networking::WinSock::setsockopt(
            sock.as_raw_socket() as usize,
            windows_sys::Win32::Networking::WinSock::SOL_SOCKET,
            windows_sys::Win32::Networking::WinSock::SO_EXCLUSIVEADDRUSE,
            &one as *const i32 as *const u8,
            4,
        )
    };
    if rc != 0 {
        return Err(std::io::Error::last_os_error());
    }
    sock.bind(&SocketAddr::from((Ipv4Addr::UNSPECIFIED, port)).into())?;
    sock.listen(128)?;
    Ok(sock.into())
}

fn accept_loop(shared: Arc<Shared>, listener: TcpListener) {
    while shared.running.load(Ordering::SeqCst) {
        match listener.accept() {
            Ok((stream, peer)) => {
                let _ = stream.set_nonblocking(false);
                if shared.conns.lock().unwrap().len() >= MAX_CONNS {
                    let mut s = stream;
                    let _ = s.write_all(b"HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    continue;
                }
                let cid = shared.next_conn.fetch_add(1, Ordering::Relaxed);
                if let Ok(c) = stream.try_clone() {
                    shared.conns.lock().unwrap().insert(cid, c);
                }
                let sh = shared.clone();
                let _ = std::thread::Builder::new()
                    .name("pkg-server-conn".into())
                    .spawn(move || {
                        handle_conn(&sh, stream, peer);
                        sh.conns.lock().unwrap().remove(&cid);
                    });
            }
            Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                std::thread::sleep(Duration::from_millis(40))
            }
            Err(_) => std::thread::sleep(Duration::from_millis(40)),
        }
    }
    drop(listener);
}

struct Req {
    method: String,
    path: String,
    range: Option<String>,
    keep_alive: bool,
}

fn find(hay: &[u8], needle: &[u8]) -> Option<usize> {
    hay.windows(needle.len()).position(|w| w == needle)
}

fn read_request(s: &mut TcpStream, buf: &mut Vec<u8>) -> Option<Req> {
    let mut tmp = [0u8; 4096];
    let he = loop {
        if let Some(p) = find(buf, b"\r\n\r\n") {
            break p;
        }
        if buf.len() > 32 * 1024 {
            return None;
        }
        let n = s.read(&mut tmp).ok()?;
        if n == 0 {
            return None;
        }
        buf.extend_from_slice(&tmp[..n]);
    };
    let head = String::from_utf8_lossy(&buf[..he]).to_string();
    buf.drain(..he + 4);
    let mut lines = head.split("\r\n");
    let first = lines.next()?;
    let mut parts = first.split_whitespace();
    let method = parts.next()?.to_string();
    let path = parts.next()?.to_string();
    let version = parts.next().unwrap_or("HTTP/1.0");
    let mut keep_alive = version.eq_ignore_ascii_case("HTTP/1.1");
    let mut range = None;
    let mut content_len = 0usize;
    for l in lines {
        if let Some((k, v)) = l.split_once(':') {
            let k = k.trim().to_ascii_lowercase();
            let v = v.trim();
            match k.as_str() {
                "range" => range = Some(v.to_string()),
                "connection" => {
                    let lv = v.to_ascii_lowercase();
                    if lv.contains("close") {
                        keep_alive = false;
                    } else if lv.contains("keep-alive") {
                        keep_alive = true;
                    }
                }
                "content-length" => content_len = v.parse().unwrap_or(0),
                _ => {}
            }
        }
    }
    // Discard any request body (we only serve GET/HEAD).
    if content_len > 0 {
        if content_len > 64 * 1024 {
            return None;
        }
        while buf.len() < content_len {
            let n = s.read(&mut tmp).ok()?;
            if n == 0 {
                return None;
            }
            buf.extend_from_slice(&tmp[..n]);
        }
        buf.drain(..content_len);
    }
    Some(Req {
        method,
        path,
        range,
        keep_alive,
    })
}

fn simple(
    s: &mut TcpStream,
    code: u16,
    reason: &str,
    ctype: &str,
    body: &[u8],
    keep: bool,
    head_only: bool,
) -> bool {
    let h = format!(
        "HTTP/1.1 {code} {reason}\r\nContent-Type: {ctype}\r\nContent-Length: {}\r\nAccess-Control-Allow-Origin: *\r\nConnection: {}\r\n\r\n",
        body.len(),
        if keep { "keep-alive" } else { "close" }
    );
    if s.write_all(h.as_bytes()).is_err() {
        return false;
    }
    head_only || s.write_all(body).is_ok()
}

fn handle_conn(sh: &Arc<Shared>, mut s: TcpStream, peer: SocketAddr) {
    let _ = s.set_nodelay(true);
    let _ = s.set_read_timeout(Some(Duration::from_secs(90)));
    let _ = s.set_write_timeout(Some(Duration::from_secs(90)));
    let mut buf = Vec::with_capacity(4096);
    while sh.running.load(Ordering::SeqCst) {
        let Some(req) = read_request(&mut s, &mut buf) else {
            break;
        };
        let keep = req.keep_alive;
        let ok = route(sh, &mut s, &req, peer);
        if !ok || !keep {
            break;
        }
    }
    let _ = s.shutdown(Shutdown::Both);
}

fn route(sh: &Arc<Shared>, s: &mut TcpStream, req: &Req, peer: SocketAddr) -> bool {
    let head_only = req.method == "HEAD";
    if req.method != "GET" && !head_only {
        if req.method == "OPTIONS" {
            return s
                .write_all(b"HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, HEAD, OPTIONS\r\nAccess-Control-Allow-Headers: *\r\nContent-Length: 0\r\n\r\n")
                .is_ok()
                && req.keep_alive;
        }
        simple(
            s,
            405,
            "Method Not Allowed",
            "text/plain",
            b"method not allowed",
            false,
            false,
        );
        return false;
    }
    let raw_path = req.path.split('?').next().unwrap_or("/");
    if raw_path == "/" || raw_path == "/index.html" {
        let body = index_page(sh);
        return simple(
            s,
            200,
            "OK",
            "text/html; charset=utf-8",
            body.as_bytes(),
            req.keep_alive,
            head_only,
        );
    }
    if let Some(rest) = raw_path.strip_prefix("/icon/") {
        let id = rest.trim_end_matches(".png");
        let entry = sh.files.read().unwrap().get(id).cloned();
        if let Some(png) =
            entry.and_then(|e| e.icon.and_then(|i| crate::pkg::read_icon(&e.path, i)))
        {
            return simple(s, 200, "OK", "image/png", &png, req.keep_alive, head_only);
        }
        return simple(
            s,
            404,
            "Not Found",
            "text/plain",
            b"no icon",
            req.keep_alive,
            head_only,
        );
    }

    let entry = if let Some(rest) = raw_path.strip_prefix("/f/") {
        let p = pct_decode(rest);
        let id = sh.by_path.read().unwrap().get(&path_key(&p)).cloned();
        id.and_then(|id| sh.files.read().unwrap().get(&id).cloned())
    } else {
        let mut it = raw_path.trim_start_matches('/').splitn(2, '/');
        let id = it.next().unwrap_or("");
        sh.files.read().unwrap().get(id).cloned()
    };
    let Some(entry) = entry else {
        (sh.log)(format!("{} 404 {}", peer.ip(), pct_decode(raw_path)));
        return simple(
            s,
            404,
            "Not Found",
            "text/plain",
            b"file not found",
            req.keep_alive,
            head_only,
        );
    };
    if sh.blocked.read().unwrap().contains(&entry.id) {
        simple(
            s,
            403,
            "Forbidden",
            "text/plain",
            b"transfer stopped",
            false,
            head_only,
        );
        return false;
    }
    serve_file(sh, s, &entry, req, head_only, peer)
}

fn parse_range(spec: &str, total: u64) -> Result<Option<(u64, u64)>, ()> {
    let Some(r) = spec.trim().strip_prefix("bytes=") else {
        return Ok(None);
    };
    let first = r.split(',').next().unwrap_or("").trim();
    let (a, b) = first.split_once('-').ok_or(())?;
    let (a, b) = (a.trim(), b.trim());
    if total == 0 {
        return Err(());
    }
    let (start, end) = if a.is_empty() {
        let n: u64 = b.parse().map_err(|_| ())?;
        if n == 0 {
            return Err(());
        }
        (total.saturating_sub(n), total - 1)
    } else {
        let s: u64 = a.parse().map_err(|_| ())?;
        let e: u64 = if b.is_empty() {
            total - 1
        } else {
            b.parse::<u64>().map_err(|_| ())?.min(total - 1)
        };
        (s, e)
    };
    if start > end || start >= total {
        return Err(());
    }
    Ok(Some((start, end)))
}

fn serve_file(
    sh: &Arc<Shared>,
    s: &mut TcpStream,
    e: &Entry,
    req: &Req,
    head_only: bool,
    peer: SocketAddr,
) -> bool {
    let mut f = match File::open(&e.path) {
        Ok(f) => f,
        Err(err) => {
            (sh.log)(format!("ERROR cannot open {}: {err}", e.path.display()));
            return simple(
                s,
                404,
                "Not Found",
                "text/plain",
                b"file missing on disk",
                req.keep_alive,
                head_only,
            );
        }
    };
    let total = f.metadata().map(|m| m.len()).unwrap_or(0);
    let range = match req.range.as_deref().map(|r| parse_range(r, total)) {
        None => None,
        Some(Ok(r)) => r,
        Some(Err(())) => {
            let h = format!(
                "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */{total}\r\nContent-Length: 0\r\nConnection: {}\r\n\r\n",
                if req.keep_alive { "keep-alive" } else { "close" }
            );
            return s.write_all(h.as_bytes()).is_ok() && req.keep_alive;
        }
    };
    let (start, end) = range.unwrap_or((0, total.saturating_sub(1)));
    let len = if total == 0 { 0 } else { end - start + 1 };
    let mut h = String::with_capacity(256);
    if range.is_some() {
        h.push_str(&format!(
            "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes {start}-{end}/{total}\r\n"
        ));
    } else {
        h.push_str("HTTP/1.1 200 OK\r\n");
    }
    h.push_str(&format!(
        "Content-Type: application/octet-stream\r\nContent-Length: {len}\r\nAccept-Ranges: bytes\r\nAccess-Control-Allow-Origin: *\r\nConnection: {}\r\n\r\n",
        if req.keep_alive { "keep-alive" } else { "close" }
    ));
    if s.write_all(h.as_bytes()).is_err() {
        return false;
    }
    if head_only || len == 0 {
        return req.keep_alive;
    }
    if start == 0 {
        (sh.log)(format!("{} started download: {}", peer.ip(), e.name));
    }
    if f.seek(SeekFrom::Start(start)).is_err() {
        return false;
    }
    {
        let mut st = sh.stats.lock().unwrap();
        let fs = st.entry(e.id.clone()).or_default();
        fs.active += 1;
        fs.last_activity = Some(Instant::now());
    }
    let mut buf = vec![0u8; CHUNK];
    let mut pos = start;
    let mut left = len;
    let mut ok = true;
    while left > 0 {
        if !sh.running.load(Ordering::Relaxed) || sh.blocked.read().unwrap().contains(&e.id) {
            ok = false;
            break;
        }
        let want = (left as usize).min(CHUNK);
        let n = match f.read(&mut buf[..want]) {
            Ok(0) | Err(_) => {
                ok = false;
                break;
            }
            Ok(n) => n,
        };
        if s.write_all(&buf[..n]).is_err() {
            ok = false;
            break;
        }
        sh.total_sent.fetch_add(n as u64, Ordering::Relaxed);
        {
            let mut st = sh.stats.lock().unwrap();
            let fs = st.entry(e.id.clone()).or_default();
            fs.add(pos, pos + n as u64);
            fs.bytes_sent += n as u64;
            fs.last_activity = Some(Instant::now());
        }
        pos += n as u64;
        left -= n as u64;
    }
    {
        let mut st = sh.stats.lock().unwrap();
        if let Some(fs) = st.get_mut(&e.id) {
            fs.active = fs.active.saturating_sub(1);
        }
    }
    ok && req.keep_alive
}

fn html_escape(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
}

fn index_page(sh: &Shared) -> String {
    let files = sh.files.read().unwrap();
    let mut v: Vec<&Entry> = files.values().collect();
    v.sort_by_key(|a| a.name.to_lowercase());
    let mut rows = String::new();
    for e in v {
        let size = std::fs::metadata(&e.path).map(|m| m.len()).unwrap_or(0);
        rows.push_str(&format!(
            "<tr><td><a href=\"{}\">{}</a></td><td>{:.2} GB</td></tr>",
            url_path(&e.id, &e.path.to_string_lossy(), &e.name, false),
            html_escape(&e.name),
            size as f64 / 1_073_741_824.0
        ));
    }
    format!(
        "<!doctype html><html><head><meta charset=utf-8><title>PS4 Package Sender</title><style>body{{background:#0d0d0d;color:#ddd;font:14px Segoe UI,sans-serif;padding:24px}}a{{color:#6bd38a;text-decoration:none}}td{{padding:6px 14px;border-bottom:1px solid #222}}h2{{font-weight:500}}</style></head><body><h2>PS4 Package Sender - served files</h2><table>{rows}</table></body></html>"
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ranges() {
        assert_eq!(parse_range("bytes=0-99", 1000), Ok(Some((0, 99))));
        assert_eq!(parse_range("bytes=900-", 1000), Ok(Some((900, 999))));
        assert_eq!(parse_range("bytes=-100", 1000), Ok(Some((900, 999))));
        assert_eq!(parse_range("bytes=0-5000", 1000), Ok(Some((0, 999))));
        assert!(parse_range("bytes=1000-", 1000).is_err());
        let mut f = FileStats::default();
        f.add(0, 10);
        f.add(5, 20);
        f.add(30, 40);
        assert_eq!(f.covered(), 30);
    }
}
