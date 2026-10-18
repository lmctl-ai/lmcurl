# Gotchas and limits

- **Header pin:** `cpp/dslvm.hpp` must match `sdk.lock` (cpp-dsl-sdk 2b28058, bytecode v3); refresh with `scripts/sync-sdk.sh`;
  `test.sh` fails on drift.
- **TLS depends on the build machine.** No OpenSSL dev files -> http-only binary; the test prints `TLS via OpenSSL: yes|no`. No live
  TLS request is part of the test; https was compiled but never exercised against a real server here.
- **Redirect semantics:** reaching `max_redirects` returns the last 3xx instead of failing (changed on 2026-10-18 after the first
  probe); a redirect loop therefore ends with a 3xx response, not an error.
- **Relative URL resolution is RFC 3986-lite** (`http::resolve`): root-relative, `./`, `../`, query-only, scheme-relative; it had a
  double-slash bug on 2026-10-18 fixed before slice 2; `tools`-less verification table is in the slice-2 commit message.
- **The mirror writes under `out_dir/host[_port]/`;** a path from a URL never escapes `out_dir` (segments sanitized), checked by the test.
- **`Output.print` goes to stdout, host diagnostics to stderr;** exit codes 0/1/3/4 (see decisions). `loaded ...`/`done ...` lines are
  stderr.
- **Mock dependence:** the whole test runs against the local node mock; nothing talks to the internet unless the live flag is set.
- **lmscript version:** needs >= 0.2.328; an older PATH binary is refused by test.sh. **Shared box:** one test at a time, `nice`.
