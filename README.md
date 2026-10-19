# lmcurl

`lmcurl` is a small C++ HTTP client host driven by an lmscript `_dsl_` chunk. The
host owns network access, credentials, redirects, response limits, and mirror output;
the schema exposes typed request and mirror builders.

This is an experimental public bytecode v5 example. Install the released compiler:

```sh
npm install -g @lmctl-ai/lmctl@0.2.347
```

Run the local mock-server example and its checks:

```sh
make
bash test.sh
```

The test uses `lmscript` from `PATH` and requires version 0.2.346 or newer. It runs
POST, JSON, redirect, chunked-response, mirror, timeout, size-limit, schema-binding,
and located runtime-error cases. Live LLM calls are optional and disabled by default.

`schema/curl.dsl.json` is schema 2. It declares `Request`, `Response`, `Mirror`,
`MirrorResult`, and `Output`, plus the `request`, `mirror`, environment, and escaping
functions. Typed signature arrays, `returns: "self"`, and host-owned handles keep
configuration explicit while the compiler resolves every call before execution.

The DSL supports typed locals, branching, loops, compiler-inlined functions, string
operations, and source locations in runtime errors. It has no bytecode collection
values; the host owns any collection and exposes methods for traversal.

The C++ runtime is vendored as `cpp/dslvm.hpp` and checked against `sdk.lock`. When
available, the Makefile dynamically links the system OpenSSL 3 libraries for TLS;
OpenSSL is not bundled. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Related public projects are [cpp-dsl-sdk](https://github.com/lmctl-ai/cpp-dsl-sdk),
[rust-dsl-sdk](https://github.com/lmctl-ai/rust-dsl-sdk),
[lmapi](https://github.com/lmctl-ai/lmapi), [lmrust](https://github.com/lmctl-ai/lmrust),
and [lmcpp](https://github.com/lmctl-ai/lmcpp).

This repository is released under the MIT License. See [LICENSE](LICENSE).
