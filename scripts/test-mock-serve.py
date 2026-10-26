#!/usr/bin/env python3
"""Lifecycle regression for Mock.serve: readiness, stop, timeout and bad paths."""

import os
import socket
import subprocess
import tempfile
import time
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HOST = Path(os.environ.get("LMCURL_HOST", ROOT / "build-clean/lmcurl")).resolve()
assert HOST.is_file(), f"host is not built: {HOST}"
env = dict(os.environ, LMCURL_LIVE="0")
env["PATH"] = str(HOST.parent) + os.pathsep + env.get("PATH", "")
compiler = subprocess.check_output(
    [str(ROOT / "scripts/select-compiler.sh")], env=env, text=True, timeout=30
).strip()


def compile_program(source: str, directory: Path) -> Path:
    script = directory / "case.lms"
    bytecode = directory / "case.dslb"
    script.write_text(
        'p = _dsl_ cli="lmcurl";\n' + source + "\nend _dsl_\n" + f'savedsl(p,"{bytecode}");\n'
    )
    result = subprocess.run(
        [compiler, str(script)], cwd=directory, env=env, capture_output=True, text=True, timeout=30
    )
    assert result.returncode == 0, result.stderr
    return bytecode


def run_host(bytecode: Path, timeout: float = 5.0) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(HOST), "--dsl", str(bytecode)],
        cwd=bytecode.parent,
        env=env,
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def wait_ready(path: Path) -> str:
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if path.exists():
            value = path.read_text().strip()
            if value:
                return value
        time.sleep(0.01)
    raise AssertionError("Mock.serve did not atomically publish a non-empty ready URL")


with tempfile.TemporaryDirectory(prefix="lmcurl-serve-") as raw:
    root = Path(raw)
    ready, stop = root / "ready.url", root / "stop.signal"
    bytecode = compile_program(f'let m=curl.mock();m.serve("{ready}","{stop}",2000);', root)
    process = subprocess.Popen(
        [str(HOST), "--dsl", str(bytecode)], cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    url = wait_ready(ready)
    with urllib.request.urlopen(url + "/json", timeout=2) as response:
        assert response.status == 200
        assert b'"ok":true' in response.read()
    stop.write_text("stop\n")
    stdout, stderr = process.communicate(timeout=5)
    assert process.returncode == 0, (stdout, stderr)
    with socket.socket() as probe:
        probe.settimeout(1)
        try:
            probe.connect((url.rsplit(":", 1)[0].split("//", 1)[1], int(url.rsplit(":", 1)[1])))
            raise AssertionError("Mock.serve port remained open after stop")
        except (ConnectionRefusedError, TimeoutError, OSError):
            pass

    timeout_bytecode = compile_program(f'let m=curl.mock();m.serve("{root / "timeout.url"}","{root / "never.signal"}",200);', root)
    process = subprocess.Popen([str(HOST), "--dsl", str(timeout_bytecode)], cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    timeout_url = wait_ready(root / "timeout.url")
    stdout, stderr = process.communicate(timeout=5)
    assert process.returncode == 3 and "timed out waiting for stop file" in stderr, stderr
    with socket.socket() as probe:
        probe.settimeout(1)
        try:
            probe.connect((timeout_url.rsplit(":", 1)[0].split("//", 1)[1], int(timeout_url.rsplit(":", 1)[1])))
            raise AssertionError("timed-out Mock.serve port remained open")
        except (ConnectionRefusedError, TimeoutError, OSError):
            pass

    invalid = compile_program(f'let m=curl.mock();m.serve("{root / "same"}","{root / "same"}",0);', root)
    result = run_host(invalid)
    assert result.returncode == 3 and "non-empty paths and timeout" in result.stderr, result.stderr

    stopped = compile_program(f'let m=curl.mock();m.stop();m.serve("{root / "a"}","{root / "b"}",100);', root)
    result = run_host(stopped)
    assert result.returncode == 3 and "server must be running" in result.stderr, result.stderr

    missing = compile_program('let m=curl.mock();m.serve("/proc/lmcurl-serve-ready","/proc/lmcurl-serve-stop",100);', root)
    result = run_host(missing)
    assert result.returncode == 3 and ("cannot open output file" in result.stderr or "publish ready file" in result.stderr), result.stderr

print("PASS Mock.serve lifecycle: ready, HTTP, stop, timeout, invalid paths, stopped server and missing parent")
