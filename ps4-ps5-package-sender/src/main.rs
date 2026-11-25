#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod console;
mod core;
mod http;
mod pkg;
mod server;
mod win;

use std::path::PathBuf;
use std::sync::Arc;

use tauri::{DragDropEvent, Emitter, Manager, RunEvent, State, WindowEvent};
use tauri_plugin_dialog::DialogExt;

use crate::core::{Config, Core, LogLine, Snapshot};

type Shared<'a> = State<'a, Arc<Core>>;

async fn blocking<T: Send + 'static>(f: impl FnOnce() -> T + Send + 'static) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(f)
        .await
        .map_err(|e| e.to_string())
}

#[tauri::command]
fn get_snapshot(core: Shared) -> Snapshot {
    core.snapshot(true)
}

#[tauri::command]
fn get_logs(core: Shared) -> Vec<LogLine> {
    core.logs()
}

#[tauri::command]
fn list_interfaces() -> Vec<win::Iface> {
    win::interfaces()
}

#[tauri::command]
fn save_config(core: Shared, cfg: Config) {
    core.apply_config(cfg);
}

#[tauri::command]
async fn server_start(core: Shared<'_>) -> Result<(), String> {
    let c = core.inner().clone();
    blocking(move || c.start_server(true)).await?
}

#[tauri::command]
async fn server_stop(core: Shared<'_>) -> Result<(), String> {
    let c = core.inner().clone();
    blocking(move || c.stop_server()).await
}

#[tauri::command]
async fn server_restart(core: Shared<'_>) -> Result<(), String> {
    let c = core.inner().clone();
    blocking(move || {
        c.stop_server();
        c.start_server(true)
    })
    .await?
}

#[tauri::command]
async fn kill_port_owner(core: Shared<'_>, pid: u32) -> Result<(), String> {
    let c = core.inner().clone();
    blocking(move || c.kill_port_owner(pid)).await?
}

#[tauri::command]
async fn use_next_port(core: Shared<'_>) -> Result<u16, String> {
    let c = core.inner().clone();
    blocking(move || c.use_next_free_port()).await?
}

#[tauri::command]
async fn pick_folder(app: tauri::AppHandle) -> Result<Option<String>, String> {
    blocking(move || {
        app.dialog()
            .file()
            .set_title("Select the base folder of your PKG files")
            .blocking_pick_folder()
    })
    .await
    .map(|r| {
        r.and_then(|p| p.into_path().ok())
            .map(|p| p.to_string_lossy().to_string())
    })
}

#[tauri::command]
async fn pick_files(app: tauri::AppHandle, core: Shared<'_>) -> Result<usize, String> {
    let picked = blocking(move || {
        app.dialog()
            .file()
            .add_filter("PlayStation package", &["pkg", "PKG"])
            .blocking_pick_files()
    })
    .await?;
    let paths: Vec<PathBuf> = picked
        .unwrap_or_default()
        .into_iter()
        .filter_map(|p| p.into_path().ok())
        .collect();
    let n = paths.len();
    if n > 0 {
        core.add_dragged(paths);
    }
    Ok(n)
}

#[tauri::command]
fn rescan(core: Shared) {
    core.scan_base();
}

#[tauri::command]
fn add_paths(core: Shared, paths: Vec<String>) {
    core.add_dragged(paths.into_iter().map(PathBuf::from).collect());
}

#[tauri::command]
fn remove_files(core: Shared, ids: Vec<String>) {
    core.remove_files(&ids);
}

#[tauri::command]
fn get_icon(core: Shared, id: String) -> Option<String> {
    core.icon_data_url(&id)
}

#[tauri::command]
fn queue_add(core: Shared, ids: Vec<String>) -> usize {
    core.queue_add(&ids)
}

#[tauri::command]
fn install_now(core: Shared, id: String) -> Result<(), String> {
    core.queue_add(std::slice::from_ref(&id));
    let qid = {
        let g = core.inner.lock().unwrap();
        g.queue
            .iter()
            .rev()
            .find(|q| q.file_id == id && q.status == "queued")
            .map(|q| q.qid)
    };
    let qid = qid.ok_or("already in progress")?;
    core.start_items(vec![qid]);
    Ok(())
}

#[tauri::command]
fn queue_start(core: Shared, qids: Vec<u64>) {
    core.start_items(qids);
}

#[tauri::command]
fn queue_autostart(core: Shared) -> usize {
    core.autostart()
}

#[tauri::command]
async fn queue_action(core: Shared<'_>, qid: u64, action: String) -> Result<(), String> {
    let c = core.inner().clone();
    blocking(move || c.item_action(qid, &action)).await?
}

#[tauri::command]
fn queue_remove(core: Shared, qids: Vec<u64>) {
    core.queue_remove(&qids);
}

#[tauri::command]
fn queue_reset(core: Shared, qids: Vec<u64>) {
    core.queue_reset(&qids);
}

