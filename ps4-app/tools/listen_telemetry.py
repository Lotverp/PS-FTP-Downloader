#!/usr/bin/env python3
import socket
import sys

PORT = 9999
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('0.0.0.0', PORT))
print(f"[PC-Listener] In ascolto telemetria UDP PS4 su porta {PORT}...", flush=True)

try:
    while True:
        data, addr = s.recvfrom(4096)
        msg = data.decode('utf-8', errors='ignore')
        print(f"[{addr[0]}] {msg}", end='', flush=True)
except KeyboardInterrupt:
    pass
