#!/usr/bin/env python3
"""Real C++ worker integration test; stdlib only. --compose uses the running stack."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
PARSER = argparse.ArgumentParser()
PARSER.add_argument('--compose', action='store_true')
PARSER.add_argument('--hardware', action='store_true')
PARSER.add_argument('--load', type=int, default=2)
ARGS = PARSER.parse_args()
BASE = os.getenv('GRID_URL', 'http://127.0.0.1:8080' if ARGS.compose else 'http://127.0.0.1:18080')
TOKEN = os.getenv('GRID_API_TOKEN', 'local-api-token' if ARGS.compose else 'integration-api-token')
processes = []
handles = []

def api(path, body=None, expected=200):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(BASE + path, data=data, headers={
        'Content-Type': 'application/json', 'Authorization': 'Bearer ' + TOKEN})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            assert r.status == expected, (r.status, expected)
            return json.load(r)
    except urllib.error.HTTPError as e:
        if e.code == expected:
            return json.load(e)
        raise AssertionError((e.code, e.read().decode())) from e

def await_build(bid, timeout=180):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        b = api('/api/builds/' + bid)
        if b['status'] in ('passed', 'failed', 'cancelled'):
            return b
        time.sleep(.3)
    raise AssertionError('build did not finish: ' + bid)

def git(repo, *args):
    return subprocess.check_output(['git', '-C', str(repo), *args], stderr=subprocess.STDOUT).decode().strip()

def repository(temp, name):
    repo = Path(temp) / name
    shutil.copytree(ROOT / 'examples' / name, repo)
    git(repo, 'init', '-b', 'main')
    git(repo, 'add', '.')
    git(repo, '-c', 'user.name=RegressGrid', '-c', 'user.email=test@regressgrid.local', 'commit', '-m', 'Baseline')
    git(repo, 'tag', 'baseline')
    if name == 'cpp':
        for tag, transform in [
            ('regression', lambda s: s.replace('RG_LATENCY_MS 18.2', 'RG_LATENCY_MS 22.7')),
            ('broken', lambda s: '#error deliberate regression fixture\n' + s),
            ('slow', lambda s: s.replace('RG_LATENCY_MS 18.2', 'RG_LATENCY_MS 30000')),
        ]:
            git(repo, 'checkout', '-b', tag, 'baseline')
            path = repo / 'main.cpp'
            path.write_text(transform(path.read_text()))
            git(repo, 'add', '.')
            git(repo, '-c', 'user.name=RegressGrid', '-c', 'user.email=test@regressgrid.local', 'commit', '-m', tag)
        git(repo, 'checkout', 'main')
    return repo.as_uri()

def start(cmd, env, log):
    handle = open(log, 'w')
    handles.append(handle)
    p = subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=handle, stderr=subprocess.STDOUT, start_new_session=True)
    processes.append(p)
    return p

def main(temp):
    if not ARGS.compose:
        subprocess.run(['go', 'build', '-o', 'build/regressgrid', './cmd/server'], cwd=ROOT, check=True)
        repo = repository(temp, 'cpp')
        env = {**os.environ, 'HTTP_ADDR': '127.0.0.1:18080', 'GRPC_ADDR': '127.0.0.1:19090',
               'DATA_DIR': str(Path(temp) / 'data'), 'API_TOKEN': TOKEN, 'WORKER_TOKEN': 'integration-worker-token',
               'DATABASE_URL': '', 'REDIS_URL': ''}
        start([str(ROOT / 'build/regressgrid')], env, Path(temp) / 'server.log')
        for n in range(2):
            start([str(ROOT / 'build/worker/regressgrid-worker')], {
                **env, 'CONTROL_ADDR': '127.0.0.1:19090', 'WORKER_ID': f'e2e-{n}', 'WORKER_SLOTS': '1',
                'WORK_DIR': str(Path(temp) / f'work-{n}')}, Path(temp) / f'worker-{n}.log')
    else:
        repo = 'file:///demo/cpp'
    deadline = time.monotonic() + 60
    while True:
        try:
            state = api('/api/state')
            if len([w for w in state['workers'] if w['healthy']]) >= 2:
                break
        except (urllib.error.URLError, AssertionError):
            pass
        assert time.monotonic() < deadline, 'need two healthy workers'
        time.sleep(.5)
    # macOS aliases gcc to clang; the native smoke test uses actual Clang only.
    matrix = [{'compiler': c, 'build_type': b} for c in (['gcc', 'clang'] if ARGS.compose else ['clang']) for b in ['Debug', 'Release']]
    def submit(commit='baseline', baseline=None, timeout=120, url=repo, plugin='cmake'):
        return api('/api/builds', {'repository': url, 'commit': commit, 'configurations': matrix,
                    'baseline_id': baseline or '', 'timeout_seconds': timeout, 'plugin': plugin}, 201)
    baseline = await_build(submit()['id'])
    assert baseline['status'] == 'passed', baseline
    assert len({j['worker_id'] for j in baseline['jobs']}) >= 2, 'scheduler did not spread jobs'
    for job in baseline['jobs']:
        assert job['metrics']['tests_passed'] == 3, job
        assert job['metrics']['latency_ms'] > 0 and job['metrics']['peak_memory_bytes'] > 0
        for artifact in job['artifacts']:
            req = urllib.request.Request(BASE + f"/api/jobs/{job['id']}/artifacts/{artifact['name']}",
                    headers={'Authorization': 'Bearer ' + TOKEN})
            with urllib.request.urlopen(req) as r:
                assert hashlib.sha256(r.read()).hexdigest() == artifact['sha256']
        req = urllib.request.Request(BASE + f"/api/jobs/{job['id']}/logs?stream=1", headers={'Authorization': 'Bearer ' + TOKEN})
        with urllib.request.urlopen(req, timeout=10) as r:
            logs = r.read().decode()
            assert '[stage] tests' in logs and 'event: complete' in logs
    version = 'e2e-' + str(time.time_ns())
    api('/api/builds/' + baseline['id'] + '/promote', {'version': version}, 201)
    api('/api/builds/' + baseline['id'] + '/promote', {'version': version}, 409)
    candidate = await_build(submit('regression', baseline['id'])['id'])
    assert candidate['status'] == 'passed', candidate
    assert candidate['comparison']['status'] == 'regression', candidate
    assert any(c['metric'] == 'latency_ms' and c['regression'] for c in candidate['comparison']['changes'])
    api('/api/builds/' + candidate['id'] + '/promote', {'version': version + '-blocked'}, 409)
    broken = await_build(submit('broken')['id'])
    assert broken['status'] == 'failed' and len({j['failure_signature'] for j in broken['jobs']}) == 1, broken
    timed = await_build(submit('slow', timeout=3)['id'])
    assert timed['status'] == 'failed' and any('timeout' in j.get('error', '') for j in timed['jobs']), timed
    cancel = submit('slow')
    time.sleep(2)
    api('/api/builds/' + cancel['id'] + '/cancel', {})
    assert await_build(cancel['id'])['status'] == 'cancelled'
    loads = [submit()['id'] for _ in range(ARGS.load)]
    for bid in loads:
        assert await_build(bid)['status'] == 'passed'
    if ARGS.hardware:
        hardware = 'file:///demo/hardware' if ARGS.compose else repository(temp, 'hardware')
        b = await_build(submit(url=hardware, plugin='verilator')['id'])
        assert b['status'] == 'passed', b
        assert all(any(a['name'].endswith('.vcd') for a in j['artifacts']) for j in b['jobs'])
        if ARGS.compose:
            failed = await_build(submit('broken', url=hardware, plugin='verilator')['id'])
            assert failed['status'] == 'failed', failed
            assert all(j['metrics']['tests_failed'] > 0 and any(a['name'].endswith('.vcd') for a in j['artifacts']) for j in failed['jobs'])
            for j in failed['jobs']:
                trace = next(a for a in j['artifacts'] if a['name'].endswith('.vcd'))
                req = urllib.request.Request(BASE + f"/api/jobs/{j['id']}/artifacts/{trace['name']}", headers={'Authorization':'Bearer ' + TOKEN})
                with urllib.request.urlopen(req) as r:
                    assert hashlib.sha256(r.read()).hexdigest() == trace['sha256']
    try:
        urllib.request.urlopen(BASE + '/api/state')
        raise AssertionError('unauthenticated API allowed')
    except urllib.error.HTTPError as e:
        assert e.code == 401
    with urllib.request.urlopen(BASE + '/metrics') as r:
        assert 'regressgrid_queue_depth' in r.read().decode()
    print(f"PASS: {len(matrix)}-configuration matrix across two C++ workers; live logs, measured regression, release gate, checksums, auth, {ARGS.load} load-test builds" + '; failure grouping, timeout, cancellation' + ('; Verilator traces' if ARGS.hardware else ''))

with tempfile.TemporaryDirectory(prefix='regressgrid-e2e-') as temp:
    try:
        main(temp)
    except Exception:
        for path in Path(temp).glob('*.log'):
            print(path.name + ':\n' + path.read_text()[-6000:])
        raise
    finally:
        for p in processes:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in processes:
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        for h in handles:
            h.close()
