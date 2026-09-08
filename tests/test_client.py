#!/usr/bin/env python3
"""Real C++ client + C server through deterministic UDP loss/jitter/reordering."""
import heapq
import itertools
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]

def udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 0))
    return s

def main():
    reservation = udp()
    port = reservation.getsockname()[1]
    reservation.close()
    with tempfile.TemporaryDirectory(prefix='fzvs-memory-') as directory:
        ready = Path(directory) / 'ready'
        with (Path(directory) / 'server.log').open('w+') as log:
            server = subprocess.Popen([str(ROOT/'build/server/fzvs-server'), '--port', str(port),
                '--players', '4', '--room-key', 'memory-test', '--ready-file', str(ready)], stdout=log, stderr=log)
            sockets = []
            stop = threading.Event()
            thread = None
            try:
                deadline = time.monotonic() + 5
                while not ready.exists():
                    assert server.poll() is None and time.monotonic() < deadline, 'Server startup failed'
                    time.sleep(.01)
                pairs = [(udp(), udp()) for _ in range(4)]
                sockets = [s for pair in pairs for s in pair]
                routes = {s: (i, side) for i,pair in enumerate(pairs) for side,s in enumerate(pair)}
                stats = {'dropped_commands': 0, 'dropped_snapshots': 0, 'duplicated_states': 0}
                def proxy():
                    destinations = {}
                    counts = [0]*4
                    dropped = set()
                    pending = []
                    serial = itertools.count()
                    while not stop.is_set():
                        readable, _, _ = select.select(sockets, [], [], .002)
                        for s in readable:
                            data, address = s.recvfrom(4096)
                            i, side = routes[s]
                            if side == 0:
                                destinations[i] = address
                                if len(data)>5 and data[5] == 3 and i not in dropped:
                                    dropped.add(i); stats['dropped_commands'] += 1
                                    continue
                                target, endpoint = pairs[i][1], ('127.0.0.1', port)
                                delay = .008
                                if len(data)>5 and data[5] == 4:
                                    # A delayed duplicate exercises sequence rejection.
                                    heapq.heappush(pending, (time.monotonic()+.08, next(serial), target, endpoint, data))
                                    stats['duplicated_states'] += 1
                            else:
                                if i not in destinations: continue
                                target, endpoint = pairs[i][0], destinations[i]
                                counts[i] += 1
                                if len(data)>5 and data[5] == 5 and counts[i]%11 == 0:
                                    stats['dropped_snapshots'] += 1
                                    continue
                                delay = .035 if counts[i]%3 == 0 else .005
                            heapq.heappush(pending, (time.monotonic()+delay, next(serial), target, endpoint, data))
                        while pending and pending[0][0] <= time.monotonic():
                            _, _, target, endpoint, data = heapq.heappop(pending)
                            target.sendto(data, endpoint)
                thread = threading.Thread(target=proxy)
                thread.start()
                result = subprocess.run([str(ROOT/'build/tests/client-test'), *[str(p[0].getsockname()[1]) for p in pairs]],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=45)
                if result.returncode:
                    print(result.stdout)
                    log.flush(); log.seek(0); print(log.read())
                    raise AssertionError(f'Client test exited {result.returncode}')
                assert stats['dropped_commands'] == 4 and stats['dropped_snapshots'] and stats['duplicated_states']
                print(result.stdout.splitlines()[-1])
                print('Fault injection:', stats)
            finally:
                stop.set()
                if thread: thread.join(2)
                for s in sockets: s.close()
                server.terminate()
                server.wait(timeout=5)
if __name__ == '__main__':
    main()
