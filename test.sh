#!/usr/bin/env bash
# End-to-end test of poc-curl. Needs g++ (or CXX), make, node (for the local mock server) and the released lmscript CLI
# (`npm install -g @lmctl-ai/lmctl`, >= 0.2.328; LMSCRIPT_BIN overrides the binary).
# Everything runs against the local mock; one LIVE LLM call happens only when POC_CURL_LIVE=1 and OPENAI_API_KEY are both set.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LMSCRIPT="${LMSCRIPT_BIN:-lmscript}"   # the released CLI on PATH (npm install -g @lmctl-ai/lmctl); override with LMSCRIPT_BIN
MIN_LMSCRIPT="${LMSCRIPT_MIN:-0.2.328}"  # first release with _dsl_ / savedsl; LMSCRIPT_MIN=0 admits a mono build
if ! command -v "$LMSCRIPT" >/dev/null 2>&1 && [ ! -f "$LMSCRIPT" ]; then echo "FAIL: lmscript not found ($LMSCRIPT). Install the released CLI: npm install -g @lmctl-ai/lmctl (>= $MIN_LMSCRIPT), or set LMSCRIPT_BIN." >&2; exit 1; fi
HAVE_LMSCRIPT="$("$LMSCRIPT" --version 2>/dev/null | sed -n 's/^lmscript //p')"
if [ -z "$HAVE_LMSCRIPT" ] || [ "$(printf '%s\n%s\n' "$MIN_LMSCRIPT" "$HAVE_LMSCRIPT" | sort -V | head -1)" != "$MIN_LMSCRIPT" ]; then echo "FAIL: lmscript ${HAVE_LMSCRIPT:-unknown} at $LMSCRIPT is too old: _dsl_ needs >= $MIN_LMSCRIPT (npm install -g @lmctl-ai/lmctl@latest) or set LMSCRIPT_BIN." >&2; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/poc-curl-test-XXXXXX")"
export HOME="$WORK/home"; mkdir -p "$HOME"
MOCK_PID=""
fail() { echo "FAIL: $*" >&2; exit 1; }
pass=0; ok() { pass=$((pass + 1)); echo "ok $pass - $*"; }
cleanup() { if [ -n "$MOCK_PID" ] && kill -0 "$MOCK_PID" 2>/dev/null; then kill -TERM "$MOCK_PID"; wait "$MOCK_PID" 2>/dev/null; fi; rm -rf "$WORK"; }
trap cleanup EXIT
SCHEMA="$HERE/schema/curl.dsl.json"
compile() { (cd "$HERE" && "$LMSCRIPT" "$@") ; }

# 0. The vendored runtime must be the pinned cpp-dsl-sdk header.
"$HERE/scripts/sync-sdk.sh" --verify >/dev/null || fail "cpp/dslvm.hpp differs from sdk.lock (run scripts/sync-sdk.sh)"
ok "cpp/dslvm.hpp matches sdk.lock ($(sed -n 's/^commit=//p' "$HERE/sdk.lock" | cut -c1-12))"

# 1. Build the host; start the mock.
make -C "$HERE" BUILD="$WORK/build" >"$WORK/make.log" 2>&1 || { cat "$WORK/make.log"; fail "C++ build"; }
HOST="$WORK/build/curlhost"
grep -q -- "-DHAVE_OPENSSL" "$WORK/make.log" && TLS=yes || TLS=no
ok "C++ host builds (TLS via OpenSSL: $TLS)"
# 1b. Registry versus schema (spec v4 S0, A02): the host's declared signatures must equal the schema's.
if "$LMSCRIPT" --dsl-signatures "$SCHEMA" >"$WORK/schema.sigs" 2>/dev/null; then
  "$HOST" --dsl-signatures >"$WORK/host.sigs" || fail "host --dsl-signatures"
  diff -u "$WORK/schema.sigs" "$WORK/host.sigs" >"$WORK/sigs.diff" || { cat "$WORK/sigs.diff"; fail "registry signatures differ from the schema"; }
  ok "registry signatures equal the schema's ($(wc -l <"$WORK/host.sigs") entries, lmscript --dsl-signatures)"
else
  echo "skip - lmscript $HAVE_LMSCRIPT has no --dsl-signatures yet (lands in the release after 0.2.330); the host declares $("$HOST" --dsl-signatures | wc -l) signatures"
