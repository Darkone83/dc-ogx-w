"""Existing DC-OGX-W UDP v1 discovery, including legacy ESP beacons."""
from __future__ import annotations
import select
import socket
import time

PORT = 38473
PROBE = b"DC-OGX-W|DISCOVER|1"

def parse_announcement(packet: bytes, source: str) -> dict | None:
    try:
        fields = packet.decode('utf-8').rstrip('\r\n').split('|')
        if len(fields) != 6 or fields[:2] != ['DC-OGX-W', '1']:
            return None
        pairs = [field.split('=', 1) for field in fields[2:]]
        if any(len(pair) != 2 for pair in pairs):
            return None
        data = dict(pairs)
        if set(data) != {'id', 'board', 'ip', 'http'}:
            return None
        socket.inet_pton(socket.AF_INET, data['ip'])
        port = int(data['http'])
        if data['ip'] != source or not 0 < port <= 65535 or not 0 < len(data['id']) <= 48 or not 0 < len(data['board']) <= 64:
            return None
        return {'id': data['id'], 'board': data['board'], 'ip': data['ip'], 'http_port': port}
    except (UnicodeError, ValueError, OSError):
        return None

def discover(timeout: float = 6.2) -> list[dict]:
    sockets = []
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sockets.append(probe)
        probe.bind(('0.0.0.0', 0))
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        probe.setblocking(False)
        beacon = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            beacon.bind(('0.0.0.0', PORT))
            beacon.setblocking(False)
            sockets.append(beacon)
        except OSError:
            beacon.close()
        start = time.monotonic()
        next_probe = 0.0
        found = {}
        while time.monotonic() - start < timeout:
            now = time.monotonic()
            if found and now - start >= 1.9:
                break
            if now >= next_probe:
                try:
                    probe.sendto(PROBE, ('255.255.255.255', PORT))
                except OSError:
                    pass  # VPN broadcast failure must not discard legacy beacons.
                next_probe = now + .45
            readable, _, _ = select.select(sockets, [], [], min(.16, max(0, timeout - (now-start))))
            for sock in readable:
                try:
                    packet, sender = sock.recvfrom(512)
                except OSError:
                    continue
                data = parse_announcement(packet, sender[0])
                if data:
                    found.setdefault(data['id'], data)
        return list(found.values())
    finally:
        for sock in sockets:
            sock.close()
