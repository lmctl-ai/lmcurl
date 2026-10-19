# Purpose and place

- **Fact (2026-10-18):** poc-curl answers R22: "a curl-like C++ HTTP host driven by the DSL; the immediate benefit of lmscript: ONE
  self-contained script file does a POST". Three chunks: `examples/post.lms` (JSON body built by lmscript's `json_stringify`, handed
  over as a `${}` literal; headers, timeout, send, print), `examples/llm.lms` (OpenAI-style chat completion; URL/model/prompt as `${}`
  literals; the API key read inside the chunk with `env("POC_CURL_KEY")`, so it is in neither the script nor the bytecode; the reply
  extracted by the host with `json_text("choices.0.message.content")`), `examples/mirror.lms` (the chunk configures a host crawler:
  start, depth, same host, allowed content types, output dir, limits; the host crawls).
- **Fact:** this host forced string `+` concatenation and `str(int|float|bool)` in spec v3 (2026-10-18); the host now runs the v4
  bytecode format with those language features retained, from the one line lmscript
  cannot pre-build, `"Bearer " + env(KEY)`. Builder chains needed nothing new (methods returning their own type).
- **Fact (R20, measured by counting):** POST = 1 host process instead of 1 curl plus shell quoting (or a body file from another
  process); LLM call with extraction = 1 process instead of curl + jq; mirror of 9 pages = 1 process instead of `wget -r` or 9+
  curls. The lmscript compile is a separate run whose output file is reusable with different environments.
- **Place:** read after poc-numpy; shows strings, builders, secrets and host-owned iteration (the crawler) under R13 "configure, do not loop".
