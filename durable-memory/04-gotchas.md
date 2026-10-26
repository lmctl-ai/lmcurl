# Operational gotchas

Schema changes alter bytecode compatibility and require regenerating signatures and references. `lmctl autorun` must execute saved bytecode and write images under the current directory; examples therefore contain their own `savedsl`, `system ${dsl_cli(...)}` and renderer blocks.

The live LLM path is opt-in and should use tiny prompts/free or local models. Cost is reserved before transmission, settled from provider usage when available, and never logs Authorization headers or key-looking values. A failed provider call may still consume a reserved estimate.

OpenRouter and DeepSeek are OpenAI-compatible; Ollama uses its native chat endpoint. Streaming is bounded response buffering rather than a token callback. JSON mode, sampling and matrix outputs use built-in mock fixtures or cassettes by default. Public examples do not depend on live provider availability.

curl runs configure-time link probes against ZLIB::ZLIB. Keep zlib as a separately built, pinned static archive exposed through a real imported target: an alias of a not-yet-built target fails in CMake scratch projects on a fresh checkout. Configure/build failures print bounded diagnostics in test.sh.

OpenSSL must receive `--libdir=lib64` explicitly. Its upstream default is lib64 on Linux x64 and lib on ARM64; the imported archive paths intentionally use the same layout on both.

New examples need lmcurl >=0.1.1 for Mock.serve. Use opaque `_dsl_` values with savedsl/system; strings are not executable DSL. Variables do not cross native processes: parser exports must be read back by outer lmscript. `${value}` already emits a typed DSL literal and must not be wrapped in extra quotes.

lmhtml v0.7.3 is an HTML5 parser. The XML recipes select unprefixed elements; RSS uses permalink GUIDs because HTML treats link as a void element. Atom URLs come from link href. Table-to-CSV and field handoff are example glue, not new lmhtml APIs. Robots handling intentionally covers the fixture subset (wildcard agent, Disallow prefixes and Crawl-delay), not complete RFC policy.
