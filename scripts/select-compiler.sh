#!/usr/bin/env bash
# Select by what the compiler can do, not its version: implicit std and run_lmcode (checked, never executed).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/lmcurl-compiler-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
# A native (mingw) python cannot see MSYS /tmp paths: hand it drive-letter paths when cygpath exists.
native() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi; }
candidate="${LMSCRIPT_BIN:-$(command -v lmscript || true)}"
printf '%s\n' '{"dsl.schema":2,"name":"compiler_probe","types":{},"functions":{},"context":{}}' > "$WORK/probe.schema.json"
# Verify the actual source compiler capabilities below, not package metadata.
# JSON string quoting also supports checkouts whose paths contain spaces/quotes.
python3 - "$(native "$WORK/probe.schema.json")" "$(native "$WORK")" <<'PY'
import json,pathlib,sys
work=pathlib.Path(sys.argv[2])
(work/'probe.lms').write_text('p = _lmcode_ schema='+json.dumps(sys.argv[1])+';\nlet n = str_len("implicit std");\nend _lmcode_\nr = run_lmcode(p, {command: ["probe", "--lmcode", "$DSLFILE"]});\n')
PY
if [ -n "$candidate" ] &&
   "$candidate" --lmcode-signatures "$WORK/probe.schema.json" > /dev/null 2>&1 &&
   "$candidate" --check "$WORK/probe.lms" > /dev/null 2>&1
then
  printf '%s\n' "$candidate"
else
  if [ -n "${LMCURL_DEBUG:-}" ]; then
    echo "--- compiler probe diagnostics (candidate=$candidate)" >&2
    cat "$WORK/probe.lms" >&2 || true
    [ -n "$candidate" ] && "$candidate" --lmcode-signatures "$WORK/probe.schema.json" 2>&1 | head -3 >&2
    [ -n "$candidate" ] && "$candidate" --check "$WORK/probe.lms" 2>&1 | head -10 >&2
    ls -la "$WORK" >&2
  fi
  echo "error: no run_lmcode compiler found; set LMSCRIPT_BIN to a private @lmctl-ai/lmctl@0.2.460 installation" >&2
  exit 2
fi
