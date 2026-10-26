#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 "$ROOT/scripts/test-examples.py" --host "${LMCURL_BIN:-$ROOT/build/lmcurl}" --render