fi
# stdout is exactly one JSON result line (spec v4 S0, A03); out.print lines travel in result.output, in order.
out_lines() { python3 -I -c 'import json,sys; d=json.load(sys.stdin); assert d["ok"] is True, d; print("\n".join(d["result"]["output"]))'; }
node "$HERE/tools/mock-server.mjs" 0 >"$WORK/mock.out" 2>&1 &
MOCK_PID=$!
PORT=""; for _ in $(seq 1 50); do PORT="$(sed -n 's/^listening on //p' "$WORK/mock.out")"; [ -n "$PORT" ] && break; sleep 0.1; done
[ -n "$PORT" ] || fail "mock server did not start: $(cat "$WORK/mock.out")"
BASE="http://127.0.0.1:$PORT"
ok "mock server listens on $PORT"

# 2. post.lms: one script file does a JSON POST; the echo proves method, headers and the exact body.
compile examples/post.lms "$WORK/post.dslb" "$BASE/echo" >"$WORK/c.out" 2>&1 || { cat "$WORK/c.out"; fail "compile post.lms"; }
[ "$(od -An -tu1 -j4 -N2 "$WORK/post.dslb" | tr -s ' ' | sed 's/^ //')" = "0 5" ] || fail "bytecode version 5"
RAW="$("$HOST" --dsl "$WORK/post.dslb" --schema "$SCHEMA" 2>"$WORK/post.err")"; code=$?
echo "$RAW" | sed 's/^/    /'
[ "$code" = 0 ] || { cat "$WORK/post.err"; fail "post host exit $code"; }
[ "$(echo "$RAW" | wc -l)" = 1 ] || fail "stdout must be exactly one JSON line"
case "$RAW" in '{"ok": true, "result": {"output": ['*'], "requests": 1, "mirrors": 0}}') ;; *) fail "result line shape: $RAW";; esac
OUT="$(echo "$RAW" | out_lines)"
[ "$(echo "$OUT" | sed -n 1p)" = "status 200 ok=true" ] || fail "post status line"
[ "$(echo "$OUT" | sed -n 2p)" = "content-type: application/json" ] || fail "post content-type line"
echo "$OUT" | sed -n 3p | node -e '
const r=JSON.parse(require("fs").readFileSync(0,"utf8"));
if(r.method!=="POST")throw Error("method "+r.method);
if(r.headers["content-type"]!=="application/json"||r.headers["x-trace"]!=="post-example")throw Error("headers "+JSON.stringify(r.headers));
const body=JSON.parse(r.body);if(body.name!=="poc-curl"||body.answer!==42||body.quote!=="he said \"hi\""||body.tags.join()!=="dsl,post")throw Error("body "+r.body);
if(r.headers["content-length"]!==String(Buffer.byteLength(r.body)))throw Error("content-length");' || fail "echoed request differs"
ok "post.lms: POST /echo with JSON body and two headers, status 200, exact body echoed"

