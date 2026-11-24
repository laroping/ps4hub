//! Windows specifics: which process owns a listening port (so a stale
//! instance can be closed instead of leaving the port blocked), process
//! termination, and the list of active IPv4 network interfaces.

use serde::Serialize;
use std::ffi::c_void;
use std::path::PathBuf;

use windows_sys::Win32::Foundation::{
    CloseHandle, ERROR_BUFFER_OVERFLOW, ERROR_INSUFFICIENT_BUFFER, NO_ERROR,
};
use windows_sys::Win32::NetworkManagement::IpHelper::{
    GetAdaptersAddresses, GetExtendedTcpTable, GAA_FLAG_SKIP_ANYCAST, GAA_FLAG_SKIP_DNS_SERVER,
    GAA_FLAG_SKIP_MULTICAST, IP_ADAPTER_ADDRESSES_LH, MIB_TCP6TABLE_OWNER_PID,
    MIB_TCPTABLE_OWNER_PID, TCP_TABLE_OWNER_PID_LISTENER,
};
use windows_sys::Win32::Networking::WinSock::{AF_INET, AF_INET6, SOCKADDR_IN};
use windows_sys::Win32::System::Threading::{
    OpenProcess, QueryFullProcessImageNameW, TerminateProcess, WaitForSingleObject,
    PROCESS_NAME_WIN32, PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_SYNCHRONIZE, PROCESS_TERMINATE,
};

#[derive(Serialize, Clone, Debug)]
pub struct PortOwner {
    pub pid: u32,
    pub name: String,
    pub path: String,
    pub is_self_exe: bool,
    pub protected: bool,
}

fn tcp_table(af: u32) -> Option<Vec<u8>> {
    unsafe {
        let mut size: u32 = 0;
        let rc = GetExtendedTcpTable(
            std::ptr::null_mut(),
            &mut size,
            0,
            af,
            TCP_TABLE_OWNER_PID_LISTENER,
            0,
        );
        if rc != ERROR_INSUFFICIENT_BUFFER && rc != NO_ERROR {
            return None;
        }
        let mut buf = vec![0u8; size as usize + 1024];
        size = buf.len() as u32;
        let rc = GetExtendedTcpTable(
            buf.as_mut_ptr() as *mut c_void,
            &mut size,
            0,
            af,
            TCP_TABLE_OWNER_PID_LISTENER,
            0,
        );
        (rc == NO_ERROR).then_some(buf)
    }
}

pub fn listening_pids(port: u16) -> Vec<u32> {
    let mut pids = Vec::new();
    unsafe {
        if let Some(buf) = tcp_table(AF_INET as u32) {
            let t = &*(buf.as_ptr() as *const MIB_TCPTABLE_OWNER_PID);
            let rows = std::slice::from_raw_parts(t.table.as_ptr(), t.dwNumEntries as usize);
            for r in rows {
                if u16::from_be(r.dwLocalPort as u16) == port {
                    pids.push(r.dwOwningPid);
                }
            }
        }
        if let Some(buf) = tcp_table(AF_INET6 as u32) {
            let t = &*(buf.as_ptr() as *const MIB_TCP6TABLE_OWNER_PID);
            let rows = std::slice::from_raw_parts(t.table.as_ptr(), t.dwNumEntries as usize);
            for r in rows {
                if u16::from_be(r.dwLocalPort as u16) == port {
                    pids.push(r.dwOwningPid);
                }
            }
        }
    }
    pids.sort_unstable();
    pids.dedup();
    pids
}

pub fn process_path(pid: u32) -> Option<String> {
    unsafe {
        let h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if h.is_null() {
            return None;
        }
        let mut buf = [0u16; 1024];
        let mut len = buf.len() as u32;
        let ok = QueryFullProcessImageNameW(h, PROCESS_NAME_WIN32, buf.as_mut_ptr(), &mut len);
        CloseHandle(h);
        (ok != 0).then(|| String::from_utf16_lossy(&buf[..len as usize]))
    }
}

fn norm(p: &str) -> String {
    p.replace('/', "\\").to_lowercase()
}

