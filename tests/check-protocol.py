#!/usr/bin/env python3
"""Invocation envelope of the real host: one frame, exits 0/2/3/4, load before run, dry, redaction."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'scripts'))
from lmcode_capture import program

HOST = Path(sys.argv[1]).resolve()
COMPILER = sys.argv[2]
SECRET = 'fixture-' + 'k3Xp' * 10
env = dict(os.environ, LMCURL_LIVE='0', LMCURL_TEST_SECRET=SECRET)
env.pop('LMCODE_INVOCATION_FILE', None)
env['PATH'] = str(HOST.parent) + os.pathsep + env['PATH']


def frame(result, code):
    assert result.returncode == code, (code, result.returncode, result.stdout, result.stderr)
    text = result.stdout.decode()
    assert text.count('LMCODE_RETURN_BEGIN\n') == text.count('LMCODE_RETURN_END\n') == 1, text
    assert text.endswith('LMCODE_RETURN_END\n'), text
    value = json.loads(text.split('LMCODE_RETURN_BEGIN\n')[1].splitlines()[0])
    assert value['ok'] == (code == 0), value
    if code:
        assert 'result' not in value, value
    return value


with tempfile.TemporaryDirectory(prefix='lmcurl protocol ') as directory:
    work = Path(directory).resolve()

    def compile(name, body):
        return program(COMPILER, HOST, body, work / (name + '.bin'), env, work)

    def run(files, dry=None, extra=None, change=None, flags=()):
        environment = dict(env, **(extra or {}))
        if dry is not None:
            value = {'version': 1, 'mode': 'builder' if len(files) > 1 else 'single', 'dry': dry,
                     'parts': [{'index': i, 'file': str(f)} for i, f in enumerate(files)]}
            if change:
                value = change(value)
            manifest = work / 'manifest.json'
            manifest.write_text(json.dumps(value))
            environment['LMCODE_INVOCATION_FILE'] = str(manifest)
        return subprocess.run([str(HOST), '--lmcode', *map(str, files), *flags], env=environment, capture_output=True, cwd=work)

    first = compile('first', '$return = 11;')
    second = compile('second', '$return = 22;')
    write = compile('write', 'file_write("must-not-exist.txt","x");')
    assert frame(run([first]), 0) == {'ok': True, 'result': 11}
    assert frame(run([first, second]), 0) == {'ok': True, 'result': [11, 22], 'parts': 2}
    assert not (work / 'must-not-exist.txt').exists()
    print('PASS one frame, single and multi-file result')

    assert frame(subprocess.run([str(HOST)], env=env, capture_output=True), 2)['phase'] == 'cli'
    assert frame(subprocess.run([str(HOST), '--lmcode'], env=env, capture_output=True), 2)['phase'] == 'cli'
    for name, bad in [('LMCURL_MAX_MEMORY_MB', '-5'), ('LMCURL_MAX_COST_USD', 'many'), ('LMCURL_MAX_COST_USD', '9')]:
        assert frame(run([write], extra={name: bad}), 2)['phase'] == 'cli'
    for corrupt in [lambda x: {k: v for k, v in x.items() if k != 'dry'}, lambda x: dict(x, version=2), lambda x: dict(x, extra=True)]:
        assert frame(run([write], dry=False, change=corrupt), 2)['phase'] == 'cli'
    assert frame(run([write], dry=False, flags=['--dry-run']), 2)['phase'] == 'cli'
    assert not (work / 'must-not-exist.txt').exists()
    print('PASS usage, settings and invalid metadata: exit 2 before any effect')

    assert frame(run([write], flags=['--dry-run']), 0) == {'ok': True, 'result': None, 'dry': True}
    assert frame(run([write], dry=True), 0) == {'ok': True, 'result': None, 'dry': True}
    assert not (work / 'must-not-exist.txt').exists()
    print('PASS dry flag and dry metadata load programs and run none')

    junk = work / 'bad.bin'
    junk.write_bytes(b'invalid')
    missing = work / 'missing.bin'
    for bad in (junk, missing):
        failure = frame(run([bad]), 3)
        assert failure['phase'] == 'load', failure
        failure = frame(run([write, bad]), 3)
        assert failure['phase'] == 'load' and failure['part'] == 1, failure
        assert not (work / 'must-not-exist.txt').exists()
    print('PASS every file loads before any program runs')

    broken = compile('broken', 'assert_eq(1,2);')
    failure = frame(run([first, broken, write]), 3)
    assert failure['phase'] == 'run' and failure['part'] == 1, failure
    assert not (work / 'must-not-exist.txt').exists()
    report = compile('report', 'let m=curl.mock();let suite=curl.suite("exit");'
                     'suite.test_request("bad",curl.session(m.url()).get("/status/500")).expect_status(200);'
                     'suite.report().assert_all();m.stop();')
    failure = frame(run([first, report, write]), 4)
    assert failure['phase'] == 'app' and failure['code'] == 'TEST_FAILED' and failure['part'] == 1, failure
    assert not (work / 'must-not-exist.txt').exists()
    print('PASS first failure stops the run: run error exit 3, failed report exit 4')

    returned = compile('returned', '$return = curl.env("LMCURL_TEST_SECRET");')
    result = run([returned])
    assert frame(result, 0)['result'] == '[REDACTED]', result
    thrown = compile('thrown', 'file_read(curl.env("LMCURL_TEST_SECRET"));')
    result = run([thrown])
    assert frame(result, 3)['phase'] == 'run', result
    assert SECRET.encode() not in result.stdout + result.stderr, result
    print('PASS frame results and failure messages are redacted')

    # The same behaviour through the real client path: the compiler must accept what emit_frame writes.
    source = work / 'through-lmscript.lms'
    source.write_text('p = _lmcode_ cli="lmcurl";\n$return = 7;\nend _lmcode_\nr = run_lmcode(p);\nrequire(r.ok && r.result == 7, json_stringify(r));\n'
                      'q = _lmcode_ cli="lmcurl";\nlet m=curl.mock();let suite=curl.suite("exit");'
                      'suite.test_request("bad",curl.session(m.url()).get("/status/500")).expect_status(200);'
                      'suite.report().assert_all();m.stop();\nend _lmcode_\n'
                      's = run_lmcode(q);\nrequire(!s.ok && s.phase == "app" && s.code == "TEST_FAILED", json_stringify(s));\n'
                      'println("through lmscript ok");\n')
    result = subprocess.run([COMPILER, str(source)], env=env, capture_output=True, text=True, cwd=work, timeout=60)
    assert result.returncode == 0 and 'through lmscript ok' in result.stdout, result
    # Dry replies must end with the dry marker; --dry-run exits 0 even when it refuses a frame.
    source = work / 'dry.lms'
    source.write_text('p = _lmcode_ cli="lmcurl";\n$return = 7;\nend _lmcode_\nr = run_lmcode(p);\nprintln(json_stringify(r));\n')
    result = subprocess.run([COMPILER, '--dry-run', str(source)], env=env, capture_output=True, text=True, cwd=work, timeout=60)
    assert result.returncode == 0 and 'PLAN complete' in result.stdout, result
    print('PASS lmscript accepts the redacted success, failure and dry frames')

    # Exit 4 for an output failure: the terminal frame cannot be written.
    if os.access('/dev/full', os.W_OK):
        with open('/dev/full', 'wb') as full:
            result = subprocess.run([str(HOST), '--lmcode', str(first)], env=env, stdout=full, stderr=subprocess.PIPE, cwd=work)
        assert result.returncode == 4 and b'cannot write envelope' in result.stderr, result
        print('PASS unwritable frame: exit 4')
