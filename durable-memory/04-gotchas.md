# Gotchas

- Bytecode is bound to exact schema bytes; changing whitespace in a schema changes its hash.
- Handles are opaque and host-owned for one run.
- The language and VM impose no time or memory limits; hosts may enforce domain policy around their own APIs.
- The released CLI and the runtime must agree on bytecode version.
- OpenSSL is optional and dynamically linked only in the HTTP client host; see `THIRD_PARTY_NOTICES`.