# 3. llm.lms against the mock: key from the host environment, reply extracted by dotted JSON path; no key / wrong key fail cleanly.
compile examples/llm.lms "$WORK/llm.dslb" "$BASE/v1/chat/completions" "mock-mini" "Say hello in five words." POC_CURL_KEY >/dev/null 2>&1 || fail "compile llm.lms"
grep -q "test-key" "$WORK/llm.dslb" && fail "the API key must not be in the bytecode"
RAW="$(POC_CURL_KEY=test-key "$HOST" --dsl "$WORK/llm.dslb" --schema "$SCHEMA" 2>/dev/null)"; code=$?
echo "$RAW" | sed 's/^/    /'
[ "$code" = 0 ] || fail "llm host exit $code"
OUT="$(echo "$RAW" | out_lines)"
[ "$OUT" = "model: mock-mini
reply: Echo: Say hello in five words.
tokens: 54" ] || fail "llm output: $OUT"
ok "llm.lms (mock): Authorization from the host env, reply and usage extracted with json_text paths"
RAW="$(env -u POC_CURL_KEY "$HOST" --dsl "$WORK/llm.dslb" --schema "$SCHEMA" 2>"$WORK/nokey.err")"; code=$?
[ "$code" = 4 ] && grep -q '^fail: set POC_CURL_KEY in the host environment' "$WORK/nokey.err" || fail "missing key: exit $code: $(cat "$WORK/nokey.err")"
case "$RAW" in '{"ok": false, "phase": "app", "code": "APP_FAIL", "message": "set POC_CURL_KEY in the host environment'*) ;; *) fail "missing key result line: $RAW";; esac
ok "llm.lms without the key: no request is sent, out.fail -> one JSON line, phase app, APP_FAIL, exit 4"
RAW="$(POC_CURL_KEY=wrong "$HOST" --dsl "$WORK/llm.dslb" --schema "$SCHEMA" 2>"$WORK/wrongkey.err")"; code=$?
[ "$code" = 4 ] && grep -q '^fail: API error 401: invalid api key' "$WORK/wrongkey.err" || fail "wrong key: exit $code: $(cat "$WORK/wrongkey.err")"
[ "$RAW" = '{"ok": false, "phase": "app", "code": "APP_FAIL", "message": "API error 401: invalid api key"}' ] || fail "wrong key result line: $RAW"
ok "llm.lms with a wrong key: the API error message is the APP_FAIL result line (401), exit 4"
if [ "${POC_CURL_LIVE:-}" = 1 ] && [ -n "${OPENAI_API_KEY:-}" ]; then
  compile examples/llm.lms "$WORK/live.dslb" "https://api.openai.com/v1/chat/completions" "gpt-4o-mini" "Reply with the single word pong." OPENAI_API_KEY >/dev/null 2>&1 || fail "compile live llm"
  RAW="$("$HOST" --dsl "$WORK/live.dslb" --schema "$SCHEMA" 2>"$WORK/live.err")"; code=$?
  echo "$RAW" | sed 's/^/    /'
  [ "$code" = 0 ] && echo "$RAW" | out_lines | grep -qi '^reply: .*pong' || { cat "$WORK/live.err"; fail "live llm call"; }
  ok "LIVE llm call (POC_CURL_LIVE=1): gpt-4o-mini answered"
else
  echo "skip - live LLM call (set POC_CURL_LIVE=1 and OPENAI_API_KEY to run it; it costs money)"
fi

