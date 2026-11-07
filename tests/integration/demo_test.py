#!/usr/bin/env python3
"""Exercise only the demo contract and resource lifecycle, using the built binary."""

import http.client
import os
from pathlib import Path
import queue
import re
import signal
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
ENTRY = ROOT / 'scripts/demo.sh'


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def reclaimed(output):
    directories = re.findall(r'^TEMP_DIR=(.+)$', output, re.MULTILINE)
    require(directories, 'demo did not report its temporary directory')
    for directory in directories:
        require(not Path(directory).exists(), f'temporary directory remains: {directory}')
    for pid in re.findall(r'^GATEWAY_PID=(\d+)$', output, re.MULTILINE):
        try:
            os.kill(int(pid), 0)
        except ProcessLookupError:
            pass
        else:
            raise AssertionError(f'gateway {pid} remains')
    for port in re.findall(r'^(?:GATEWAY|BACKEND_[AB])=http://127.0.0.1:(\d+)$', output, re.MULTILINE):
        with socket.socket() as peer:
            peer.settimeout(1)
            require(peer.connect_ex(('127.0.0.1', int(port))) != 0, f'listener {port} remains')


def run(arguments, cwd, env=None):
    process = subprocess.Popen(['/bin/bash', str(ENTRY), *arguments], cwd=cwd, env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        output, _ = process.communicate(timeout=20)
        print(output, end='')
        return subprocess.CompletedProcess(process.args, process.returncode, output)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate(timeout=3)


def interactive(binary, cwd, signum):
    process = subprocess.Popen(['/bin/bash', str(ENTRY), '--binary', binary], cwd=cwd,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, bufsize=1)
    lines = []
    events = queue.Queue()

    def read():
        for line in process.stdout:
            lines.append(line)
            events.put(line)
        events.put(None)

    reader = threading.Thread(target=read)
    reader.start()
    try:
        deadline = time.monotonic() + 15
        while True:
            remaining = deadline - time.monotonic()
            require(remaining > 0, 'manual demo startup deadline')
            line = events.get(timeout=remaining)
            require(line is not None, 'manual demo exited early: ' + ''.join(lines))
            if 'Ctrl+C to stop' in line:
                break
        output = ''.join(lines)
        port = int(re.search(r'^GATEWAY=http://127.0.0.1:(\d+)$', output, re.MULTILINE)[1])
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=3)
        try:
            connection.request('GET', '/healthz')
            response = connection.getresponse()
            require(response.status == 200, 'manual service unavailable')
            response.read()
        finally:
            connection.close()
        # exec in the Bash entry preserves PID: SIGINT is the same signal delivered by Ctrl+C.
        process.send_signal(signum)
        require(process.wait(timeout=10) == 0, 'signal exit failed')
        reader.join(timeout=2)
        require(not reader.is_alive(), 'output reader remains')
        reclaimed(''.join(lines))
        print(f'PASS manual access + {signal.Signals(signum).name} + cleanup')
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
        reader.join(timeout=3)
        process.stdout.close()


def main():
    def interrupted(*_):
        raise KeyboardInterrupt('demo test interrupted')

    signal.signal(signal.SIGTERM, interrupted)
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='apigate-demo-test-') as temporary:
        cwd = Path(temporary)
        polluted = os.environ.copy()
        polluted.update(APIGATE_ROUTES='invalid', APIGATE_TLS_KEY_FILE='/does/not/exist',
                        APIGATE_LISTEN_ADDRESS='bad-address', APIGATE_METRICS_ENABLED='invalid',
                        APIGATE_AUTH_KEY_UNUSED='not-a-key', APIGATE_VCPKG_ROOT='/does/not/exist')
        # With no CMake/Ninja/bash on PATH, --binary must still work without a build.
        runtime_path = cwd / 'runtime-path'
        runtime_path.mkdir()
        for tool in ['dirname', 'uname', 'python3']:
            (runtime_path / tool).symlink_to(shutil.which(tool))
        polluted['PATH'] = str(runtime_path)
        smoke = run(['--smoke', '--binary', binary], cwd, polluted)
        require(smoke.returncode == 0 and 'DEMO VERIFIED' in smoke.stdout, 'smoke failed')
        reclaimed(smoke.stdout)
        for signum in [signal.SIGINT, signal.SIGTERM]:
            interactive(binary, cwd, signum)
        failed = run(['--smoke', '--binary', '/bin/false'], cwd)
        require(failed.returncode != 0 and 'Demo failed:' in failed.stdout, 'startup failure hidden')
        reclaimed(failed.stdout)
        stalled = cwd / 'stalled-gateway'
        stalled.write_text(f'#!{sys.executable}\nimport threading\nthreading.Event().wait(30)\n')
        stalled.chmod(0o700)
        timed_out = run(['--smoke', '--binary', str(stalled)], cwd)
        require(timed_out.returncode != 0 and 'http_listener_started' in timed_out.stdout,
                'startup timeout was not reported')
        reclaimed(timed_out.stdout)
        for arguments in [['--unknown'], ['--binary'], ['--binary', '/does/not/exist']]:
            result = run(arguments, cwd)
            require(result.returncode != 0, f'invalid argument accepted: {arguments}')
        require(run(['--help'], cwd).returncode == 0, 'help failed')
        tools = cwd / 'minimal-path'
        tools.mkdir()
        for tool in ['dirname', 'uname']:
            (tools / tool).symlink_to('/usr/bin/' + tool)
        missing = os.environ.copy()
        missing['PATH'] = str(tools)
        result = run(['--smoke', '--binary', binary], cwd, missing)
        require(result.returncode != 0 and 'requires python3' in result.stdout,
                'missing Python dependency not diagnosed')
    print('PASS demo lifecycle, failures, external cwd, environment isolation and arguments')


if __name__ == '__main__':
    main()
