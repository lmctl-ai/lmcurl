# Invocation envelope and run_lmcode (2026-11-04)

The current compiler (lmscript 0.2.460) has no CLI bytecode files. A script runs a block with r = run_lmcode(p): lmscript compiles it into a private program file, starts lmcurl --lmcode FILE and reads one terminal frame back. r is that envelope (r.ok, r.result, r.phase, r.code, r.message), so a failure is data and a script must check r.ok. Host output is not in r.

Host side (cpp/main.cpp, SDK lmcode-cpp 93bddf1): --lmcode builds one InvocationResults. Every file loads first; each slot is selected, run and completed in order; the first failure stops the run. Exit codes: 0 success; 2 cli (usage, invalid metadata, bad LMCURL_MAX_MEMORY_MB or LMCURL_MAX_COST_USD); 3 load or run, including the memory budget and an unreadable program file; 4 application or output failure.

A report that fails assert_all is an application failure: phase app, code TEST_FAILED, exit 4. It used to be exit 1 with phase test, which the protocol does not have. It is checked after each program, so it stops later programs of the invocation; statements after assert_all in the same program still run.

The frame is host output, so it is redacted like every other: emit_frame passes the payload through Runtime::redact, and failure messages are redacted before they are recorded. A JSON payload comes back from redact with sorted keys and sensitive keys masked.

A 1 MiB reserve is released before each failure is recorded, so a memory budget failure cannot throw again inside a catch handler.

Dry mode: metadata from run_lmcode under lmscript --dry-run, or the --dry-run flag without metadata. A dry invocation loads every program and runs none, because a request has no effect-free form. The flag contradicting real metadata is a cli error.

The stub for macOS and Windows has no SDK and writes no frame: execution stays exit 3 with "not supported" on stderr.

Tests: examples and tests/*.lms run through run_lmcode. Tests that need the raw host (exit codes, stderr, Popen, ulimit) take the private program file with scripts/lmcode_capture.py: lmscript --dry-run with an explicit command, scripts/record-program.py, which keeps a copy and runs the host. Other hosts bind only when a compatible one is already on PATH (it answers --lmcode-schema --json); nothing installs a renderer.
