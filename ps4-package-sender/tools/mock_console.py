"""Mock PlayStation installer for local testing.

Speaks flatZ RPI (/api/install, /api/get_task_progress, /api/is_exists,
/api/pause_task, ...) with hex numbers like the real thing, and etaHEN DPI v2
(POST /upload form, plain-text SUCCESS reply). "Downloads" the package URL
with 4 MB Range requests like BGFT does, throttled to ~40 MB/s.
Usage: python mock_console.py [port]
"""
import json, re, sys, threading, time, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TASKS = {}
NEXT = [100]
LOCK = threading.Lock()

def downloader(tid, url):
    t = TASKS[tid]
    req = urllib.request.Request(url, method="HEAD")
    with urllib.request.urlopen(req, timeout=10) as r:
        total = int(r.headers["Content-Length"])
    t["length"] = total
    pos, chunk = 0, 4 * 1024 * 1024
    while pos < total:
        if t["stopped"]:
            t["error"] = 0x80990001; return
        if t["paused"]:
            time.sleep(0.2); continue
        end = min(pos + chunk, total) - 1
        req = urllib.request.Request(url, headers={"Range": f"bytes={pos}-{end}"})
        with urllib.request.urlopen(req, timeout=20) as r:
            assert r.status == 206, r.status
            data = r.read()
        assert len(data) == end - pos + 1
        pos += len(data)
        t["transferred"] = pos
        time.sleep(len(data) / (40 * 1024 * 1024))
    t["done"] = True

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def reply(self, code, body, ctype="application/json"):
        b = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(b)
    def do_GET(self):
        self.reply(200, "<html><form id='uploadForm'>etaHEN mock</form></html>", "text/html")
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n).decode("utf-8", "replace")
        if self.path == "/upload":
            m = re.search(r'name="url"\r\n\r\n(.*?)\r\n', raw)
            if not m: return self.reply(200, "No data received", "text/plain")
            url = m.group(1)
            with LOCK:
                tid = NEXT[0]; NEXT[0] += 1
            TASKS[tid] = dict(length=0, transferred=0, paused=False, stopped=False, error=0, done=False)
            threading.Thread(target=downloader, args=(tid, url), daemon=True).start()
            print("etaHEN install", url, "| fields:", re.findall(r'name="(\w+)"', raw), flush=True)
            return self.reply(200, f"SUCCESS: Direct install console Task started for URL: {url}", "text/plain")
        body = json.loads(raw or "{}")
        p = self.path
        if p == "/api/install":
            url = body["packages"][0]
            with LOCK:
                tid = NEXT[0]; NEXT[0] += 1
            TASKS[tid] = dict(length=0, transferred=0, paused=False, stopped=False, error=0, done=False)
            threading.Thread(target=downloader, args=(tid, url), daemon=True).start()
            print("flatZ install", url, "-> task", tid, flush=True)
            return self.reply(200, '{ "status": "success", "task_id": %d, "title": "Mock" }\n' % tid)
        if p == "/api/get_task_progress":
            t = TASKS.get(int(body["task_id"]))
            if not t: return self.reply(500, '{ "status": "fail", "error_code": 0x80990015 }\n')
            rest = 0 if not t["length"] else int((t["length"] - t["transferred"]) / (40 * 1024 * 1024))
            return self.reply(200, '{ "status": "success", "bits": 0x0, "error": %d, "length": 0x%X, "transferred": 0x%X, "length_total": 0x%X, "transferred_total": 0x%X, "num_index": 0, "num_total": 1, "rest_sec": %d, "rest_sec_total": %d, "preparing_percent": 100, "local_copy_percent": 0 }\n'
                % (t["error"], t["length"], t["transferred"], t["length"], t["transferred"], rest, rest))
        if p in ("/api/pause_task", "/api/resume_task", "/api/stop_task"):
            t = TASKS.get(int(body["task_id"]))
            if t:
                t["paused"] = p == "/api/pause_task"
                t["stopped"] = p == "/api/stop_task"
            return self.reply(200, '{ "status": "success" }\n')
        if p == "/api/is_exists":
            return self.reply(200, '{ "status": "success", "exists": "%s", "size": 0x1000 }\n' % ("true" if body.get("title_id") == "CUSA99999" else "false"))
        self.reply(404, '{ "status": "fail", "error": "unknown endpoint" }\n')

port = int(sys.argv[1]) if len(sys.argv) > 1 else 12800
print("mock console on", port, flush=True)
ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
