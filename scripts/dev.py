#!/usr/bin/env python3
"""Start the built native control plane and two workers. Ctrl-C stops all three."""
import os
from pathlib import Path
import signal
import subprocess
import time
ROOT = Path(__file__).resolve().parents[1]
processes = []
try:
    processes.append(subprocess.Popen([str(ROOT/'build/regressgrid')], cwd=ROOT))
    for n in range(2):
        processes.append(subprocess.Popen([str(ROOT/'build/worker/regressgrid-worker')], cwd=ROOT,
            env={**os.environ,'WORKER_ID':f'local-{n+1}','WORK_DIR':str(ROOT/f'.local/worker-{n+1}')}))
    print('Dashboard: http://localhost:8080 — Ctrl-C stops server and workers', flush=True)
    while all(p.poll() is None for p in processes):
        time.sleep(.5)
except KeyboardInterrupt:
    pass
finally:
    for p in processes:
        if p.poll() is None:
            p.send_signal(signal.SIGTERM)
    for p in processes:
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