pub fn port_owner(port: u16) -> Option<PortOwner> {
    let me = std::process::id();
    let my_exe = std::env::current_exe()
        .ok()
        .map(|p| norm(&p.to_string_lossy()));
    let pid = listening_pids(port).into_iter().find(|&p| p != me)?;
    let path = process_path(pid).unwrap_or_default();
    let name = PathBuf::from(&path)
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_else(|| {
            if pid == 4 {
                "System".into()
            } else {
                format!("PID {pid}")
            }
        });
    let lname = name.to_lowercase();
    let protected = pid <= 4
        || path.is_empty()
        || [
            "svchost.exe",
            "system",
            "lsass.exe",
            "services.exe",
            "wininit.exe",
            "csrss.exe",
            "smss.exe",
        ]
        .contains(&lname.as_str());
    let is_self_exe = !path.is_empty() && my_exe.as_deref() == Some(norm(&path).as_str());
    Some(PortOwner {
        pid,
        name,
        path,
        is_self_exe,
        protected,
    })
}

pub fn kill_process(pid: u32) -> Result<(), String> {
    if pid == std::process::id() || pid <= 4 {
        return Err("refusing to terminate this process".into());
    }
    unsafe {
        let h = OpenProcess(PROCESS_TERMINATE | PROCESS_SYNCHRONIZE, 0, pid);
        if h.is_null() {
            return Err(format!("cannot open process {pid} (access denied?)"));
        }
        let ok = TerminateProcess(h, 1);
        if ok != 0 {
            WaitForSingleObject(h, 4000);
        }
        CloseHandle(h);
        if ok == 0 {
            return Err(format!("TerminateProcess failed for PID {pid}"));
        }
    }
    Ok(())
}

#[derive(Serialize, Clone, Debug)]
pub struct Iface {
    pub name: String,
    pub ip: String,
}

pub fn interfaces() -> Vec<Iface> {
    let mut out = Vec::new();
    unsafe {
        let flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        let mut size: u32 = 32 * 1024;
        let mut buf: Vec<u8>;
        let mut tries = 0;
        loop {
            buf = vec![0u8; size as usize];
            let rc = GetAdaptersAddresses(
                AF_INET as u32,
                flags,
                std::ptr::null(),
                buf.as_mut_ptr() as *mut IP_ADAPTER_ADDRESSES_LH,
                &mut size,
            );
            if rc == NO_ERROR {
                break;
            }
            tries += 1;
            if rc != ERROR_BUFFER_OVERFLOW || tries > 3 {
                return fallback();
            }
        }
        let mut p = buf.as_ptr() as *const IP_ADAPTER_ADDRESSES_LH;
        while !p.is_null() {
            let a = &*p;
            // IfType 24 = software loopback, OperStatus 1 = up
            if a.IfType != 24 && a.OperStatus == 1 {
                let mut name = String::new();
                if !a.FriendlyName.is_null() {
                    let mut n = 0;
                    while *a.FriendlyName.add(n) != 0 && n < 256 {
                        n += 1;
                    }
                    name = String::from_utf16_lossy(std::slice::from_raw_parts(a.FriendlyName, n));
                }
                let mut u = a.FirstUnicastAddress;
                while !u.is_null() {
                    let sa = (*u).Address.lpSockaddr;
                    if !sa.is_null() && (*sa).sa_family == AF_INET {
                        let sin = &*(sa as *const SOCKADDR_IN);
                        let ip = std::net::Ipv4Addr::from(u32::from_be(sin.sin_addr.S_un.S_addr));
                        if !ip.is_loopback() && !ip.is_link_local() {
                            out.push(Iface {
                                name: name.clone(),
                                ip: ip.to_string(),
                            });
                        }
                    }
                    u = (*u).Next;
                }
            }
            p = a.Next;
        }
    }
    if out.is_empty() {
        return fallback();
    }
    out
}

fn fallback() -> Vec<Iface> {
    // Default-route address via a connected (never sent) UDP socket.
    std::net::UdpSocket::bind("0.0.0.0:0")
        .and_then(|s| {
            s.connect("8.8.8.8:80")?;
            s.local_addr()
        })
        .map(|a| {
            vec![Iface {
                name: "Default".into(),
                ip: a.ip().to_string(),
            }]
        })
        .unwrap_or_default()
}
