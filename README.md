# poc-curl: a curl-like C++ HTTP host driven by the DSL

Status: EXPERIMENTAL proof of concept for the lmscript `_dsl_` framework (lmctl-mono `lmscript/durable-memory/requirements/dsl-bytecode.md`
R12-R16, R20, R22; living language spec `lmscript/durable-memory/design/dsl-language.md`, bytecode v3). Nothing here is an accepted
design. Run on 2026-10-18 with g++ 11, Node 26, OpenSSL 3 and a mono build of lmscript.

**lmscript build needed.** `_dsl_` and `savedsl` are not in the npm staging build 0.2.311. Use a mono build:
`LMSCRIPT_BIN=~/repos/lmctlhq/lmctl-mono/lmscript/bin/lmscript` (default in `test.sh`: `../lmctl-mono/lmscript/bin/lmscript`;
build it with `npm run build` in `lmctl-mono/lmscript`).

## What it proves

- **One script file does a POST (R22).** `examples/post.lms` builds the JSON body with lmscript's `json_stringify`, hands it to
  the DSL as a `${}` literal, and the chunk configures the host's request builder: method, URL, headers, timeout, body, send,
  print. No shell quoting, no long command line, and the chunk is type-checked before anything runs.
- **An LLM call without the key in the script (R22).** `examples/llm.lms` takes URL, model and prompt as `${}` literals and
  reads the API key from the host environment with `env("POC_CURL_KEY")` inside the chunk, so the secret is in neither the
  script text nor the bytecode (the test greps the bytecode). The reply is extracted by the host (`json_text("choices.0.message.content")`).
  The test runs against a local mock; a live call to `gpt-4o-mini` is opt-in (`POC_CURL_LIVE=1` plus `OPENAI_API_KEY`).
- **The DSL configures a crawler instead of looping (R13).** `examples/mirror.lms` sets start URL, depth, same-host rule,
  allowed content types, output directory and limits on a host-provided `Mirror` object and runs it; the host does the
  breadth-first crawl, link extraction, URL resolution and safe local paths. Exact file set asserted by the test.
- **R12 again.** The host starts, builds its registry, loads and verifies the bytecode, runs the chunk once with `out` as
  context, then flushes and exits 0 (ok), 1 (load refused), 3 (DSL run-time error from the host) or 4 (`out.fail`).
- **R20, one run instead of many launches.** See "Processes" below.

## Run it

    make                                                        # build/curlhost (C++17, POSIX sockets; HTTPS via OpenSSL when its dev files exist: yes here)
    node tools/mock-server.mjs 8080 &                           # deterministic local endpoints
    $LMSCRIPT_BIN examples/post.lms build/post.dslb http://127.0.0.1:8080/echo
    build/curlhost --dsl build/post.dslb --schema schema/curl.dsl.json
    $LMSCRIPT_BIN examples/llm.lms build/llm.dslb http://127.0.0.1:8080/v1/chat/completions mock-mini "Say hello in five words." POC_CURL_KEY
    POC_CURL_KEY=test-key build/curlhost --dsl build/llm.dslb --schema schema/curl.dsl.json
    $LMSCRIPT_BIN examples/mirror.lms build/mirror.dslb http://127.0.0.1:8080/site/index.html build/mirror 2
    build/curlhost --dsl build/mirror.dslb --schema schema/curl.dsl.json

