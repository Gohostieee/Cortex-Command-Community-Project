"""Bounded, seeded network impairment for native clients of our AWS worker."""
import argparse
import heapq
import itertools
import json
import random
import select
import socket
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', required=True)
    parser.add_argument('--server-port', type=int, default=8100)
    parser.add_argument('--port', type=int, default=38990)
    parser.add_argument('--delay-ms', type=float, default=60)
    parser.add_argument('--jitter-ms', type=float, default=20)
    parser.add_argument('--loss-percent', type=float, default=3)
    parser.add_argument('--mbps', type=float, default=3)
    parser.add_argument('--duration', type=int, default=600)
    parser.add_argument('--stop-file')
    args = parser.parse_args()
    rng = random.Random(20261009)
    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if hasattr(socket, 'SIO_UDP_CONNRESET'):
        listener.ioctl(socket.SIO_UDP_CONNRESET, False)
    listener.bind(('127.0.0.1', args.port))
    listener.setblocking(False)
    upstreams, clients = {}, {}
    queue, serial = [], itertools.count()
    sent = lost = congested = 0
    deadline = time.monotonic() + args.duration
    next_report = time.monotonic() + 10
    print(json.dumps({'ready': True, **vars(args)}), flush=True)
    while time.monotonic() < deadline:
        now = time.monotonic()
        timeout = max(0, min(.005, queue[0][0] - now)) if queue else .005
        ready, _, _ = select.select([listener, *upstreams], [], [], timeout)
        for sock in ready:
            for _ in range(2048):
                try:
                    data, address = sock.recvfrom(65535)
                except (BlockingIOError, ConnectionResetError):
                    break
                now = time.monotonic()
                if sock is listener:
                    if address not in clients:
                        upstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                        if hasattr(socket, 'SIO_UDP_CONNRESET'):
                            upstream.ioctl(socket.SIO_UDP_CONNRESET, False)
                        upstream.bind(('0.0.0.0', 0))
                        upstream.setblocking(False)
                        clients[address] = upstreams[upstream] = {'socket': upstream, 'address': address, 'up': now, 'down': now}
                    client, direction = clients[address], 'up'
                    target_socket, target = client['socket'], (args.server, args.server_port)
                else:
                    client, direction = upstreams[sock], 'down'
                    target_socket, target = listener, client['address']
                if rng.random() < args.loss_percent / 100:
                    lost += 1
                    continue
                start = max(now, client[direction])
                if start - now > .250:
                    congested += 1
                    continue
                duration = (len(data) + 28) / (args.mbps * 125000)
                client[direction] = start + duration
                delay = max(0, args.delay_ms + rng.uniform(-args.jitter_ms, args.jitter_ms)) / 1000
                heapq.heappush(queue, (start + duration + delay, next(serial), target_socket, data, target))
        now = time.monotonic()
        while queue and queue[0][0] <= now:
            _, _, sock, data, target = heapq.heappop(queue)
            sock.sendto(data, target)
            sent += 1
        if now >= next_report:
            print(json.dumps({'forwarded': sent, 'loss_drops': lost, 'congestion_drops': congested, 'peers': len(clients)}), flush=True)
            next_report = now + 10
            if args.stop_file and Path(args.stop_file).exists():
                break
    print(json.dumps({'forwarded': sent, 'loss_drops': lost, 'congestion_drops': congested, 'peers': len(clients)}), flush=True)


if __name__ == '__main__':
    main()
