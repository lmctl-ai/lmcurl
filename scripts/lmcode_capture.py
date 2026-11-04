"""Program files from the real compiler: lmscript --dry-run hands the private file to a recording command."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def capture(compiler, host, source, output, env, cwd):
    """Run SOURCE (it must call run_lmcode with record_command) dry; the host loads it and runs nothing."""
    result = subprocess.run([compiler, '--dry-run', str(source)], cwd=cwd, env=env, capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, 'compile failed: ' + result.stdout + result.stderr
    # --dry-run exits 0 even when it could not read the host's frame.
    assert 'needs-real-result' not in result.stdout, 'dry frame refused: ' + result.stdout + result.stderr
    data = Path(output).read_bytes()
    assert data[:6] == b'DSLB\x00\x06', 'not bytecode v6'
    assert data[6:38] == hashlib.sha256((ROOT / 'schema/lmcurl.dsl.json').read_bytes()).digest(), 'schema hash differs from the raw host schema bytes'
    return Path(output)


def record_command(host, output):
    return json.dumps([sys.executable, str(ROOT / 'scripts/record-program.py'), str(host), str(output), '--lmcode', '$DSLFILE'], ensure_ascii=False)


def program(compiler, host, body, output, env, cwd):
    """The program file for one lmcurl block."""
    output = Path(output)
    source = output.with_suffix('.lms')
    source.write_text('p = _lmcode_ schema=' + json.dumps(str(ROOT / 'schema/lmcurl.dsl.json')) + ';\n' + body + '\nend _lmcode_\nr = run_lmcode(p, {command: ' + record_command(host, output) +
                      '});\nrequire(r.ok, json_stringify(r));\n', encoding='utf-8')
    return capture(compiler, host, source, output, env, cwd)