Transcript (`bash test.sh`, 23 checks against the mock):

    ok 3 - post.lms: POST /echo with JSON body and two headers, status 200, exact body echoed
        status 200 ok=true
        content-type: application/json
        {"method":"POST","path":"/echo","headers":{...,"content-type":"application/json","x-trace":"post-example","content-length":"78"},"body":"{\"name\":\"poc-curl\",...}"}
    ok 4 - llm.lms (mock): Authorization from the host env, reply and usage extracted with json_text paths
        model: mock-mini
        reply: Echo: Say hello in five words.
        tokens: 54
    ok 5 - llm.lms without the key: no request is sent, out.fail -> exit 4
    ok 6 - llm.lms with a wrong key: the API error message is reported (401), exit 4
    skip - live LLM call (set POC_CURL_LIVE=1 and OPENAI_API_KEY to run it; it costs money)
    ok 7 - mirror.lms: exactly 8 files under host_port/; d3 beyond depth 2 not fetched; dir/ -> index.html; css kept, png skipped
        pages 9 skipped 2 failed 1 bytes 887
    ok 11 - client: chunked body reassembled, 302 followed (and not followed with max_redirects(0)), 404 reported
    ok 12 - client: size limit refuses a 1 MB body before reading it (exit 3)
    ok 13 - client: timeout_ms(300) aborts a 3 s response (exit 3)
    ok 14 ... ok 20 - seven ill-typed chunks rejected at lmscript check time with located errors
    ok 21 ... ok 23 - host refuses a schema-hash mismatch, a truncated file and bytecode version 2
    PASS 23 checks

The mirror chunk, the one that replaces a loop with configuration:

    let site = mirror().start(${start}).depth(${depth}).same_host(true).allow("text/html").allow("text/css")
      .out_dir(${out_dir}).max_pages(50).max_bytes(2000000).delay_ms(0);
    let result = site.run();
    out.print("pages " + str(result.pages()) + " skipped " + str(result.skipped()) + " failed " + str(result.failed()) + " bytes " + str(result.bytes()));

## Processes (R20)

| Task | curl equivalent | poc-curl |
| --- | --- | --- |
| POST with JSON body and two headers | 1 `curl` (plus the shell quoting of the body, or a body file written by a second process) | 1 host process (plus 1 `lmscript` compile, reusable) |
| LLM call with extraction | `curl` + `jq` (2 processes, the key in the command line or a header file) | 1 host process; the key never leaves the environment |
| Mirror of 9 pages, depth 2 | `wget -r -l2 -np` (1 process, its own crawler) or a script spawning 9+ `curl`s | 1 host process; the crawler is a host object the chunk configured |

One DSL run replaces the per-request process launches and the shell-quoting layer; the compile step is a separate lmscript run
but its output is a file that can be reused, and the same bytecode runs with different environments (different key, same
chunk).

## Files

