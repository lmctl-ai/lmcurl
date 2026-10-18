# poc-curl durable memory

What this repo is: the third proof-of-concept host of the lmscript `_dsl_` framework: a curl-like C++ HTTP client where one
self-contained lmscript file does a POST, an LLM API call (key from the host environment) or a recursive static-site mirror configured
by the chunk. It forced DSL spec v3 (string concatenation, `str()`). Status (2026-10-18): EXPERIMENTAL, green (`test.sh` 24 checks on
lmscript 0.2.328 and 0.2.330); private repo `lmctlhq/poc-curl`. Operator requirement R22 (and R12, R13, R15, R20).

Team guide: lmctl-mono `lmscript/durable-memory/dsl-framework.md`. Requirements and findings: lmctl-mono
`lmscript/durable-memory/requirements/dsl-bytecode.md`. README.md holds the transcripts, the process-count table and the decisions.

| File | Fact |
| --- | --- |
| [01-purpose-and-place.md](01-purpose-and-place.md) | the three examples, what each proved, R20 process count |
| [02-decisions.md](02-decisions.md) | dated decisions: v3 additions, builders over optional args, JSON on the host, secrets, exit codes, mirror rules, client limits, TLS |
| [03-build-run-test.md](03-build-run-test.md) | commands, mock server, exact expectations, the opt-in live call |
| [04-gotchas.md](04-gotchas.md) | header pin, TLS availability, redirect semantics, mock dependence, shared box |
| [05-open-items.md](05-open-items.md) | next steps |