# 4. mirror.lms: the host crawls, the DSL configured it. Exact file set, depth limit, loop, off-site, 404, content types, safe paths.
rm -rf "$WORK/mirror"
compile examples/mirror.lms "$WORK/mirror.dslb" "$BASE/site/index.html" "$WORK/mirror" 2 >/dev/null 2>&1 || fail "compile mirror.lms"
RAW="$("$HOST" --dsl "$WORK/mirror.dslb" --schema "$SCHEMA" 2>"$WORK/mirror.err")"; code=$?
echo "$RAW" | sed 's/^/    /'
[ "$code" = 0 ] || { cat "$WORK/mirror.err"; fail "mirror host exit $code"; }
OUT="$(echo "$RAW" | out_lines)"
[ "$OUT" = "pages 9 skipped 2 failed 1 bytes 887" ] || fail "mirror summary: $OUT"
FILES="$(cd "$WORK/mirror" && find . -type f | sort)"
[ "$FILES" = "./127.0.0.1_$PORT/site/a.html
./127.0.0.1_$PORT/site/b.html
./127.0.0.1_$PORT/site/d1.html
./127.0.0.1_$PORT/site/d2.html
./127.0.0.1_$PORT/site/dir/index.html
./127.0.0.1_$PORT/site/dir/page.html
./127.0.0.1_$PORT/site/index.html
./127.0.0.1_$PORT/site/style.css" ] || fail "mirrored files:
$FILES"
ok "mirror.lms: exactly 8 files under host_port/; d3 beyond depth 2 not fetched; dir/ -> index.html; css kept, png skipped"
grep -q "skip http://offsite.invalid/x.html: other host" "$WORK/mirror.err" || fail "off-site link must be skipped"
grep -q "fail $BASE/site/missing.html: status 404" "$WORK/mirror.err" || fail "404 counted as failed"
grep -q "skip $BASE/site/img/logo.png: content-type image/png" "$WORK/mirror.err" || fail "disallowed content type skipped"
[ "$(grep -c "saved $BASE/site/a.html" "$WORK/mirror.err")" = 2 ] || fail "a.html fetched once plain and once with ?v=2 (the loop a<->b is not refetched)"
ok "mirror log: off-site skipped, 404 failed, image/png skipped, loop not refetched"
[ "$(cat "$WORK/mirror/127.0.0.1_$PORT/site/style.css")" = "body { color: #333; }" ] || fail "css content"
grep -q "Depth 2" "$WORK/mirror/127.0.0.1_$PORT/site/d2.html" || fail "d2 content"
ok "mirrored file contents are the served bytes"
find "$WORK" -path "$WORK/mirror" -prune -o -name "*.html" -newer "$WORK/mirror.dslb" -print | grep -v "^$WORK/mirror/" | grep -q . && fail "a file was written outside the output directory"
ok "no file written outside the output directory (path traversal via ../ resolves inside the site)"

# 5. HTTP client behaviors through a probe chunk: chunked body, redirect followed, 404 status, size limit, timeout.
cat >"$WORK/probe.lms" <<LMS
p = _dsl_ schema="$SCHEMA";
let a = request().url("$BASE/chunked").send();
out.print("chunked: " + a.body());
let b = request().url("$BASE/go").send();
out.print("redirect: " + str(b.status()) + " " + b.header("Content-Type"));
let c = request().url("$BASE/nope").send();
out.print("missing: " + str(c.status()) + " " + c.body() + " ok=" + str(c.ok()));
let d = request().url("$BASE/go").max_redirects(0).send();
out.print("no-follow: " + str(d.status()) + " " + d.header("Location"));
end _dsl_
savedsl(p, "$WORK/probe.dslb");
LMS
compile "$WORK/probe.lms" >/dev/null 2>&1 || fail "compile probe"
OUT="$("$HOST" --dsl "$WORK/probe.dslb" --schema "$SCHEMA" 2>/dev/null | out_lines)" || fail "probe host"
[ "$OUT" = "chunked: first second third
redirect: 200 text/html
missing: 404 not found ok=false
no-follow: 302 /site/index.html" ] || fail "probe output: $OUT"
ok "client: chunked body reassembled, 302 followed (and not followed with max_redirects(0)), 404 reported"
printf 'p = _dsl_ schema="%s";\nlet d = request().url("%s/big").max_bytes(1000).send();\nend _dsl_\nsavedsl(p, "%s/big.dslb");\n' "$SCHEMA" "$BASE" "$WORK" >"$WORK/big.lms"; compile "$WORK/big.lms" >/dev/null 2>&1 || fail "compile big"
RAW="$("$HOST" --dsl "$WORK/big.dslb" --schema "$SCHEMA" 2>"$WORK/big.err")"; code=$?
[ "$code" = 3 ] && grep -q 'response of 1000000 bytes exceeds the size limit of 1000' "$WORK/big.err" || fail "size limit: $code $(cat "$WORK/big.err")"
case "$RAW" in '{"ok": false, "phase": "run", "code": "RUN_HOST", "message": "Request.send GET '*'response of 1000000 bytes exceeds the size limit of 1000"}') ;; *) fail "size limit result line: $RAW";; esac
ok "client: size limit refuses a 1 MB body before reading it (phase run, RUN_HOST, exit 3)"
printf 'p = _dsl_ schema="%s";\nlet s = request().url("%s/slow").timeout_ms(300).send();\nend _dsl_\nsavedsl(p, "%s/slow.dslb");\n' "$SCHEMA" "$BASE" "$WORK" >"$WORK/slow.lms"; compile "$WORK/slow.lms" >/dev/null 2>&1 || fail "compile slow"
RAW="$("$HOST" --dsl "$WORK/slow.dslb" --schema "$SCHEMA" 2>"$WORK/slow.err")"; code=$?
[ "$code" = 3 ] && grep -q 'receive failed or timed out' "$WORK/slow.err" || fail "timeout: $code $(cat "$WORK/slow.err")"
case "$RAW" in '{"ok": false, "phase": "run", "code": "RUN_HOST", "message": "Request.send GET '*'receive failed or timed out'*) ;; *) fail "timeout result line: $RAW";; esac
ok "client: timeout_ms(300) aborts a 3 s response (exit 3)"

