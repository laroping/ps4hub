//! Minimal blocking HTTP/1.1 client + helpers. The console-side servers
//! (flatZ RPI / PS4Hub / etaHEN DPI) are tiny embedded servers, so a small
//! hand-rolled client is more predictable than a full stack and compiles fast.

use std::io::{Read, Write};
use std::net::{IpAddr, SocketAddr, TcpStream, ToSocketAddrs};
use std::time::Duration;

pub struct Resp {
    pub status: u16,
    pub body: String,
}

fn friendly(e: &std::io::Error) -> String {
    use std::io::ErrorKind::*;
    match e.kind() {
        TimedOut | WouldBlock => "timeout - console not responding".into(),
        ConnectionRefused => {
            "connection refused - is the installer app running on the console?".into()
        }
        ConnectionReset | ConnectionAborted => "connection reset by console".into(),
        HostUnreachable | NetworkUnreachable => "host unreachable".into(),
        _ => e.to_string(),
    }
}

pub fn resolve(host: &str, port: u16) -> Result<SocketAddr, String> {
    if let Ok(ip) = host.trim().parse::<IpAddr>() {
        return Ok(SocketAddr::new(ip, port));
    }
    (host.trim(), port)
        .to_socket_addrs()
        .map_err(|e| format!("bad address '{host}': {e}"))?
        .next()
        .ok_or_else(|| format!("bad address '{host}'"))
}

pub fn connect(host: &str, port: u16, timeout: Duration) -> Result<TcpStream, String> {
    let addr = resolve(host, port)?;
    let s = TcpStream::connect_timeout(&addr, timeout).map_err(|e| friendly(&e))?;
    let _ = s.set_read_timeout(Some(timeout));
    let _ = s.set_write_timeout(Some(timeout));
    let _ = s.set_nodelay(true);
    Ok(s)
}

fn find(hay: &[u8], needle: &[u8]) -> Option<usize> {
    hay.windows(needle.len()).position(|w| w == needle)
}

fn header_value(head: &str, name: &str) -> Option<String> {
    let lname = name.to_ascii_lowercase();
    head.split("\r\n").skip(1).find_map(|l| {
        let (k, v) = l.split_once(':')?;
        (k.trim().to_ascii_lowercase() == lname).then(|| v.trim().to_string())
    })
}

fn dechunk(mut b: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    while let Some(eol) = find(b, b"\r\n") {
        let line = String::from_utf8_lossy(&b[..eol]);
        let n =
            usize::from_str_radix(line.split(';').next().unwrap_or("0").trim(), 16).unwrap_or(0);
        b = &b[eol + 2..];
        if n == 0 || b.len() < n {
            out.extend_from_slice(&b[..n.min(b.len())]);
            break;
        }
        out.extend_from_slice(&b[..n]);
        b = &b[(n + 2).min(b.len())..];
    }
    out
}

pub fn request(
    host: &str,
    port: u16,
    method: &str,
    path: &str,
    content_type: Option<&str>,
    body: &[u8],
    timeout: Duration,
) -> Result<Resp, String> {
    let mut s = connect(host, port, timeout)?;
    let mut req = format!(
        "{method} {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUser-Agent: PS4PackageSender/1.0\r\nAccept: */*\r\nConnection: close\r\n"
    );
    if method != "GET" || !body.is_empty() {
        req.push_str(&format!("Content-Length: {}\r\n", body.len()));
    }
    if let Some(ct) = content_type {
        req.push_str(&format!("Content-Type: {ct}\r\n"));
    }
    req.push_str("\r\n");
    s.write_all(req.as_bytes()).map_err(|e| friendly(&e))?;
    if !body.is_empty() {
        s.write_all(body).map_err(|e| friendly(&e))?;
    }

    let mut raw = Vec::with_capacity(4096);
    let mut buf = [0u8; 16 * 1024];
    loop {
        match s.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => {
                raw.extend_from_slice(&buf[..n]);
                if let Some(he) = find(&raw, b"\r\n\r\n") {
                    let head = String::from_utf8_lossy(&raw[..he]).to_string();
                    if let Some(cl) =
                        header_value(&head, "content-length").and_then(|v| v.parse::<usize>().ok())
                    {
                        if raw.len() >= he + 4 + cl {
                            break;
                        }
                    }
                }
                if raw.len() > 8 * 1024 * 1024 {
                    break;
                }
            }
            Err(e) => {
                if raw.is_empty() {
                    return Err(friendly(&e));
                }
                break;
            }
        }
    }
    let he = find(&raw, b"\r\n\r\n").ok_or_else(|| {
        if raw.is_empty() {
            "empty response from console".to_string()
        } else {
            "malformed HTTP response".to_string()
        }
    })?;
    let head = String::from_utf8_lossy(&raw[..he]).to_string();
    let status = head
        .split_whitespace()
        .nth(1)
        .and_then(|c| c.parse::<u16>().ok())
        .ok_or("malformed status line")?;
    let mut body = raw[he + 4..].to_vec();
    if header_value(&head, "transfer-encoding")
        .map(|v| v.to_ascii_lowercase().contains("chunked"))
        .unwrap_or(false)
    {
        body = dechunk(&body);
    } else if let Some(cl) =
        header_value(&head, "content-length").and_then(|v| v.parse::<usize>().ok())
    {
        body.truncate(cl);
    }
    Ok(Resp {
        status,
        body: String::from_utf8_lossy(&body).to_string(),
    })
}