#[tauri::command]
async fn test_connection(core: Shared<'_>) -> Result<String, String> {
    let c = core.console_now();
    if c.ip.is_empty() {
        return Err("PlayStation IP is not set".into());
    }
    blocking(move || c.test()).await?
}

#[tauri::command]
async fn scan_network(core: Shared<'_>) -> Result<Vec<console::Found>, String> {
    let ip = core.inner.lock().unwrap().cfg.server_ip.clone();
    blocking(move || console::scan_network(&ip)).await
}

#[tauri::command]
async fn check_installed(core: Shared<'_>, title_id: String) -> Result<Option<u64>, String> {
    let c = core.console_now();
    blocking(move || c.is_exists(title_id.trim())).await?
}

#[tauri::command]
async fn uninstall(core: Shared<'_>, kind: String, id: String) -> Result<(), String> {
    let c = core.console_now();
    let cc = core.inner().clone();
    let r = blocking(move || c.uninstall(&kind, id.trim()).map(|_| (kind, id))).await?;
    let (kind, id) = r?;
    cc.log("ok", format!("Uninstalled {kind} {id}"));
    Ok(())
}

#[tauri::command]
fn open_external(target: String) -> Result<(), String> {
    let ok = target.starts_with("http://")
        || target.starts_with("https://")
        || std::path::Path::new(&target).is_dir();
    if !ok {
        return Err("refused".into());
    }
    std::process::Command::new("explorer.exe")
        .arg(&target)
        .spawn()
        .map(|_| ())
        .map_err(|e| e.to_string())
}

#[tauri::command]
fn reveal_file(path: String) -> Result<(), String> {
    if !std::path::Path::new(&path).is_file() {
        return Err("file not found".into());
    }
    std::process::Command::new("explorer.exe")
        .arg(format!("/select,{path}"))
        .spawn()
        .map(|_| ())
        .map_err(|e| e.to_string())
}

#[tauri::command]
fn quit_app(app: tauri::AppHandle, core: Shared) {
    core.shutdown();
    app.exit(0);
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, _argv, _cwd| {
            // A second launch must not fight for the port - focus the running window instead.
            if let Some(w) = app.get_webview_window("main") {
                let _ = w.unminimize();
                let _ = w.show();
                let _ = w.set_focus();
            }
        }))
        .plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            let cfg_dir = app
                .path()
                .app_config_dir()
                .unwrap_or_else(|_| PathBuf::from("."));
            let core = Core::new(cfg_dir.join("config.json"));
            let _ = core.app.set(app.handle().clone());
            core.ensure_server_ip();
            core.log(
                "info",
                format!(
                    "PS4-PS5 Package Sender {} started",
                    env!("CARGO_PKG_VERSION")
                ),
            );
            core.load_dragged();
            let auto_scan = core.inner.lock().unwrap().cfg.auto_scan;
            if auto_scan {
                core.scan_base();
            }
            let auto_server = core.inner.lock().unwrap().cfg.auto_start_server;
            if auto_server {
                let c = core.clone();
                std::thread::spawn(move || {
                    let _ = c.start_server(true);
                });
            }
            core.spawn_workers();
            app.manage(core);
            Ok(())
        })
        .on_window_event(|window, event| match event {
            WindowEvent::DragDrop(DragDropEvent::Enter { .. }) => {
                let _ = window.emit("drag", "enter");
            }
            WindowEvent::DragDrop(DragDropEvent::Leave) => {
                let _ = window.emit("drag", "leave");
            }
            WindowEvent::DragDrop(DragDropEvent::Drop { paths, .. }) => {
                let _ = window.emit("drag", "drop");
                if let Some(core) = window.app_handle().try_state::<Arc<Core>>() {
                    core.add_dragged(paths.clone());
                }
            }
            WindowEvent::CloseRequested { .. } => {
                if let Some(core) = window.app_handle().try_state::<Arc<Core>>() {
                    core.shutdown();
                }
            }
            _ => {}
        })
        .invoke_handler(tauri::generate_handler![
            get_snapshot,
            get_logs,
            list_interfaces,
            save_config,
            server_start,
            server_stop,
            server_restart,
            kill_port_owner,
            use_next_port,
            pick_folder,
            pick_files,
            rescan,
            add_paths,
            remove_files,
            get_icon,
            queue_add,
            install_now,
            queue_start,
            queue_autostart,
            queue_action,
            queue_remove,
            queue_reset,
            test_connection,
            scan_network,
            check_installed,
            uninstall,
            open_external,
            reveal_file,
            quit_app
        ])
        .build(tauri::generate_context!())
        .expect("error while building the application")
        .run(|app, event| {
            if let RunEvent::Exit | RunEvent::ExitRequested { .. } = event {
                if let Some(core) = app.try_state::<Arc<Core>>() {
                    core.shutdown();
                }
            }
        });
}
