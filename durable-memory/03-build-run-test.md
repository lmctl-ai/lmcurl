# Build, run, test

- **Toolchain facts:** C++17, g++ 11, POSIX sockets, OpenSSL 3 (optional, auto-detected by the Makefile), node (mock server only),
  curl (tests), lmscript >= 0.2.328 from npm `@lmctl-ai/lmctl` (`latest` 0.2.328 / `staging` 0.2.330 on 2026-10-18). `test.sh` takes
  `lmscript` from PATH or `LMSCRIPT_BIN` and refuses older versions.
- **Commands:** `make` (build/curlhost; prints whether TLS was linked via the Makefile's `-DHAVE_OPENSSL`); `node tools/mock-server.mjs
  8080 &`; `lmscript examples/post.lms build/post.dslb http://127.0.0.1:8080/echo`; `build/curlhost --dsl build/post.dslb --schema
  schema/curl.dsl.json`; `POC_CURL_KEY=test-key build/curlhost --dsl build/llm.dslb ...`; `bash test.sh` (25 checks with a mono lmscript via `LMSCRIPT_MIN=0
  LMSCRIPT_BIN=...`, 24 with released 0.2.328 which lacks `--dsl-signatures`; that check prints `skip -`).
- **Mock server endpoints** (`tools/mock-server.mjs`, deterministic): `/echo` (JSON echo of method/headers/body), `/v1/chat/completions`
  (requires `Authorization: Bearer test-key`; replies `Echo: <last user message>`; 401 otherwise), `/go` (302), `/chunked`, `/big`
  (1 MB), `/slow` (3 s), `/site/...` (10-page static site with a loop, an off-site link, a 404 link, a depth chain, CSS, a PNG).
- **Exact expectations** (stdout is one JSON line; `out.print` lines are `result.output`; the test's `out_lines` unpacks them):
  POST echo status 200 with both headers and the exact body, `"requests": 1, "mirrors": 0`; LLM mock `model: mock-mini`, `reply:
  Echo: Say hello in five words.`, `tokens: 54`; missing key -> `{"ok": false, "phase": "app", "code": "APP_FAIL", "message": "set
  POC_CURL_KEY ..."}` exit 4 without any request; wrong key -> APP_FAIL `API error 401: invalid api key` (stderr `fail: ...`), exit 4; mirror depth 2 -> exactly 8 files (`a b d1 d2 dir/index dir/page index style.css` under `127.0.0.1_PORT/site/`), summary
  `pages 9 skipped 2 failed 1 bytes 887`, d3 never fetched, off-site skipped, PNG skipped by content type; chunked body
  `first second third`; `max_redirects(0)` returns the 302 with its Location; size limit and timeout are `phase run` RUN_HOST lines
  (`Request.send GET <url>: ...`), exit 3; schema mismatch / truncation / version 2 are `phase load` lines, exit 3.
- **Live LLM call:** opt-in only, `POC_CURL_LIVE=1` plus `OPENAI_API_KEY` (target gpt-4o-mini, expects "pong"); never run by default
  and never run so far (no key was present on this box).
