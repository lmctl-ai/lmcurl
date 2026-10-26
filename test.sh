#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
BUILD_DIR="${BUILD_DIR:-build}"
export LMCURL_LIVE=0
case "$(uname -s)" in Darwin|MINGW*|MSYS*|CYGWIN*)
 cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
 cmake --build "$BUILD_DIR" --parallel 2 >/dev/null
 exe="$BUILD_DIR/lmcurl"; [[ -f "$exe.exe" ]] && exe="$exe.exe"
 [[ "$("$exe" --version)" == 'lmcurl 0.1.0' ]]
 "$exe" --help | grep -q 'stub'
 "$exe" --dsl-schema --json | python3 -c 'import json,sys;assert json.load(sys.stdin)["name"]=="lmcurl"'
 set +e; "$exe" --dsl not-present.dslb 2>"$BUILD_DIR/stub.err"; status=$?; set -e
 [[ "$status" == 3 ]]; grep -q 'not supported' "$BUILD_DIR/stub.err"
 echo 'PASS: stub metadata and explicit unsupported execution (4 checks)'; exit 0;;
esac
if ! scripts/sync-deps.sh --verify >/dev/null 2>&1; then timeout 600 scripts/sync-deps.sh; fi
scripts/sync-sdk.sh --verify
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release >"/tmp/lmcurl-test-config.log" 2>&1
cmake --build "$BUILD_DIR" --parallel "${BUILD_JOBS:-2}" >"/tmp/lmcurl-test-build.log" 2>&1
exe="$(cd "$BUILD_DIR" && pwd)/lmcurl"
export PATH="$(dirname "$exe"):$PATH"
compiler="$(scripts/select-compiler.sh)"
[[ "$("$exe" --version)" == 'lmcurl 0.1.0' ]]
"$exe" --notices >"$BUILD_DIR/notices.check"; cmp THIRD_PARTY_NOTICES "$BUILD_DIR/notices.check"
python3 scripts/gen-schema.py "$exe" >"$BUILD_DIR/schema.check"; cmp schema/lmcurl.dsl.json "$BUILD_DIR/schema.check"
"$exe" --dsl-schema --json >"$BUILD_DIR/embedded.check"; cmp schema/lmcurl.dsl.json "$BUILD_DIR/embedded.check"
diff -u <("$compiler" --dsl-signatures schema/lmcurl.dsl.json) <("$exe" --dsl-signatures)
python3 scripts/check-linux-linkage.py "$exe"
mkdir -p build/tests
for source in tests/*.lms; do timeout 60 "$compiler" "$source"; done
timeout 60 scripts/test-memory-budget.sh
python3 scripts/test-native.py --host "$exe"
if [[ "${LMCURL_TEST_NETWORK:-0}" == 1 ]] || command -v lmplot >/dev/null; then
 timeout 1500 python3 scripts/test-examples.py --host "$exe" --render
else
 python3 scripts/test-examples.py --host "$exe"
 echo 'SKIP LOUDLY: example execution/images not regenerated; set LMCURL_TEST_NETWORK=1 or put lmplot on PATH'
fi
# Default 4096 MiB budget checked separately from the small native regression.
cat >"$BUILD_DIR/default-budget.lms" <<'LMS'
p = _dsl_ cli="lmcurl";
let s=curl.session("http://127.0.0.1:1");
for(i in range(0,100000)){s.get("/").body(str_repeat("x",1000000));}
end _dsl_
savedsl(p,"build/tests/default-budget.dslb");
LMS
"$compiler" "$BUILD_DIR/default-budget.lms"
set +e
(ulimit -v 6291456; timeout 180 "$exe" --dsl build/tests/default-budget.dslb) >"$BUILD_DIR/budget.out" 2>"$BUILD_DIR/budget.err"; status=$?
set -e
[[ "$status" == 3 ]]; grep -q 'memory budget exceeded' "$BUILD_DIR/budget.err"; grep -q 'LMCURL_MAX_MEMORY_MB' "$BUILD_DIR/budget.err"
python3 scripts/check-secrets.py --artifacts
echo 'PASS: pins, static dependencies/dynamic glibc, exact schema/notices, four DSL suites, 28 native scenarios, gallery and default memory budget'
