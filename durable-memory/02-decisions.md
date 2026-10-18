# Decisions (dated, with reasons; details in README "Decisions")

- **2026-10-18, language v3 adds only string concatenation and `str(x)`.** Nothing implicit: `"a" + 1` and `str("x")` are errors with
  hints. Not added: lists, maps, optional arguments, loops, functions (R13 minimum).
- **2026-10-18, builder objects instead of optional arguments:** `request().method(..).url(..).header(k, v).send()`; repeated
  `header`/`allow` calls replace lists; validation in the host method (unknown verb, CR/LF in a header, depth 0..10) surfaces as a DSL
  run-time error, exit 3.
- **2026-10-18, JSON stays on the host:** bodies via `json_stringify` through `${}`, fragments via `json_escape(s)`; reading via
  `has_json(path)` / `json_text(path)` with dotted paths over a host-parsed tree; scalars only. The DSL has no JSON type.
- **2026-10-18, secrets:** `env(name)` throws if unset (hint: `has_env`); read inside the chunk at run time, so neither the script,
  the bytecode nor the lmscript `--dry-run` PLAN contain the key; the test greps the bytecode for the key.
- **2026-10-18, errors in three places and three exit codes:** compiler (2, located), host validation and HTTP transport failures
  (3), application failures the chunk decides on with `out.fail` (4). HTTP statuses are data (`ok()`, `status()`), not errors.
- **2026-10-18, mirror rules:** breadth-first, same host and port, content-type prefixes (`allow`), depth = link hops, each URL once
  (`?query` variants are distinct URLs mapping to the same file), `out_dir/host[_port]/segments` with `index.html` for directories,
  `..`/`.` neutralized, fragments dropped, `mailto:`/`javascript:`/`data:` ignored; 404 counts as failed, not as an error.
- **2026-10-18, client limits:** `timeout_ms` for connect/send/each receive; `max_bytes` refuses a declared Content-Length above the
  limit before reading and aborts a growing chunked body; `max_redirects` hops, the last 3xx returned as-is at the limit (curl without
  `-L`); 303 and 301/302 on POST switch to GET.
- **2026-10-18, TLS only when OpenSSL dev files exist at build time** (`/usr/include/openssl/ssl.h` + `libssl.so`; present on this box,
  `-DHAVE_OPENSSL`); otherwise http-only with a clear message on https URLs. Verification on (system store, SNI, host name check).
