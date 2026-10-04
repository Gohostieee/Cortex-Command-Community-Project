"""Local-only WAN fixture: first peer has a constrained home uplink."""
import argparse
import heapq
import itertools
import select
import socket
import time
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--port', type=int, default=38996)
p.add_argument('--server-port', type=int, default=38995)
p.add_argument('--mbps', type=float, default=4)
p.add_argument('--duration', type=int, default=300)
p.add_argument('--warmup-log')
a = p.parse_args()
listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
if hasattr(socket, 'SIO_UDP_CONNRESET'):
    listener.ioctl(socket.SIO_UDP_CONNRESET, False)
listener.bind(('127.0.0.1', a.port))
listener.setblocking(False)
clients, upstreams = {}, {}
queue, serial = [], itertools.count()
sent = dropped = 0
constrained = not a.warmup_log
next_check = 0
end = time.monotonic() + a.duration
print('READY: loopback WAN fixture, host upload', a.mbps, 'Mbps; guest relay ping 30 ms', flush=True)
while time.monotonic() < end:
    now = time.monotonic()
    if not constrained and now >= next_check:
        next_check = now + .1
        log = Path(a.warmup_log)
        if log.exists() and 'ENCOUNTER: guest world loaded' in log.read_text(errors='replace'):
            constrained = True
            print('CONSTRAINED: guest loaded; applying host upload cap', flush=True)
    timeout = max(0, min(.005, queue[0][0] - now)) if queue else .005
    ready, _, _ = select.select([listener, *upstreams], [], [], timeout)
    for sock in ready:
        for _ in range(2048):
            try:
                data, addr = sock.recvfrom(65535)
            except BlockingIOError:
                break
            except ConnectionResetError:
                # Windows reports an ICMP port-unreachable here after a game
                # process exits. UDP has no connection to tear down.
                break
            now = time.monotonic()
            if sock is listener:
                if addr not in clients:
                    upstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    if hasattr(socket, 'SIO_UDP_CONNRESET'):
                        upstream.ioctl(socket.SIO_UDP_CONNRESET, False)
                    upstream.bind(('127.0.0.1', 0))
                    upstream.setblocking(False)
                    client = {'addr': addr, 'socket': upstream, 'host': not clients, 'next': now}
                    clients[addr] = upstreams[upstream] = client
                client = clients[addr]
                delay = .030 if client['host'] else .015
                start = max(now, client['next'])
                duration = (len(data) + 28) / (a.mbps * 125000) if client['host'] and constrained else 0
                if start - now > .100:
                    dropped += 1
                    continue
                client['next'] = start + duration
                heapq.heappush(queue, (start + duration + delay, next(serial), client['socket'], data, ('127.0.0.1', a.server_port)))
            else:
                client = upstreams[sock]
                delay = .030 if client['host'] else .015
                heapq.heappush(queue, (now + delay, next(serial), listener, data, client['addr']))
    now = time.monotonic()
    while queue and queue[0][0] <= now:
        _, _, sock, data, addr = heapq.heappop(queue)
        sock.sendto(data, addr)
        sent += 1
print('DONE: forwarded', sent, 'dropped', dropped, flush=True)