# 6. Ill-typed chunks are refused at lmscript check time with located errors, before anything runs.
check_rejects() { # $1 body, $2 expected "LINE:COL: message", $3 label
  printf 'program = _dsl_ schema="%s";\n%s\nend _dsl_\nsavedsl(program, "%s/never-%s.dslb");\n' "$SCHEMA" "$1" "$WORK" "$3" >"$WORK/bad-$3.lms"
  local out code; out="$("$LMSCRIPT" --check "$WORK/bad-$3.lms" 2>&1)"; code=$?
  [ "$code" = 2 ] || fail "$3: --check exit $code: $out"
  [ "$out" = "error:$WORK/bad-$3.lms:$2" ] || fail "$3:
  got:  $out
  want: error:$WORK/bad-$3.lms:$2"
  [ ! -e "$WORK/never-$3.dslb" ] || fail "$3: savedsl ran"
  ok "rejected at check: $3 ($2)"
}
check_rejects 'let r = request().url("http://h/").send();
out.print("status " + r.status());' "3:23: _dsl_: '+' on a string needs a string on the right; found int (convert with str(x))" 'concat-int'
check_rejects 'out.print(str("x"));' '2:15: _dsl_: built-in str expects int, float or bool; found string (already a string)' 'str-of-string'
check_rejects 'let r = request().url("http://h/").send();
out.print(r.json_text(1));' '3:23: _dsl_: method Response.json_text expects string for argument 1; found int' 'json-path-type'
check_rejects 'let m = mirror().depth("2");' '2:24: _dsl_: method Mirror.depth expects int for argument 1; found string' 'depth-string'
check_rejects 'let m = mirror().start("http://h/");
out.print(str(m.run()));' '3:15: _dsl_: built-in str expects int, float, bool or enum; found object MirrorResult' 'str-of-handle'
check_rejects 'let r = request().url("http://h/");
out.print(r.status());' "3:13: _dsl_: type Request has no method 'status' (methods: method, url, header, body, timeout_ms, max_bytes, max_redirects, send)" 'response-method-on-request'
check_rejects 'out.print(env());' '2:14: _dsl_: function env expects 1 argument(s) (string); found 0' 'env-arity'

# 7. The host refuses a schema mismatch, a truncated file and the older v4 bytecode version.
sed 's/"name": "curl"/"name": "curl2"/' "$SCHEMA" >"$WORK/other.json"
printf 'p = _dsl_ schema="%s";\nout.print("x");\nend _dsl_\nsavedsl(p, "%s/other.dslb");\n' "$WORK/other.json" "$WORK" >"$WORK/other.lms"; compile "$WORK/other.lms" >/dev/null 2>&1 || fail "compile other"
out="$("$HOST" --dsl "$WORK/other.dslb" --schema "$SCHEMA" 2>/dev/null)"; code=$?
[ "$code" = 3 ] || fail "schema mismatch exit $code (host failures exit 3)"
case "$out" in '{"ok": false, "phase": "load", "code": "LOAD_SCHEMA_MISMATCH", "message": "schema mismatch'*) ;; *) fail "mismatch result line: $out";; esac
ok "host refuses a schema-hash mismatch before running: one JSON line, phase load, LOAD_SCHEMA_MISMATCH, exit 3"
head -c 60 "$WORK/post.dslb" >"$WORK/trunc.dslb"; out="$("$HOST" --dsl "$WORK/trunc.dslb" --schema "$SCHEMA" 2>/dev/null)"; [ $? = 3 ] || fail "truncated exit"
case "$out" in *'"code": "LOAD_BAD_BYTECODE", "message": "bytecode is truncated'*) ;; *) fail "truncated result line: $out";; esac
ok "host refuses a truncated file (LOAD_BAD_BYTECODE, exit 3)"
{ printf 'DSLB\000\004'; tail -c +7 "$WORK/post.dslb"; } >"$WORK/v4.dslb"; out="$("$HOST" --dsl "$WORK/v4.dslb" --schema "$SCHEMA" 2>/dev/null)"; [ $? = 3 ] || fail "version exit"
case "$out" in *'"code": "LOAD_BAD_BYTECODE", "message": "unsupported bytecode version 4 (this runtime reads version 5)"'*) ;; *) fail "version result line: $out";; esac
ok "host refuses bytecode version 4 (no backward compatibility)"
echo "PASS $pass checks"
