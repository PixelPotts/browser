#!/usr/bin/env python3
"""
Traffic Sim Web Bridge
Spawns ./traffic_sim --web, pipes JSON via SSE, serves index.html

Usage: python3 web/server.py
"""
import subprocess, threading, queue, os, sys, webbrowser, argparse, json, time
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn

_ap = argparse.ArgumentParser()
_ap.add_argument('--mode', default='normal1', choices=['normal1','default'])
_ap.add_argument('--port', type=int, default=8765)
_args = _ap.parse_args()

PORT    = _args.port
MODE    = _args.mode
DIR     = os.path.dirname(os.path.abspath(__file__))
BINARY  = os.path.join(DIR, '..', 'traffic_sim')
HTML    = os.path.join(DIR, 'index.html')

FAULT_FIFO = '/tmp/traffic_sim_faults'

_clients: list[queue.Queue] = []
_clients_lock = threading.Lock()
_latest: str = ''

_fifo_fd   = None
_fifo_lock = threading.Lock()

def connect_fifo():
    global _fifo_fd
    while True:
        try:
            fd = open(FAULT_FIFO, 'w', buffering=1)
            with _fifo_lock:
                _fifo_fd = fd
            print('[fault] connected to FIFO')
            return
        except Exception:
            time.sleep(0.5)

def send_fault_cmd(cmd: str):
    with _fifo_lock:
        if _fifo_fd:
            try:
                _fifo_fd.write(cmd + '\n')
                _fifo_fd.flush()
            except Exception:
                pass

def broadcast(data: str):
    global _latest
    _latest = data
    with _clients_lock:
        dead = []
        for q in _clients:
            try:
                q.put_nowait(data)
            except queue.Full:
                dead.append(q)
        for q in dead:
            _clients.remove(q)

def sim_reader():
    proc = subprocess.Popen(
        [BINARY, '--web', '--mode', MODE],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=1)
    for line in iter(proc.stdout.readline, b''):
        txt = line.decode('utf-8', errors='replace').strip()
        if txt.startswith('{'):
            broadcast(txt)
    proc.wait()
    print('[sim] process exited')

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_): pass

    def do_POST(self):
        if self.path == '/fault':
            length = int(self.headers.get('Content-Length', 0))
            body   = self.rfile.read(length)
            try:
                data = json.loads(body)
                fid  = str(data.get('id',  '')).strip()
                ftype= str(data.get('type',''))  .strip()
                if fid and ftype:
                    send_fault_cmd(f'FAULT {fid} {ftype}')
                self.send_response(200)
                self.send_header('Content-Type', 'text/plain')
                self.send_header('Access-Control-Allow-Origin', '*')
                self.end_headers()
                self.wfile.write(b'ok')
            except Exception as ex:
                self.send_error(400, str(ex))
        else:
            self.send_error(404)

    def do_GET(self):
        if self.path == '/':
            try:
                with open(HTML, 'rb') as f:
                    data = f.read().decode('utf-8')
                data = data.replace('__SIM_MODE__', MODE).encode('utf-8')
                self.send_response(200)
                self.send_header('Content-Type', 'text/html; charset=utf-8')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            except FileNotFoundError:
                self.send_error(404, 'index.html not found')
        elif self.path == '/stream':
            self._sse()
        elif self.path == '/mode':
            body = MODE.encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_error(404)

    def _sse(self):
        self.send_response(200)
        self.send_header('Content-Type',  'text/event-stream')
        self.send_header('Cache-Control', 'no-cache')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        q: queue.Queue = queue.Queue(maxsize=60)
        with _clients_lock:
            _clients.append(q)
        try:
            if _latest:
                self.wfile.write(f'data: {_latest}\n\n'.encode())
                self.wfile.flush()
            while True:
                try:
                    msg = q.get(timeout=20)
                    self.wfile.write(f'data: {msg}\n\n'.encode())
                    self.wfile.flush()
                except queue.Empty:
                    self.wfile.write(b': keepalive\n\n')
                    self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            with _clients_lock:
                if q in _clients:
                    _clients.remove(q)

class Server(ThreadingMixIn, HTTPServer):
    daemon_threads = True

if __name__ == '__main__':
    if not os.path.exists(BINARY):
        print(f'Error: {BINARY} not found – run "make" first in the traffic_sim directory.')
        sys.exit(1)
    threading.Thread(target=sim_reader, daemon=True).start()
    threading.Thread(target=connect_fifo, daemon=True).start()
    srv = Server(('127.0.0.1', PORT), Handler)
    url = f'http://localhost:{PORT}'
    print(f'  Traffic Control Simulator  →  {url}')
    threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print('\nStopped.')