/// The flatZ RPI wire format prints numbers as bare hex literals
/// (`"length": 0x1A2B`), which is not valid JSON. Rewrite them to decimal
/// outside of string literals so serde_json can parse the body.
pub fn relax_json(s: &str) -> String {
    let b = s.as_bytes();
    let mut out: Vec<u8> = Vec::with_capacity(b.len() + 16);
    let mut i = 0;
    let mut in_str = false;
    while i < b.len() {
        let c = b[i];
        if in_str {
            out.push(c);
            if c == b'\\' && i + 1 < b.len() {
                out.push(b[i + 1]);
                i += 2;
                continue;
            }
            if c == b'"' {
                in_str = false;
            }
            i += 1;
            continue;
        }
        if c == b'"' {
            in_str = true;
            out.push(c);
            i += 1;
            continue;
        }
        if c == b'0' && i + 1 < b.len() && (b[i + 1] == b'x' || b[i + 1] == b'X') {
            let prev_ok = i == 0 || !(b[i - 1].is_ascii_alphanumeric());
            let mut j = i + 2;
            while j < b.len() && b[j].is_ascii_hexdigit() {
                j += 1;
            }
            if prev_ok && j > i + 2 {
                let hex = std::str::from_utf8(&b[i + 2..j]).unwrap_or("0");
                let v = u64::from_str_radix(hex, 16).unwrap_or(0);
                out.extend_from_slice(v.to_string().as_bytes());
                i = j;
                continue;
            }
        }
        out.push(c);
        i += 1;
    }
    String::from_utf8_lossy(&out).to_string()
}

pub fn parse_json(s: &str) -> Result<serde_json::Value, String> {
    serde_json::from_str(&relax_json(s.trim())).map_err(|_| {
        let t: String = s.trim().chars().take(160).collect();
        format!("unexpected response: {t}")
    })
}

const UNRESERVED: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";

pub fn pct_encode(s: &str) -> String {
    let mut o = String::with_capacity(s.len() * 3);
    for &c in s.as_bytes() {
        if UNRESERVED.contains(&c) {
            o.push(c as char);
        } else {
            o.push_str(&format!("%{c:02X}"));
        }
    }
    o
}

pub fn pct_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' && i + 2 < b.len() {
            if let Ok(v) =
                u8::from_str_radix(std::str::from_utf8(&b[i + 1..i + 3]).unwrap_or("zz"), 16)
            {
                out.push(v);
                i += 3;
                continue;
            }
        }
        out.push(b[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).to_string()
}

pub fn base64(data: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut o = String::with_capacity(data.len().div_ceil(3) * 4);
    for ch in data.chunks(3) {
        let n = (ch[0] as u32) << 16
            | (*ch.get(1).unwrap_or(&0) as u32) << 8
            | *ch.get(2).unwrap_or(&0) as u32;
        o.push(T[(n >> 18) as usize & 63] as char);
        o.push(T[(n >> 12) as usize & 63] as char);
        o.push(if ch.len() > 1 {
            T[(n >> 6) as usize & 63] as char
        } else {
            '='
        });
        o.push(if ch.len() > 2 {
            T[n as usize & 63] as char
        } else {
            '='
        });
    }
    o
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn relax() {
        let v = parse_json(
            r#"{ "status": "success", "length": 0x1F, "s": "0x10", "error_code": 0x80990085 }"#,
        )
        .unwrap();
        assert_eq!(v["length"], 31);
        assert_eq!(v["s"], "0x10");
        assert_eq!(v["error_code"].as_u64(), Some(0x80990085));
    }
    #[test]
    fn pct() {
        assert_eq!(
            pct_decode(&pct_encode("Gra [v1.00] ąę.pkg")),
            "Gra [v1.00] ąę.pkg"
        );
    }
    #[test]
    fn b64() {
        assert_eq!(base64(b"hello"), "aGVsbG8=");
    }
}
