# Decisions and safety

The HTTP transport is upstream libcurl pinned by lock/hash and built statically with permissive dependencies. Linux permits only dynamic glibc; no GPL/LGPL transport is linked. A loopback mock implements JSON, echo, status, redirects, cookies, login/protected, flaky/poll, delay, SSE and LLM-shaped responses.

Responses are bounded at 8 MiB, headers at 64 KiB and sessions/objects are capped. XML/XPath uses libxml2 with NONET/no-warning parsing; external entities and network fetches are disabled. Query regexes and paths have explicit size/depth limits. Output files are caller-selected and redacted before writing.

Reports are console/TAP/JUnit/JSON/HTML. Test setup and teardown requests run around the suite; fail-fast, tag filtering and parallel request execution are explicit settings. Load checks cap virtual users, duration and request count.

Regex assertions use the Linux POSIX ERE automaton with no backreferences/lookarounds, 8192 text bytes and 256 pattern bytes. Common \d/\w/\s escapes are translated. HTTP redirects are checked hop by hop, strip credentials across origins, and share one deadline; LLM redirects cannot leave their configured origin. Global memory charges use atomic reservations, including allocator rounding.

Mock.serve atomically publishes its URL, watches a stop file and enforces 1..120000 ms. The site fixtures cover pages, real absolute sitemap/feed URLs, CSRF/login cookies, counters, pagination, assets and audit findings. Unknown site paths return 404. Cookie authentication compares parsed opaque names/values, with no substring matching or form-style URL decoding. Explicit Request.proxy("") disables environment proxies; an unset proxy retains libcurl defaults.
