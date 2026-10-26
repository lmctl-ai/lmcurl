# v0.1.0 review

Final independent follow-up: no blocking findings; approved for the reviewed code.

An independent source review identified and resolved these findings before tagging:

- All LLM JSON entry points now enforce the shared 8 MiB/128-level parser limits; deep tool schemas fail cleanly.
- Parallel memory charges reserve with CAS and account for allocator rounding; the concurrent regression holds reservations until every thread has attempted a charge.
- Redirects are followed manually with destination cost guards, credential stripping, five-hop cap and one deadline. LLM redirects cannot leave their provider origin.
- Outer exception messages pass through the redactor.
- Assertions and query filters use bounded POSIX ERE without backreferences, avoiding recursive std::regex backtracking.
- Streamed tool-call deltas assemble by bounded index, retaining split names, IDs and arguments.

Local gate: four DSL programs, 28 native scenarios, concurrent memory regression, default 4096 MiB rejection, exact schema/notices/signatures, linkage allowlist and 32 regenerated images. Artifact scans found no credential values. Live tests are separate; see ../evidence/live-llm-v010.json.

macOS/Windows execute metadata stubs only. HTTP streaming is buffered within the response cap, and OpenAPI validation covers the documented JSON response subset.

# v0.1.1 follow-up

The 16 downloader/parser stories were inspected independently; no blocking findings remained. Metrics are derived from real loopback responses and lmhtml parsing. Fixed parallel lists cover independent downloads; timeout/body caps, stop-file/deadline cleanup, same-origin route checks, explicit proxy bypass and zero-cost ledgers bound the examples.

Additional regressions cover atomic mock readiness, independent closed-port checks after stop/deadline, unknown-site 404, CSRF rejection/login/account state, exact cookie names and opaque values, pagination bounds, and explicit proxy("") behavior under dead environment proxies. The empty-proxy regression failed on the old implementation and passes with the explicit override flag.

Published-dependency check found lmhtml v0.7.3 missing the implicit standard file catalog (LOAD_UNBOUND), despite the local main build exposing it under the same version. lmhtml v0.7.4 publishes the SDK migration; generated examples reject older versions before mock startup. Full local validation was repeated with the patched parser and pinned compiler.