| File | Role |
| --- | --- |
| `schema/curl.dsl.json` | the contract: `Request` builder (method, url, header, body, timeout_ms, max_bytes, max_redirects, send), `Response` (status, ok, body, header, has_json, json_text), `Mirror` (start, depth, same_host, allow, out_dir, max_pages, max_bytes, delay_ms, run), `MirrorResult`, `Output` (print, write_file, fail); functions `request`, `mirror`, `env`, `has_env`, `json_escape`; context `out` |
| `cpp/http.hpp` | HTTP/1.1 client over POSIX sockets: Connection: close, Content-Length and chunked bodies, bounded redirects, timeouts, size limits, URL parsing and RFC 3986-lite resolution, TLS through OpenSSL under `HAVE_OPENSSL` |
| `cpp/curlhost.cpp` | the host: registry, a small JSON parser with dotted-path lookup, the crawler (same-host, content types, safe local paths, loop and depth control), load, run, exit codes |
| `cpp/dslvm.hpp` | the shared runtime, vendored from [lmctlhq/cpp-dsl-sdk](https://github.com/lmctlhq/cpp-dsl-sdk) and pinned by `sdk.lock` (commit + sha256; `scripts/sync-sdk.sh` refreshes it from a sibling checkout, `test.sh` fails on drift); bytecode v3 |
| `tools/mock-server.mjs` | node mock: `/echo`, `/v1/chat/completions`, `/go` (302), `/chunked`, `/big`, `/slow`, a static site with a loop, an off-site link, a 404 link, a depth chain, CSS and a PNG |
| `examples/post.lms`, `examples/llm.lms`, `examples/mirror.lms` | the three scripts |
| `test.sh` | end-to-end test (LMSCRIPT_BIN); first check: the vendored header matches `sdk.lock` |
| `sdk.lock`, `scripts/sync-sdk.sh` | pin and refresh of the cpp-dsl-sdk header |

## Decisions (confirm or change)

1. **Language additions (spec v3): string concatenation and `str(x)`.** `"Bearer " + env(KEY)` cannot be pre-built by
   lmscript because the secret must come from the host, so `+` on two strings was unavoidable; `str()` of an int, float or
   bool is the only way to put a number into text (nothing implicit: `"a" + 1` is an error with the hint). Both are the
   configuration-chunk minimum (R13). Not added: lists, maps, optional arguments, loops, functions.
2. **Builder objects instead of optional arguments.** `request().method(...).url(...).header(k, v).send()`: a method returning its
   own type costs the language nothing (poc-numpy already had handle-returning methods) and makes every option named and
   typed. Repeated `header`/`allow` calls replace lists. Validation happens in the host method (`Request.method` rejects an
   unknown verb, `header` rejects CR/LF, `Mirror.depth` is 0..10) and surfaces as a DSL run-time error (exit 3).
3. **JSON stays on the host.** Building: lmscript's `json_stringify` through `${}` for whole bodies, plus `json_escape(s)` for a
   quoted fragment. Reading: `has_json(path)` / `json_text(path)` with dotted paths over a host-parsed tree; scalars only.
   The DSL has no JSON type and does not need one for configuration.
4. **Secrets.** `env(name)` throws if unset (with the hint to test `has_env`); the key is read inside the chunk at run time, so
   neither the script, the bytecode nor the lmscript `--dry-run` PLAN contain it.
5. **Errors in three places.** Type and arity errors at lmscript check time (exit 2, located); host validation and HTTP
   failures as DSL run-time errors (exit 3, message names the method and the URL); application-level failures that the
   chunk decides on through `out.fail(...)` (exit 4). HTTP status codes are data, not errors: `res.ok()` and `res.status()`.
6. **Mirror rules.** Breadth-first, same host and port, `allow` prefixes on Content-Type (HTML and CSS here), depth counts
   link hops from the start page, every URL fetched at most once (`?query` variants are distinct URLs but map to the same file),
   output path `out_dir/host[_port]/segments` with `index.html` for directories, `..`/`.` segments neutralized and unsafe
   characters replaced, fragments dropped, `mailto:`/`javascript:`/`data:` ignored. 404 and other statuses count as failed,
   not as errors.
7. **Client limits.** `timeout_ms` applies to connect, send and each receive; `max_bytes` refuses a declared Content-Length
   above the limit before reading and aborts a chunked or unbounded body that grows past it; `max_redirects` hops, the last 3xx
   returned as-is when the limit is reached (curl without `-L`); 303 and 301/302 on POST switch to GET.
8. **TLS.** OpenSSL is used only when `/usr/include/openssl/ssl.h` and `libssl.so` exist at build time (they do here: TLS yes);
   otherwise the host is http-only and says so when an https URL is used. Certificate verification is on (system store, SNI,
   host name check). No live TLS request is part of the test.

## Deliberately missing

HTTP keep-alive, HTTP/2, proxies, cookies, compression, multipart, streaming responses, request bodies from files, a JSON
value type in the DSL, per-link callbacks in the mirror (R13 says configure, not loop), a Rust host.

## Lessons for the real design

- The second and third POC each forced exactly one small language addition (floats and arithmetic; string concatenation and
  `str()`), and each came from a single line that lmscript could not pre-compute (`float(x.len() - 1)`, `"Bearer " + env(KEY)`).
  The chunk-as-configuration model (R13) held: three domains, zero loops.
- Builder objects with typed setters are the schema's answer to optional arguments and lists, at the price of a method per option.
  A schema shorthand for "setter returning self" would remove a lot of repetition.
- `dslvm.hpp` is now copied three times; a shared `cpp-dsl-sdk` repository is clearly needed before lmor (not created here, the
  operator decides the repo).
- Host-side validation is where most real errors live (bad verb, header injection, depth range, unset env, HTTP failures); the
  exit-code scheme 2/3/4 separated compiler, host and application failures cleanly, but a structured error channel back to
  lmscript is still missing.
