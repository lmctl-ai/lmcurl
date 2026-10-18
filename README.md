# poc-curl: a curl-like C++ HTTP host driven by the DSL

Status: EXPERIMENTAL proof of concept for the lmscript `_dsl_` framework (lmctl-mono `lmscript/durable-memory/requirements/dsl-bytecode.md`
R12-R16, R20, R22; living language spec `lmscript/durable-memory/design/dsl-language.md`). Nothing here is an accepted design.
Slice 1: host, mock server and the POST example. The full write-up follows in the last slice.

**lmscript build needed.** `_dsl_` and `savedsl` are not in the npm staging build 0.2.311. Use a mono build:
`LMSCRIPT_BIN=~/repos/lmctlhq/lmctl-mono/lmscript/bin/lmscript` (`npm run build` in `lmctl-mono/lmscript`).

    make                                                        # build/curlhost (C++17, POSIX sockets, OpenSSL when its dev files exist)
    node tools/mock-server.mjs 8080 &                           # local deterministic endpoints
    $LMSCRIPT_BIN examples/post.lms build/post.dslb http://127.0.0.1:8080/echo
    build/curlhost --dsl build/post.dslb --schema schema/curl.dsl.json
