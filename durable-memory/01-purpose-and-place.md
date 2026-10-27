# Purpose and architecture

lmcurl is a public MIT C++ host for lmscript `_dsl_` programs. It embeds pinned upstream libcurl and exposes Curl/Session/Request/Response objects, a local mock server, typed Query and Suite/Test/Report objects, bounded load checks and an opt-in LLM layer for OpenRouter, DeepSeek and Ollama. The result handle is named `Result` in the LLM schema.

The runtime owns sockets, cookie jars, variables, response limits, retries, timing data, reports and redaction. The DSL has no collection bytecode values; host objects provide typed accessors. Schema bytes bind programs to the registered API. The memory guard is `LMCURL_MAX_MEMORY_MB`, default 4096 MiB, with request/response/parser/object caps layered above it.

Live provider calls require `LMCURL_LIVE=1`; credentials remain environment-only and are never printed. Cassettes keep public examples offline. LLM cost reservations default to $0.02 per example and cannot exceed $0.25 per process.

Downloader/parser recipes keep the mock site in one bounded lmcurl process while other lmcurl processes download files and lmhtml processes inspect them. Cross-host values travel through caller-selected CSV/text/JSON files. The 259-line registry adds Mock.serve and Request.allow_error without removing existing signatures.
