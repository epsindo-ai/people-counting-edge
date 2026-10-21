#!/usr/bin/env python3
"""Restart DeepStream when a configured source stops producing frames for 90s.

The demo app can remain alive after a failed RTSP depayloader reconnect. Monitor
per-source FPS, not just process existence. SIGTERM/SIGINT stops supervision.
"""
import configparser
import fcntl
import os
import re
import signal
import subprocess
import sys
import threading
import time

APP = '/opt/nvidia/deepstream/deepstream/bin/deepstream-test5-app'
CONFIG = '/deepstream-mount/config/app.txt'


class FrameProgress:
    def __init__(self, sources, now, timeout=90):
        self.last = [now] * sources
        self.timeout = timeout
        self.started = now
        self.seen_performance = False

    def observe(self, line, now):
        if not line.lstrip().startswith('**PERF:'):
            return
        rates = re.findall(r'([\d.]+)\s+\([\d.]+\)', line)
        if len(rates) != len(self.last):
            return
        self.seen_performance = True
        for i, fps in enumerate(rates):
            if float(fps) > 0:
                self.last[i] = now

    def stalled(self, now):
        # Allow a first TensorRT engine build before normal FPS reporting starts.
        if not self.seen_performance and now - self.started <= 600:
            return []
        return [i for i, last in enumerate(self.last) if now - last > self.timeout]


def source_count():
    cfg = configparser.ConfigParser(interpolation=None, strict=False)
    cfg.read(CONFIG)
    return sum(1 for section in cfg.sections() if re.fullmatch(r'source\d+', section)
               and cfg.getint(section, 'enable', fallback=0) == 1)


def stop_child(child):
    if child.poll() is None:
        try:
            os.killpg(child.pid, signal.SIGINT)
            child.wait(timeout=10)
        except subprocess.TimeoutExpired:
            os.killpg(child.pid, signal.SIGKILL)
            child.wait()
        except ProcessLookupError:
            child.wait()


def main():
    lock = open('/tmp/storecv-deepstream.lock', 'w')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print('DeepStream supervisor is already running.', flush=True)
        return 1
    stop = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.set())
    while not stop.is_set():
        progress = FrameProgress(source_count(), time.monotonic())
        child = subprocess.Popen([APP, '-c', CONFIG], stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, errors='replace',
                                 bufsize=1, start_new_session=True)
        print(f'watchdog: started detector pid={child.pid}', flush=True)

        def output(proc=child, monitor=progress):
            for line in proc.stdout:
                monitor.observe(line, time.monotonic())
                sys.stdout.write(line)
                sys.stdout.flush()

        reader = threading.Thread(target=output, daemon=True)
        reader.start()
        while child.poll() is None and not stop.wait(1):
            stalled = progress.stalled(time.monotonic())
            if stalled:
                print(f'watchdog: no frames for 90s on sources {stalled}; restarting detector', flush=True)
                break
        stop_child(child)
        reader.join(timeout=2)
        child.stdout.close()
        if not stop.is_set():
            print(f'watchdog: detector stopped ({child.returncode}); retrying in 5s', flush=True)
            stop.wait(5)
    lock.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
