# PS4-PS5 Package Sender

Windows remote PKG installer for PS4 and PS5.
Built with Rust + Tauri 2 (WebView2) and a plain HTML/CSS/JS UI with no bundler.

## Supported console apps

| Choice in *Console → Installer on the console* | Protocol | Progress |
|---|---|---|
| **PS4 RPI (flatZ)** (also PS4Hub) | HTTP `:12800` `/api/install`, `/api/get_task_progress`, pause/resume/stop, `is_exists`, uninstall | read from the console |
| **PS5 etaHEN – DPI v2** | HTTP `:12800` `POST /upload` (`url`, `content_name`, `content_id`, `icon_url`) | measured by this app's server (bytes sent) |
| **PS5 etaHEN – DPI v1** | raw TCP `:9090`, JSON `{"url":...}` → `{"res":"0"}` | measured by this app's server |

## Ports – why they no longer stay blocked

* The file server runs **inside the app process** (`std::net` threads). There is no
  separate Node/express process that can outlive the window.
* Closing the app (X, power button, Alt+F4) stops the server, resets open connections (RST, so no
  TIME_WAIT is left behind) and exits. After a hard kill (Task Manager) Windows frees the socket
  together with the process. Measured: a restart listens again after ~0.5 s.
* Sockets are not inherited by child processes (Rust std uses `WSA_FLAG_NO_HANDLE_INHERIT`).
* The listening socket uses `SO_EXCLUSIVEADDRUSE`, so no other process can share the port with us.
* Before binding, the app checks the system TCP table. Windows will happily let a second process
  bind a port whose owner used `SO_REUSEADDR`, after which console connections land in either
  process at random. If the port is taken, a banner shows **who** holds it, with "Close <process>"
  and "Use next free port" buttons. A stale instance of this same exe is closed automatically.
* Single instance: launching the app again focuses the existing window instead of starting a
  second server.

## Build

```powershell
cargo build --release        # -> target\release\ps4-ps5-package-sender.exe (single file, ~5 MB)
```

Optional installer: `cargo install tauri-cli --version "^2"` then `cargo tauri build` (NSIS).

## Tests

```powershell
cargo test                                         # unit tests
python tools\mock_console.py 12855                 # in a second terminal
cargo test e2e -- --ignored --nocapture            # full flatZ + etaHEN flow + port release
python tools\ui_preview.py                         # UI preview in a browser (mocked backend)
```

## Notes

* On the first server start Windows Firewall asks for permission. Allow **Private networks**,
  otherwise the console cannot download.
* Config: `%APPDATA%\com.pkgsender.ps4\config.json`.
* Drag & drop PKG files or whole folders onto the window (they appear in Library → Added).

## Credits

* **laroping**: https://github.com/laroping
